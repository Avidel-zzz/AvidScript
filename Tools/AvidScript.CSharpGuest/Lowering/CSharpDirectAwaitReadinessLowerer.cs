using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpDirectAwaitReadinessLowerer
{
    internal static bool NeedsGuard(SemanticDocument document, SemanticAsyncMethod method, SemanticAsyncSegment segment) =>
        segment.AwaitSite?.CancellationToken is not null && CSharpTaskResultAbi.SupportsCancellation(document)
        && CSharpAsyncCancellationLowerer.IsStatusAware(document, method, segment);

    // Producer arguments and the source token are already evaluated once. Split
    // immediately before the scheduling call, leaving all host effects in the open arm.
    internal static bool Emit(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        SemanticAsyncSegment segment, GuestRegister scheduled, string? bindImport,
        List<GuestBasicBlock> blocks, ref string activeBlock, ref List<GuestInstruction> instructions)
    {
        var site = segment.AwaitSite!;
        int scheduleIndex = instructions.FindIndex(instruction => instruction.Op == "call" && instruction.ResultId == scheduled.Id);
        if (scheduleIndex < 0) return false;
        var binding = instructions.Skip(scheduleIndex).Where(instruction => instruction.Op == "call"
            && instruction.TargetId == bindImport).ToArray();
        if (binding.Length != 1 || binding[0].OperandIds.Count != 2 || binding[0].OperandIds[1] != scheduled.Id) return false;
        var ready = instructions.Skip(scheduleIndex).ToList();
        var check = instructions.Take(scheduleIndex).ToList();
        var status = context.CreateTemporary("type:int32", segment.Ordinal);
        var open = context.CreateTemporary("type:int32", segment.Ordinal);
        var cancelled = context.CreateTemporary("type:int32", segment.Ordinal);
        if (status is null || open is null || cancelled is null) return false;
        check.Add(new("call", status.Id, new[] { binding[0].OperandIds[0] }, GuestDirectAwaitReadiness.ImportId, null, null));
        var one = CSharpTaskResultAbi.Constant(context, "type:int32", 1, segment.Ordinal, check);
        if (one is null) return false;
        check.Add(new("binary", open.Id, new[] { status.Id, one.Id }, null, "equals", null));
        string scheduleBlock = activeBlock + ":readiness:schedule";
        string cancelCheck = activeBlock + ":readiness:cancel_check";
        string cancelBlock = activeBlock + ":readiness:cancel";
        string invalid = activeBlock + ":readiness:invalid";
        blocks.Add(new(activeBlock, check, new("branch_if", open.Id, scheduleBlock, cancelCheck, null)));
        List<GuestInstruction> cancellationCheck = new();
        var two = CSharpTaskResultAbi.Constant(context, "type:int32", 2, segment.Ordinal, cancellationCheck);
        if (two is null) return false;
        cancellationCheck.Add(new("binary", cancelled.Id, new[] { status.Id, two.Id }, null, "equals", null));
        blocks.Add(new(cancelCheck, cancellationCheck, new("branch_if", cancelled.Id, cancelBlock, invalid, null)));
        blocks.Add(new(invalid, Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)));
        string target = segment.Transfer!.CancellationTarget is int ordinal
            ? CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, ordinal)
            : CSharpAsyncCancellationLowerer.ImplicitPropagationBlock(method, site);
        if (!CSharpAsyncCancellationLowerer.EmitDirect(context, method, site, segment.Ordinal,
                cancelBlock, target, new List<GuestInstruction>(), blocks)) return false;
        activeBlock = scheduleBlock;
        instructions = ready;
        return true;
    }

    internal static bool TryWrap(GuestModule input, out GuestModule module, out string? error)
    {
        module = input;
        error = null;
        if (!input.Functions.SelectMany(function => function.Blocks).Any(block => block.Instructions
                .Any(instruction => instruction.Op == "call" && instruction.TargetId == GuestDirectAwaitReadiness.ImportId))) return true;
        var source = GuestDirectAwaitReadiness.BaseProfile(input);
        List<GuestDirectAwaitReadinessGuard> guards = new();
        foreach (var function in source.Functions)
        foreach (var check in function.Blocks.Where(block => block.Instructions.Any(instruction =>
            instruction.Op == "call" && instruction.TargetId == GuestDirectAwaitReadiness.ImportId)))
        {
            var routes = source.DirectAwaitRoutes?.Where(route => route.AwaitBlockId == check.Terminator.TargetBlockId).ToArray();
            var next = function.Blocks.Where(block => block.Id == check.Terminator.FalseTargetBlockId).ToArray();
            if (routes is not { Length: 1 } || next.Length != 1 || next[0].Terminator.TargetBlockId is not { } cancel)
            {
                error = "Pre-cancel query has no unique scheduling and cancellation route: " + function.Id + "/" + check.Id;
                return false;
            }
            guards.Add(new(function.Id, routes[0].CallbackId, check.Id, routes[0].AwaitBlockId, cancel));
        }
        var imports = source.Imports;
        if (!imports.Any(import => import.Id == GuestDirectAwaitReadiness.ImportId))
            imports = imports.Append(new GuestImport(GuestDirectAwaitReadiness.ImportId, "avidscript",
                GuestDirectAwaitReadiness.ImportName, new[] { "type:int64" }, "type:int32")).ToArray();
        module = source with
        {
            SchemaVersion = GuestDirectAwaitReadiness.SchemaVersion,
            IrVersion = GuestDirectAwaitReadiness.IrVersion,
            Imports = imports,
            DirectAwaitReadiness = new(source.SchemaVersion, source.IrVersion, guards
                .OrderBy(guard => guard.FunctionId, StringComparer.Ordinal).ThenBy(guard => guard.CallbackId).ToArray()),
        };
        return true;
    }
}
