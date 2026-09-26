using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Consumes the same validated flow as the Semantic reader. No second source
// interpretation is allowed here: writes and retiring owners come from that plan.
internal static class CSharpTaskLocalLifetimes
{
    public static GuestTaskLocalLifetimePlan BuildPlan(SemanticDocument document,
        IReadOnlyList<GuestFunction> functions, string exceptionModel)
    {
        var entries = new List<GuestTaskLocalLifetimeFunction>();
        foreach (var method in document.AsyncMethods.Where(method => method.TaskLocalLifetimes is not null))
        {
            var ids = method.Segments.Where(segment => segment.AwaitSite is not null)
                .Select(segment => CSharpGuestIds.AsyncResumeFunction(segment.AwaitSite!.CallbackId))
                .Append(CSharpGuestIds.Function(method.MethodSymbolId)).ToHashSet(StringComparer.Ordinal);
            var owners = method.TaskLocalLifetimes!.Select(local => CSharpGuestIds.Local(local.SymbolId))
                .OrderBy(id => id, StringComparer.Ordinal).ToArray();
            foreach (var function in functions.Where(function => ids.Contains(function.Id)))
            {
                var releases = new List<GuestTaskLocalReleaseSite>();
                foreach (var block in function.Blocks)
                for (int index = 0; index < block.Instructions.Count; ++index)
                {
                    var call = block.Instructions[index];
                    if (call.Op != "call" || call.TargetId != CSharpTaskOwnerGuards.ReleaseFunctionId) continue;
                    // The IR reader verifies these anchors against the full
                    // canonical load/release/clear sequence, including types.
                    releases.Add(new(block.Id, index >= 2 ? block.Instructions[index - 2].TargetId ?? "" : "",
                        call.OperandIds.Count == 1 ? call.OperandIds[0] : "",
                        index + 2 < block.Instructions.Count ? block.Instructions[index + 2].ResultId ?? "" : ""));
                }
                var exits = function.Blocks.Where(block => block.Id.Contains(":$task:exit:", StringComparison.Ordinal))
                    .SelectMany(exit => function.Blocks.Where(source => source.Terminator.TargetBlockId == exit.Id
                        || source.Terminator.FalseTargetBlockId == exit.Id).Select(source =>
                            new GuestTaskLocalScopeExit(source.Id, exit.Id, exit.Terminator.TargetBlockId ?? "")))
                    .OrderBy(exit => exit.SourceBlockId, StringComparer.Ordinal)
                    .ThenBy(exit => exit.ExitBlockId, StringComparer.Ordinal).ToArray();
                entries.Add(new(function.Id, owners, releases, exits));
            }
        }
        return new(exceptionModel, entries.OrderBy(entry => entry.FunctionId, StringComparer.Ordinal).ToArray());
    }

    public static bool LowerWrite(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        SemanticAsyncSegment segment, int statementIndex, List<GuestInstruction> instructions, out bool handled)
    {
        handled = false;
        if (method.TaskLocalLifetimes is null) return true;
        if (!SemanticAsyncTaskLocalLifetimeValidator.TryAnalyze(method, out var flow)) return false;
        var write = flow!.WritesBySegment[segment.Ordinal].SingleOrDefault(item => item.StatementIndex == statementIndex);
        if (write is null) return true;
        handled = true;
        if (write.Value is null) return Clear(context, write.SymbolId, segment.Ordinal, instructions);
        // Evaluate before modifying ownership: a failing producer cannot discard
        // the old target. Aliases retain the evaluated token before releasing it.
        var value = CSharpOperationLowerer.LowerValue(context, write.Value, segment.Ordinal, instructions);
        if (value is null) return false;
        if (write.Value.Kind == "local_reference")
        {
            var token = context.CreateTemporary(CSharpTaskResultAbi.TokenTypeId, segment.Ordinal);
            if (token is null) return false;
            instructions.Add(new("convert", token.Id, new[] { value.Id }, null, null, null));
            if (!CSharpTaskOwnerGuards.Call(context, method, CSharpTaskResultAbi.Retain,
                    token, segment.Ordinal, instructions)) return false;
        }
        return ReleaseAndClear(context, method, write.SymbolId, segment.Ordinal, instructions)
            && CSharpOperationLowerer.StoreLocal(context, write.SymbolId, value, segment.Ordinal, instructions);
    }

    public static bool Clear(CSharpFunctionLoweringContext context, string symbol,
        int block, List<GuestInstruction> instructions)
    {
        if (!context.TryGetStorage(symbol, out var storage)) return false;
        var zero = CSharpTaskResultAbi.Constant(context, CSharpTaskResultAbi.TokenTypeId, 0, block, instructions);
        var value = context.CreateTemporary(storage.TypeId, block);
        if (zero is null || value is null) return false;
        instructions.Add(new("convert", value.Id, new[] { zero.Id }, null, null, null));
        instructions.Add(new("local_store", null, new[] { value.Id }, storage.Id, null, null));
        return true;
    }

    public static bool ReleaseAndClear(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        string symbol, int block, List<GuestInstruction> instructions)
    {
        var token = CSharpTaskResultAbi.LoadTaskLocalToken(context, symbol, block, instructions);
        return token is not null && CSharpTaskOwnerGuards.Call(context, method, CSharpTaskResultAbi.Release,
            token, block, instructions) && Clear(context, symbol, block, instructions);
    }

    public static bool InsertEdges(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        List<GuestBasicBlock> blocks, int? incomingSegment, string entryPrefix)
    {
        if (method.TaskLocalLifetimes is null) return true;
        if (!SemanticAsyncTaskLocalLifetimeValidator.TryAnalyze(method, out var flow)) return false;
        foreach (var segment in method.Segments)
        {
            string source = CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, segment.Ordinal);
            if (!blocks.Any(block => block.Id == source)) continue;
            foreach (int target in SemanticAsyncScopeValidator.Targets(segment.Transfer!).Distinct())
            {
                if (!Bridge(segment.Ordinal, target, source, false)) return false;
            }
        }
        if (incomingSegment is { } incoming)
            foreach (int target in SemanticAsyncScopeValidator.Targets(method.Segments[incoming].Transfer!).Distinct())
                if (!Bridge(incoming, target, entryPrefix, true)) return false;
        return true;

        bool Bridge(int sourceOrdinal, int targetOrdinal, string prefix, bool resumed)
        {
            if (!flow!.States.ReleasedOnEdge.TryGetValue(new(sourceOrdinal, targetOrdinal), out var owners)) return false;
            if (owners.Count == 0) return true;
            string target = CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, targetOrdinal);
            bool HasPrefix(string id, string parent) => id == parent || id.StartsWith(parent + ":", StringComparison.Ordinal);
            int[] predecessors = Enumerable.Range(0, blocks.Count).Where(index =>
                HasPrefix(blocks[index].Id, prefix) && (resumed || !HasPrefix(blocks[index].Id, entryPrefix))
                && (blocks[index].Terminator.TargetBlockId == target || blocks[index].Terminator.FalseTargetBlockId == target)).ToArray();
            // Direct awaits have no synchronous successor; their callback entry
            // below carries this edge. Other reachable edges must be present.
            if (predecessors.Length == 0) return !resumed
                && method.Segments[sourceOrdinal].Transfer!.Kind == SemanticAsyncMethod.AwaitTransferKind;
            string bridge = prefix + ":$task:exit:" + targetOrdinal;
            List<GuestInstruction> instructions = new();
            foreach (string owner in owners)
                if (!ReleaseAndClear(context, method, owner, sourceOrdinal, instructions)) return false;
            foreach (int index in predecessors)
            {
                var previous = blocks[index].Terminator;
                blocks[index] = blocks[index] with { Terminator = previous with {
                    TargetBlockId = previous.TargetBlockId == target ? bridge : previous.TargetBlockId,
                    FalseTargetBlockId = previous.FalseTargetBlockId == target ? bridge : previous.FalseTargetBlockId } };
            }
            blocks.Add(new(bridge, instructions, new("branch", null, target, null, null)));
            return true;
        }
    }
}
