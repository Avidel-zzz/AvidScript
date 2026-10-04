using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpAsyncClosureAllocations
{
    public static void Transition(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        int? source, int destination, List<GuestInstruction> instructions)
    {
        var owned = context.Document.ClosureEnvironments.Where(environment => environment.OwnerMethodSymbolId == method.MethodSymbolId)
            .ToDictionary(environment => environment.Id, StringComparer.Ordinal);
        foreach (SemanticAsyncLexicalScope scope in method.LexicalScopes.Where(scope => owned.ContainsKey(scope.Id))
                     .OrderBy(scope => scope.Span.Length))
            if (source is { } previous && scope.Segments.Contains(previous) && !scope.Segments.Contains(destination))
                context.ClosureCells.Clear(owned[scope.Id], instructions);
        foreach (SemanticAsyncLexicalScope scope in method.LexicalScopes.Where(scope => owned.ContainsKey(scope.Id))
                     .OrderByDescending(scope => scope.Span.Length))
            if (scope.Entries.Contains(new(source, destination))) context.ClosureCells.Allocate(owned[scope.Id], instructions);
    }

    public static bool InsertEdges(CSharpFunctionLoweringContext context, SemanticAsyncMethod method, List<GuestBasicBlock> blocks)
    {
        foreach (SemanticAsyncSegment segment in method.Segments.Where(segment => segment.Transfer!.Kind is
                     SemanticAsyncMethod.GotoTransferKind or SemanticAsyncMethod.BranchTransferKind))
        {
            string source = CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, segment.Ordinal);
            if (!blocks.Any(block => block.Id == source)) continue; // Different continuation entry's region.
            foreach (int targetOrdinal in SemanticAsyncScopeValidator.Successors(segment).Distinct())
            {
                List<GuestInstruction> instructions = new();
                Transition(context, method, segment.Ordinal, targetOrdinal, instructions);
                if (instructions.Count == 0) continue;
                string target = CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, targetOrdinal);
                int index = blocks.FindIndex(block => (block.Id == source || block.Id.StartsWith(source + ":", StringComparison.Ordinal))
                    // Resume entry status/cancellation dispatch is a distinct
                    // edge with its own lexical transition, not the source CFG.
                    && !block.Id.StartsWith(source + ":entry", StringComparison.Ordinal)
                    && (block.Terminator.TargetBlockId == target || block.Terminator.FalseTargetBlockId == target));
                if (index < 0)
                {
                    // A projected fault target can exist without a realized
                    // outcome call in this continuation. Resume cancellation
                    // already performs its own transition before dispatch.
                    if (segment.SynchronousExceptionTarget == targetOrdinal
                        && targetOrdinal != segment.Transfer!.PrimaryTarget
                        && targetOrdinal != segment.Transfer.SecondaryTarget) continue;
                    context.Add("ASCG1024", $"Async closure scope lost its synchronous CFG edge {segment.Ordinal}->{targetOrdinal}.");
                    return false;
                }
                string bridge = source + ":$closure:edge:" + targetOrdinal;
                GuestTerminator previous = blocks[index].Terminator;
                blocks[index] = blocks[index] with { Terminator = previous with {
                    TargetBlockId = previous.TargetBlockId == target ? bridge : previous.TargetBlockId,
                    FalseTargetBlockId = previous.FalseTargetBlockId == target ? bridge : previous.FalseTargetBlockId } };
                blocks.Add(new(bridge, instructions, new("branch", null, target, null, null)));
            }
        }
        return true;
    }
}
