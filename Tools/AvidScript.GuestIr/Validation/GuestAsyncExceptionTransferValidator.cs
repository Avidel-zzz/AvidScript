using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestAsyncExceptionTransferValidator
{
    private const string TaskImport = "import:$async:task_i32_v1";
    private const string OwnerPrefix = "value:local:$async:exception_source:";

    public static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        if (!GuestTaskCancellationErrorValidator.Supports(module))
        {
            if (module.AsyncExceptionTransfers is not null) Add(context, "Exception owner transfers require IR 24/1.23.");
            return;
        }
        if (module.AsyncExceptionTransfers is not { } transfers)
        {
            if (module.Functions.SelectMany(function => function.Locals)
                .Any(local => local.Id.StartsWith(OwnerPrefix, StringComparison.Ordinal)))
                Add(context, "IR 24 exception owners require transfer metadata.");
            return;
        }
        if (transfers.Count == 0 || module.AsyncExceptionRoutes is null || module.DirectAwaitRoutes is null)
            Add(context, "Cancellation control flow requires transfer metadata and both await route lists.");
        HashSet<(string Method, string Block)> identities = new();
        HashSet<string> owners = new(StringComparer.Ordinal);
        HashSet<string> markers = new(StringComparer.Ordinal);
        (string Method, string Block)? previous = null;
        foreach (GuestAsyncExceptionTransfer transfer in transfers)
        {
            bool ordered = previous is null
                || string.CompareOrdinal(previous.Value.Method, transfer.MethodFunctionId) < 0
                || previous.Value.Method == transfer.MethodFunctionId
                    && string.CompareOrdinal(previous.Value.Block, transfer.BlockId) < 0;
            previous = (transfer.MethodFunctionId, transfer.BlockId);
            if (!ordered || !identities.Add((transfer.MethodFunctionId, transfer.BlockId))
                || !context.Functions.ContainsKey(transfer.MethodFunctionId)
                || transfer.Kind is not ("end_catch" or "rethrow" or "propagate_exception")
                || (transfer.Kind == "propagate_exception" ? transfer.TargetBlockId is not null
                    : string.IsNullOrWhiteSpace(transfer.TargetBlockId)))
            {
                Add(context, "Invalid or unordered exception transfer metadata.");
                continue;
            }
            owners.Add(transfer.OwnerLocalId);
            string marker = transfer.BlockId + ":" + transfer.Kind;
            markers.Add(marker);
            GuestFunction[] functions = module.Functions.Where(function =>
                function.Blocks.Any(block => block.Id == transfer.BlockId)).ToArray();
            if (functions.Length == 0 || functions.Any(function =>
                    !Matches(function, transfer, marker)))
                Add(context, $"Exception transfer '{transfer.BlockId}' does not preserve its owner or successor.");
        }
        if (module.Functions.SelectMany(function => function.Locals).Any(local =>
                local.Id.StartsWith(OwnerPrefix, StringComparison.Ordinal) && !owners.Contains(local.Id))
            || module.Functions.SelectMany(function => function.Blocks).Any(block =>
                (block.Id.EndsWith(":end_catch", StringComparison.Ordinal)
                    || block.Id.EndsWith(":rethrow", StringComparison.Ordinal)
                    || block.Id.EndsWith(":propagate_exception", StringComparison.Ordinal))
                && !markers.Contains(block.Id)))
            Add(context, "Cancellation control flow contains an unlisted owner or transfer.");
    }

    private static bool Matches(GuestFunction function, GuestAsyncExceptionTransfer transfer, string marker)
    {
        if ((function.Id != transfer.MethodFunctionId
                && !function.Id.StartsWith("function:synthetic:async_resume:", StringComparison.Ordinal))
            || !function.Locals.Any(local => local.Id == transfer.OwnerLocalId && local.TypeId == "type:int64")
            || !function.Locals.Any(local => local.Id == transfer.TypeLocalId && local.TypeId == "type:int32")) return false;
        Dictionary<string, GuestBasicBlock> blocks = function.Blocks.GroupBy(block => block.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        if (!blocks.TryGetValue(marker, out GuestBasicBlock? body)
            || !Branch(blocks[transfer.BlockId], marker)
            || transfer.TargetBlockId is { } target && !blocks.ContainsKey(target)) return false;
        if (transfer.Kind == "rethrow")
            return body.Instructions.Count == 0 && Branch(body, transfer.TargetBlockId!);
        if (transfer.Kind == "end_catch")
            return Release(blocks, body, transfer, out GuestBasicBlock? released)
                && Branch(released!, transfer.TargetBlockId!);

        GuestInstruction[] calls = body.Instructions.Where(instruction => instruction.Op == "call"
            && instruction.TargetId == "import:$async:task_propagate_failure_v1").ToArray();
        string producer = transfer.OwnerLocalId.Replace(OwnerPrefix,
            "value:local:$async:producer_task:", StringComparison.Ordinal);
        return calls.Length == 1 && calls[0] is { OperandIds.Count: 2, ResultId: { } result }
            && Loaded(body, calls[0].OperandIds[0], transfer.OwnerLocalId)
            && Loaded(body, calls[0].OperandIds[1], producer)
            && body.Terminator.ConditionValueId == result
            && GuestDirectAwaitCancellationValidator.Branches(body, marker + ":propagated", marker + ":propagate_rejected")
            && blocks.TryGetValue(marker + ":propagate_rejected", out GuestBasicBlock? rejected)
            && rejected.Terminator.Kind == "trap"
            && blocks.TryGetValue(marker + ":propagated", out GuestBasicBlock? accepted)
            && Release(blocks, accepted, transfer, out GuestBasicBlock? terminal)
            && terminal!.Terminator.Kind == "return";
    }

    private static bool Release(IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        GuestBasicBlock entry, GuestAsyncExceptionTransfer transfer, out GuestBasicBlock? released)
    {
        released = null;
        string releaseId = entry.Id + ":release_error_owner";
        string nextId = entry.Id + ":owner_released";
        if (!GuestDirectAwaitCancellationValidator.Branches(entry, releaseId, nextId)
            || !blocks.TryGetValue(releaseId, out GuestBasicBlock? release)
            || !blocks.TryGetValue(nextId, out released)
            || !blocks.TryGetValue(releaseId + ":rejected", out GuestBasicBlock? rejected)
            || rejected.Terminator.Kind != "trap") return false;
        GuestInstruction[] loads = entry.Instructions.Where(instruction => instruction.Op == "local_load"
            && instruction.TargetId == transfer.OwnerLocalId).ToArray();
        GuestInstruction[] calls = release.Instructions.Where(instruction => instruction.Op == "call"
            && instruction.TargetId == TaskImport).ToArray();
        return loads.Length == 1 && loads[0].ResultId is { } owner
            && GuestDirectAwaitCancellationValidator.Nonzero(entry, owner, "int64")
            && calls.Length == 1 && calls[0] is { OperandIds.Count: 4, ResultId: { } result }
            && calls[0].OperandIds[1] == owner
            && GuestDirectAwaitCancellationValidator.Literal(release, calls[0].OperandIds[0], "int32", 3)
            && GuestDirectAwaitCancellationValidator.Nonzero(release, result, "int64")
            && GuestDirectAwaitCancellationValidator.Branches(release, nextId, rejected.Id)
            && Zeroed(released, transfer.OwnerLocalId, "int64")
            && Zeroed(released, transfer.TypeLocalId, "int32");
    }

    private static bool Zeroed(GuestBasicBlock block, string local, string kind)
    {
        GuestInstruction[] stores = block.Instructions.Where(instruction => instruction.Op == "local_store"
            && instruction.TargetId == local).ToArray();
        return stores.Length == 1 && stores[0].OperandIds.Count == 1
            && GuestDirectAwaitCancellationValidator.Literal(block, stores[0].OperandIds[0], kind, 0);
    }

    private static bool Loaded(GuestBasicBlock block, string value, string local) =>
        block.Instructions.Any(instruction => instruction.Op == "local_load"
            && instruction.ResultId == value && instruction.TargetId == local);

    private static bool Branch(GuestBasicBlock block, string target) =>
        block.Terminator.Kind == "branch" && block.Terminator.TargetBlockId == target;

    private static void Add(GuestValidationContext context, string message) => context.Add("ASIR1033", message);
}
