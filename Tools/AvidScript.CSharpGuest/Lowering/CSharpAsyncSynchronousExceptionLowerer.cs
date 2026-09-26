using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpAsyncSynchronousExceptionLowerer
{
    internal static bool Rewrite(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        string functionId, List<GuestBasicBlock> blocks)
    {
        var execution = CSharpAsyncSynchronousExecutionContext.Find(context.Document);
        if (execution is null) return true;
        var segments = method.Segments.Select(segment => (Segment: segment,
            Prefix: CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, segment.Ordinal)))
            .OrderByDescending(item => item.Prefix.Length).ToArray();
        List<GuestBasicBlock> rewritten = new();
        foreach (var source in blocks)
        {
            string active = source.Id;
            List<GuestInstruction> instructions = new();
            foreach (var instruction in source.Instructions)
            {
                if (instruction.Op != "call" || instruction.TargetId is not { } callee
                    || !execution.OutcomeValues.TryGetValue(callee, out var valueType))
                {
                    instructions.Add(instruction);
                    continue;
                }
                var segment = segments.FirstOrDefault(item => source.Id == item.Prefix
                    || source.Id.StartsWith(item.Prefix + ":", StringComparison.Ordinal)).Segment;
                if (segment?.SynchronousExceptionTarget is not int target
                    || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionSourceSlot(method), out var owner)
                    || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionTypeSlot(method), out var typeSlot))
                {
                    context.Add("ASCG1026", "An outcome call has no validated async exception target or owner.");
                    return false;
                }
                int ordinal = segment.Ordinal;
                var outcome = context.CreateTemporary(CSharpLanguageOutcomeTypes.Id(valueType), ordinal);
                var status = context.CreateTemporary("type:int32", ordinal);
                if (outcome is null || status is null) return false;
                int callIndex = instructions.Count;
                instructions.Add(instruction with { ResultId = outcome.Id });
                instructions.Add(new("field_load", status.Id, new[] { outcome.Id }, "field:status", null, null));
                string marker = active + ":synchronous_exception";
                string normal = active + ":synchronous_success";
                rewritten.Add(new(active, instructions, new("branch_if", status.Id, marker, normal, null)));
                string targetId = CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, target);
                execution.Sites.Add(new(CSharpGuestIds.Function(method.MethodSymbolId), functionId,
                    active, callIndex, targetId, owner.Id, typeSlot.Id));
                if (!EmitError(context, method, ordinal, marker, targetId, outcome, owner, typeSlot, rewritten)) return false;
                execution.FailurePublishBlocks.Add(marker + ":publish");
                active = normal;
                instructions = new();
                if (instruction.ResultId is { } result)
                    instructions.Add(new("field_load", result, new[] { outcome.Id }, "field:value", null, null));
            }
            rewritten.Add(new(active, instructions, source.Terminator));
        }
        blocks.Clear();
        blocks.AddRange(rewritten);
        return true;
    }

    private static bool EmitError(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        int ordinal, string marker, string target, GuestRegister outcome, GuestRegister owner,
        GuestRegister typeSlot, List<GuestBasicBlock> blocks)
    {
        List<GuestInstruction> create = new();
        var task = CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Create, null, null, ordinal, create);
        var zero = CSharpTaskResultAbi.Constant(context, "type:int64", 0, ordinal, create);
        var created = context.CreateTemporary("type:int32", ordinal);
        if (task is null || zero is null || created is null) return false;
        create.Add(new("binary", created.Id, new[] { task.Id, zero.Id }, null, "not_equals", null));
        string faultId = marker + ":task_created";
        string invalidId = marker + ":task_create_rejected";
        blocks.Add(new(marker, create, new("branch_if", created.Id, faultId, invalidId, null)));
        blocks.Add(new(invalidId, Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)));

        var type = context.CreateTemporary("type:int32", ordinal);
        var source = context.CreateTemporary("type:int32", ordinal);
        var root = context.CreateTemporary("type:language_error_root", ordinal);
        var accepted = context.CreateTemporary("type:int32", ordinal);
        if (type is null || source is null || root is null || accepted is null) return false;
        GuestInstruction[] fault =
        {
            new("field_load", type.Id, new[] { outcome.Id }, "field:error_type", null, null),
            new("field_load", source.Id, new[] { outcome.Id }, "field:source", null, null),
            new("field_load", root.Id, new[] { outcome.Id }, "field:error_root", null, null),
            new("call", accepted.Id, new[] { task.Id, type.Id, source.Id, root.Id }, CSharpTaskResultAbi.FaultLanguageErrorImportId, null, null),
        };
        string acquiredId = marker + ":fault_acquired";
        string rejectedId = marker + ":fault_rejected";
        blocks.Add(new(faultId, fault, new("branch_if", accepted.Id, acquiredId, rejectedId, null)));
        List<GuestInstruction> rejected = new();
        if (CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release, task, null, ordinal, rejected) is null) return false;
        blocks.Add(new(rejectedId, rejected, new("trap", null, null, null, null)));

        string active = acquiredId;
        List<GuestInstruction> replace = new();
        if (!CSharpAsyncExceptionLowerer.ReleaseIfHeld(context, method, ordinal, blocks, ref active, ref replace)) return false;
        string publishId = marker + ":publish";
        blocks.Add(new(active, replace, new("branch", null, publishId, null, null)));
        blocks.Add(new(publishId, new GuestInstruction[]
        {
            new("local_store", null, new[] { task.Id }, owner.Id, null, null),
            new("local_store", null, new[] { type.Id }, typeSlot.Id, null, null),
        }, new("branch", null, target, null, null)));
        return true;
    }
}
