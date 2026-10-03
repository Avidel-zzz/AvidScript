using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestAsyncVoidErrorOwnerValidator
{
    internal static void Validate(GuestValidationContext context)
    {
        var module = context.InputArtifact;
        if (!GuestAsyncVoidErrorOwners.IsVersion(module))
        {
            if (module.AsyncVoidErrorOwners is not null) Add("Async void owners require IR 36/1.35.");
            return;
        }
        if (!GuestAsyncVoidErrorOwners.HasSourceContract(module)
            || module.AsyncVoidErrorOwners?.Owners is not { Count: > 0 and <= 4096 } owners)
        { Add("Async void owners require the paired source contract and a bounded owner plan."); return; }
        var methods = new HashSet<string>(StringComparer.Ordinal);
        var reports = new HashSet<(string Function, string Block)>();
        string previous = "";
        foreach (var owner in owners)
        {
            var transfers = module.AsyncExceptionTransfers?.Where(transfer =>
                transfer.MethodFunctionId == owner.MethodFunctionId && transfer.Kind == "propagate_exception").ToArray();
            if (!methods.Add(owner.MethodFunctionId) || string.CompareOrdinal(previous, owner.MethodFunctionId) >= 0
                || !context.Functions.TryGetValue(owner.MethodFunctionId, out var method)
                || method.ReturnTypeId != "type:void"
                || !owner.MethodFunctionId.StartsWith("function:", StringComparison.Ordinal)
                || owner.OwnerLocalId != "value:local:$async:exception_source:" + owner.MethodFunctionId[9..]
                || owner.TypeLocalId != "value:local:$async:exception_type:" + owner.MethodFunctionId[9..]
                || transfers is not [{ } transfer] || transfer.BlockId != owner.UnhandledBlockId
                || transfer.OwnerLocalId != owner.OwnerLocalId || transfer.TypeLocalId != owner.TypeLocalId
                || owner.Reports.Count is not (> 0 and <= 4096))
            { Add("Async void owner must identify one void method and its unique unhandled transfer."); continue; }
            previous = owner.MethodFunctionId;
            string marker = owner.UnhandledBlockId + ":propagate_exception";
            string priorFunction = "";
            foreach (var report in owner.Reports)
            {
                if (report.BlockId != marker || string.CompareOrdinal(priorFunction, report.FunctionId) >= 0
                    || !reports.Add((report.FunctionId, report.BlockId))
                    || !context.Functions.TryGetValue(report.FunctionId, out var function)
                    || !Matches(GuestTaskLocalLifetimeValidator.ResolveScopeExitRoutes(module, function), transfer))
                    Add("Async void report must read the held root, check reporting and then release its owner.");
                priorFunction = report.FunctionId;
            }
            foreach (var function in module.Functions.Where(function => function.Blocks.Any(block => block.Id == marker)))
                if (!reports.Contains((function.Id, marker))) Add("Unlisted async void error report.");
        }
        foreach (var function in module.Functions)
        foreach (var block in function.Blocks.Where(block => block.Instructions.Any(instruction =>
            instruction.Op == "call" && instruction.TargetId == GuestAsyncVoidErrorOwners.ReportImportId)))
        {
            // Other Task error reports have their own independent legacy proof.
            if ((block.Id.EndsWith(":propagate_exception", StringComparison.Ordinal)
                    || reports.Any(report => report.Function == function.Id))
                && !reports.Contains((function.Id, block.Id))) Add("Orphaned async void report.");
        }
        void Add(string message) => context.Add("ASIR1043", message);
    }

    internal static bool Matches(GuestFunction function, GuestAsyncExceptionTransfer transfer)
    {
        if (function.ReturnTypeId != "type:void"
            || function.Id != transfer.MethodFunctionId
                && !function.Id.StartsWith("function:synthetic:async_resume:", StringComparison.Ordinal)
            || function.Locals.Any(local => local.Id == "value:local:$async:producer_task:" + transfer.MethodFunctionId[9..])) return false;
        var blocks = function.Blocks.GroupBy(block => block.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        string marker = transfer.BlockId + ":propagate_exception";
        if (!blocks.TryGetValue(marker, out var body) || body.Instructions.Count != 8) return false;
        var code = body.Instructions;
        if (code[0] is not { Op: "local_load", OperandIds.Count: 0, ResultId: { } owner }
            || code[0].TargetId != transfer.OwnerLocalId
            || code[1] is not { Op: "constant", ResultId: { } shift, Constant: { Kind: "int64", Value: "32" } }
            || code[2] is not { Op: "call", ResultId: { } meta, TargetId: GuestTaskCancellationErrorValidator.MetaImportId }
            || !code[2].OperandIds.SequenceEqual(new[] { owner })
            || code[3] is not { Op: "binary", ResultId: { } packed, OperatorKind: "right_shift" }
            || !code[3].OperandIds.SequenceEqual(new[] { meta, shift })
            || code[4] is not { Op: "convert", ResultId: { } type }
            || !code[4].OperandIds.SequenceEqual(new[] { packed })
            || code[5] is not { Op: "convert", ResultId: { } source }
            || !code[5].OperandIds.SequenceEqual(new[] { meta })
            || code[6] is not { Op: "call", ResultId: { } root, TargetId: GuestTaskCancellationErrorValidator.RootImportId }
            || !code[6].OperandIds.SequenceEqual(new[] { owner })
            || code[7] is not { Op: "call", ResultId: { } status, TargetId: GuestAsyncVoidErrorOwners.ReportImportId }
            || !code[7].OperandIds.SequenceEqual(new[] { type, source, root })
            || body.Terminator.ConditionValueId != status
            || !GuestDirectAwaitCancellationValidator.Branches(body, marker + ":reported", marker + ":report_rejected")
            || !blocks.TryGetValue(marker + ":report_rejected", out var rejected)
            || rejected.Instructions.Count != 0 || rejected.Terminator.Kind != "trap"
            || !blocks.TryGetValue(marker + ":reported", out var accepted)
            || !GuestAsyncExceptionTransferValidator.Release(blocks, accepted, transfer, out var terminal)
            || terminal!.Terminator is not { Kind: "return", ReturnValueId: null }) return false;
        var expected = new[] { (owner, "type:int64"), (shift, "type:int64"), (meta, "type:int64"),
            (packed, "type:int64"), (type, "type:int32"), (source, "type:int32"),
            (root, "type:language_error_root"), (status, "type:int32") };
        var instructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();
        if (expected.Any(value => function.Locals.Count(local => local.Id == value.Item1 && local.TypeId == value.Item2) != 1
            || instructions.Count(instruction => instruction.ResultId == value.Item1) != 1
            || instructions.Any(instruction => instruction.TargetId == value.Item1))) return false;
        if (instructions.Count(instruction => instruction.OperandIds.Contains(root)) != 1) return false;
        // No edge may enter the release sequence before the report is accepted.
        string release = accepted.Id + ":release_error_owner";
        return IncomingOnly(accepted.Id, marker) && IncomingOnly(release, accepted.Id)
            && IncomingOnly(terminal.Id, accepted.Id, release);

        bool IncomingOnly(string target, params string[] allowed) => function.Blocks
            .Where(block => block.Terminator.TargetBlockId == target || block.Terminator.FalseTargetBlockId == target)
            .All(block => allowed.Contains(block.Id, StringComparer.Ordinal));
    }
}
