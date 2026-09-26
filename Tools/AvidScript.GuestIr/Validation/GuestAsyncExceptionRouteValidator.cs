using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

// IR 22 binds the two ways an await can finish (ready and resumed) to the
// same normal, language-fault and cancellation successors.
internal static class GuestAsyncExceptionRouteValidator
{
    private const string DiagnosticCode = "ASIR1030";
    private const string ResumeFunctionPrefix = "function:synthetic:async_resume:";
    private const string TaskImportId = "import:$async:task_i32_v1";

    public static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        bool ir22 = module.SchemaVersion == GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.ExceptionFlowIrVersion;
        bool ir23 = module.SchemaVersion == GuestTaskLanguageErrorValidator.DirectCleanupSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.DirectCleanupIrVersion;
        bool ir24 = GuestTaskCancellationErrorValidator.Supports(module)
            && module.AsyncExceptionTransfers is { Count: > 0 };
        if (!ir22 && !ir23 && !ir24 && !GuestTaskLocalLifetimeValidator.HasExceptionFlow(module))
        {
            if (module.AsyncExceptionRoutes is not null)
                Add(context, "Async exception routes require IR 22.");
            return;
        }
        if (module.AsyncExceptionRoutes is not { } routes || ir22 && routes.Count == 0)
        {
            Add(context, "Async exception IR requires protected Task await routes.");
            return;
        }

        HashSet<int> callbacks = new();
        int previousCallback = -1;
        foreach (GuestAsyncExceptionRoute route in routes)
        {
            if (route.CallbackId < 0 || route.CallbackId <= previousCallback
                || !callbacks.Add(route.CallbackId)
                || string.IsNullOrWhiteSpace(route.MethodFunctionId)
                || string.IsNullOrWhiteSpace(route.AwaitBlockId)
                || string.IsNullOrWhiteSpace(route.NormalTargetBlockId)
                || string.IsNullOrWhiteSpace(route.FaultTargetBlockId)
                || string.IsNullOrWhiteSpace(route.CancellationTargetBlockId)
                || string.IsNullOrWhiteSpace(route.OwnerLocalId)
                || string.IsNullOrWhiteSpace(route.TypeLocalId)
                || (ir24 ? route.FaultTargetBlockId != route.CancellationTargetBlockId
                        || route.NormalTargetBlockId == route.FaultTargetBlockId
                    : new[] { route.NormalTargetBlockId, route.FaultTargetBlockId,
                        route.CancellationTargetBlockId }.Distinct(StringComparer.Ordinal).Count() != 3))
            {
                Add(context, "IR 22 has an invalid or unordered await route.");
                continue;
            }
            previousCallback = route.CallbackId;

            if (!context.Functions.ContainsKey(route.MethodFunctionId)
                || !context.Functions.TryGetValue(
                    ResumeFunctionPrefix + route.CallbackId, out GuestFunction? resume)
                || resume.EntryBlockId != route.NormalTargetBlockId + ":entry"
                || !HasTargetsAndLocals(resume, route))
            {
                Add(context, $"Await callback {route.CallbackId} has no matching resume function, targets or owner slots.");
                continue;
            }
            resume = GuestTaskLocalLifetimeValidator.ResolveScopeExitRoutes(module, resume);
            GuestFunction[] sourceFunctions = module.Functions
                .Where(function => function.Id == route.MethodFunctionId
                    || function.Id.StartsWith(ResumeFunctionPrefix, StringComparison.Ordinal))
                .Where(function => function.Blocks.Any(block => block.Id == route.AwaitBlockId))
                .Select(function => GuestTaskLocalLifetimeValidator.ResolveScopeExitRoutes(module, function))
                .ToArray();
            if (sourceFunctions.Length == 0
                || sourceFunctions.Any(function => !HasTargetsAndLocals(function, route)
                    || !HasFailureFlow(function,
                        route.AwaitBlockId + ":task_failed", route,
                        releaseDirectToken: true, cancellationErrors: ir24)))
            {
                Add(context, $"Await callback {route.CallbackId} has an invalid immediate failure route.");
            }
            GuestBasicBlock[] resumedFailureBlocks = resume.Blocks
                .Where(block => block.Id.EndsWith(":task_read_rejected",
                    StringComparison.Ordinal)).ToArray();
            if (resumedFailureBlocks.Length != 1
                || !HasFailureFlow(resume, resumedFailureBlocks[0].Id, route,
                    releaseDirectToken: false, cancellationErrors: ir24))
            {
                Add(context, $"Await callback {route.CallbackId} has an invalid resumed failure route.");
            }
        }
        foreach (IGrouping<string, GuestAsyncExceptionRoute> methodRoutes in routes
            .GroupBy(route => route.MethodFunctionId, StringComparer.Ordinal))
        {
            HashSet<string> failures = methodRoutes
                .Select(route => route.AwaitBlockId + ":task_failed")
                .ToHashSet(StringComparer.Ordinal);
            HashSet<string> resumes = methodRoutes
                .Select(route => ResumeFunctionPrefix + route.CallbackId)
                .ToHashSet(StringComparer.Ordinal);
            HashSet<string> owners = methodRoutes
                .Select(route => route.OwnerLocalId)
                .ToHashSet(StringComparer.Ordinal);
            foreach (GuestFunction function in module.Functions.Where(function =>
                function.Locals.Any(local => owners.Contains(local.Id))))
            {
                if (function.Blocks.Any(block =>
                        block.Id.EndsWith(":task_failed", StringComparison.Ordinal)
                        && !failures.Contains(block.Id))
                    || function.Id.StartsWith(ResumeFunctionPrefix,
                            StringComparison.Ordinal)
                        && function.Blocks.Any(block => block.Id.EndsWith(
                            ":task_read_rejected", StringComparison.Ordinal))
                        && !resumes.Contains(function.Id))
                    Add(context, $"Method '{methodRoutes.Key}' has an unlisted protected await.");
                }
        }
        HashSet<string> listedOwners = routes.Select(route => route.OwnerLocalId)
            .ToHashSet(StringComparer.Ordinal);
        if (ir22 && module.Functions.SelectMany(function => function.Locals).Any(local =>
            local.Id.StartsWith("value:local:$async:exception_source:",
                StringComparison.Ordinal) && !listedOwners.Contains(local.Id)))
            Add(context, "IR 22 has a protected method without await routes.");
        if (ir24)
        {
            HashSet<string> failures = routes.Select(route => route.AwaitBlockId + ":task_failed")
                .ToHashSet(StringComparer.Ordinal);
            HashSet<string> resumes = routes.Select(route => ResumeFunctionPrefix + route.CallbackId)
                .ToHashSet(StringComparer.Ordinal);
            foreach (GuestFunction function in module.Functions.Where(function => function.Locals.Any(local =>
                local.Id.StartsWith("value:local:$async:exception_source:", StringComparison.Ordinal))))
            {
                if (function.Blocks.Any(block => block.Id.EndsWith(":task_failed", StringComparison.Ordinal)
                        && !failures.Contains(block.Id))
                    || function.Id.StartsWith(ResumeFunctionPrefix, StringComparison.Ordinal)
                        && function.Blocks.Any(block => block.Id.EndsWith(":task_read_rejected", StringComparison.Ordinal))
                        && !resumes.Contains(function.Id))
                    Add(context, "IR 24 contains an unlisted protected Task await.");
            }
        }
    }

    private static bool HasTargetsAndLocals(GuestFunction function,
        GuestAsyncExceptionRoute route)
    {
        return function.Blocks.Any(block => block.Id == route.NormalTargetBlockId)
            && function.Blocks.Any(block => block.Id == route.FaultTargetBlockId)
            && function.Blocks.Any(block => block.Id == route.CancellationTargetBlockId)
            && function.Locals.Any(local => local.Id == route.OwnerLocalId
                && local.TypeId == "type:int64")
            && function.Locals.Any(local => local.Id == route.TypeLocalId
                && local.TypeId == "type:int32");
    }

    private static bool HasFailureFlow(GuestFunction function, string failedId,
        GuestAsyncExceptionRoute route, bool releaseDirectToken, bool cancellationErrors)
    {
        Dictionary<string, GuestBasicBlock> blocks = function.Blocks
            .GroupBy(block => block.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        string faultId = failedId + ":fault";
        string otherId = failedId + ":not_fault";
        string cancelledId = failedId + ":cancelled";
        string invalidId = failedId + ":invalid_state";
        if (!Branches(blocks, failedId, faultId, otherId)
            || !Branches(blocks, otherId, cancelledId, invalidId)
            || !Traps(blocks, invalidId)
            || !Branches(blocks, faultId, faultId + ":retained",
                faultId + ":retain_rejected")
            || !Branches(blocks, cancelledId, cancelledId + ":retained",
                cancelledId + ":retain_rejected")
            || !Traps(blocks, faultId + ":retain_rejected")
            || !Traps(blocks, cancelledId + ":retain_rejected")
            || !blocks.TryGetValue(faultId, out GuestBasicBlock? fault))
            return false;
        GuestInstruction[] metadata = fault.Instructions.Where(instruction =>
            instruction.Op == "call"
                && instruction.TargetId == (cancellationErrors
                    ? GuestTaskCancellationErrorValidator.MetaImportId : GuestTaskLanguageErrorValidator.MetaImportId))
            .Take(2).ToArray();
        GuestInstruction[] roots = fault.Instructions.Where(instruction =>
            instruction.Op == "call"
                && instruction.TargetId == (cancellationErrors
                    ? GuestTaskCancellationErrorValidator.RootImportId : GuestTaskLanguageErrorValidator.RootImportId))
            .Take(2).ToArray();
        if (metadata.Length != 1 || roots.Length != 1
            || metadata[0] is not { OperandIds.Count: 1 }
            || roots[0] is not { OperandIds.Count: 1 }
            || metadata[0].OperandIds[0] != roots[0].OperandIds[0]
            || !blocks.TryGetValue(cancelledId, out GuestBasicBlock? cancelled)
            || RetainedSource(fault) != metadata[0].OperandIds[0]
            || RetainedSource(cancelled) != metadata[0].OperandIds[0])
            return false;
        string? typeToken = TypeTokenFromMetadata(fault, metadata[0].ResultId);
        string? cancelledType = null;
        if (cancellationErrors)
        {
            GuestInstruction[] cancelledMetadata = cancelled.Instructions.Where(instruction =>
                instruction.Op == "call" && instruction.TargetId == GuestTaskCancellationErrorValidator.MetaImportId).ToArray();
            GuestInstruction[] cancelledRoots = cancelled.Instructions.Where(instruction =>
                instruction.Op == "call" && instruction.TargetId == GuestTaskCancellationErrorValidator.RootImportId).ToArray();
            if (cancelledMetadata.Length != 1 || cancelledRoots.Length != 1
                || !cancelledMetadata[0].OperandIds.SequenceEqual(metadata[0].OperandIds)
                || !cancelledRoots[0].OperandIds.SequenceEqual(metadata[0].OperandIds)
                || !ChecksState(blocks[failedId], "2", out string? state)
                || !ChecksState(blocks[otherId], "3", out string? cancelledState)
                || state != cancelledState) return false;
            cancelledType = TypeTokenFromMetadata(cancelled, cancelledMetadata[0].ResultId);
            if (cancelledType is null) return false;
        }
        return typeToken is not null
            && OwnedBranch(blocks, faultId + ":retained",
                route.FaultTargetBlockId, route.OwnerLocalId,
                metadata[0].OperandIds[0], route.TypeLocalId, typeToken,
                releaseDirectToken)
            && OwnedBranch(blocks, cancelledId + ":retained",
                route.CancellationTargetBlockId, route.OwnerLocalId,
                metadata[0].OperandIds[0], cancellationErrors ? route.TypeLocalId : null,
                cancelledType, releaseDirectToken);
    }

    private static bool ChecksState(GuestBasicBlock block, string expected, out string? state)
    {
        GuestInstruction? compare = block.Instructions.FirstOrDefault(instruction =>
            instruction.Op == "binary" && instruction.OperatorKind == "equals"
            && instruction.ResultId == block.Terminator.ConditionValueId
            && instruction.OperandIds.Count == 2
            && block.Instructions.Any(value => value.Op == "constant"
                && value.ResultId == instruction.OperandIds[1]
                && value.Constant is { Kind: "int64", Value: var text } && text == expected));
        state = compare?.OperandIds[0];
        return state is not null;
    }

    private static string? RetainedSource(GuestBasicBlock block)
    {
        GuestInstruction[] calls = block.Instructions.Where(instruction =>
            instruction.Op == "call" && instruction.TargetId == TaskImportId)
            .Take(2).ToArray();
        if (calls.Length != 1 || calls[0] is not { OperandIds.Count: 4, ResultId: not null } call)
            return null;
        bool retainCommand = block.Instructions.Any(instruction =>
            instruction.Op == "constant"
            && instruction.ResultId == call.OperandIds[0]
            && instruction.Constant is { Kind: "int32", Value: "2" });
        bool checkedAcceptance = CheckedNonzero(block, call.ResultId);
        return retainCommand && checkedAcceptance ? call.OperandIds[1] : null;
    }

    private static string? TypeTokenFromMetadata(GuestBasicBlock block, string? metadataId)
    {
        GuestInstruction? shifted = block.Instructions.FirstOrDefault(instruction =>
            instruction.Op == "binary" && instruction.OperatorKind == "right_shift"
            && instruction.OperandIds.Count == 2
            && instruction.OperandIds[0] == metadataId
            && block.Instructions.Any(value => value.Op == "constant"
                && value.ResultId == instruction.OperandIds[1]
                && value.Constant is { Kind: "int64", Value: "32" }));
        return block.Instructions.FirstOrDefault(instruction =>
            instruction.Op == "convert"
            && instruction.OperandIds.Count == 1
            && instruction.OperandIds[0] == shifted?.ResultId)?.ResultId;
    }

    private static bool CheckedNonzero(GuestBasicBlock block, string resultId)
    {
        return block.Instructions.Any(instruction =>
            instruction.Op == "binary"
            && instruction.ResultId == block.Terminator.ConditionValueId
            && instruction.OperatorKind == "not_equals"
            && instruction.OperandIds.Count == 2
            && instruction.OperandIds[0] == resultId
            && block.Instructions.Any(value => value.Op == "constant"
                && value.ResultId == instruction.OperandIds[1]
                && value.Constant is { Kind: "int64", Value: "0" }));
    }

    private static bool Branches(IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        string blockId, string yes, string no)
    {
        return blocks.TryGetValue(blockId, out GuestBasicBlock? block)
            && block.Terminator.Kind == "branch_if"
            && block.Terminator.TargetBlockId == yes
            && block.Terminator.FalseTargetBlockId == no;
    }

    private static bool Traps(IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        string blockId)
    {
        return blocks.TryGetValue(blockId, out GuestBasicBlock? block)
            && block.Terminator.Kind == "trap";
    }

    private static bool OwnedBranch(
        IReadOnlyDictionary<string, GuestBasicBlock> blocks, string blockId,
        string targetId, string ownerLocalId, string sourceValueId,
        string? typeLocalId, string? typeValueId, bool releaseDirectToken)
    {
        if (!blocks.TryGetValue(blockId, out GuestBasicBlock? block)
            || !block.Instructions.Any(instruction => instruction.Op == "local_store"
                && instruction.TargetId == ownerLocalId
                && instruction.OperandIds.SequenceEqual(new[] { sourceValueId }))
            || typeLocalId is not null && !block.Instructions.Any(instruction =>
                instruction.Op == "local_store" && instruction.TargetId == typeLocalId
                && instruction.OperandIds.SequenceEqual(new[] { typeValueId! })))
            return false;
        if (!releaseDirectToken)
            return block.Terminator.Kind == "branch"
                && block.Terminator.TargetBlockId == targetId;
        if (block.Terminator.Kind != "branch_if"
            || block.Terminator.TargetBlockId != targetId
            || block.Terminator.FalseTargetBlockId is not string rejectedId
            || !Traps(blocks, rejectedId))
            return false;
        GuestInstruction[] releases = block.Instructions.Where(instruction =>
            instruction.Op == "call" && instruction.TargetId == TaskImportId
            && instruction.OperandIds.Count == 4
            && instruction.OperandIds[1] == sourceValueId).Take(2).ToArray();
        string? releaseResultId = releases.Length == 1 ? releases[0].ResultId : null;
        return releases.Length == 1
            && block.Instructions.Any(instruction => instruction.Op == "constant"
                && instruction.ResultId == releases[0].OperandIds[0]
                && instruction.Constant is { Kind: "int32", Value: "3" })
            && releaseResultId is not null
            && CheckedNonzero(block, releaseResultId);
    }

    private static void Add(GuestValidationContext context, string message)
    {
        context.Add(DiagnosticCode, message);
    }
}
