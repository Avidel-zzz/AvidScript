using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

// IR 23 routes cancellation to cleanup; IR 24 creates a typed cancellation
// owner before entering the source method's exception dispatch.
internal static class GuestDirectAwaitRouteValidator
{
    private const string DiagnosticCode = "ASIR1031";
    private const string ResumePrefix = "function:synthetic:async_resume:";

    public static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        bool ir23 = module.SchemaVersion == GuestTaskLanguageErrorValidator.DirectCleanupSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.DirectCleanupIrVersion;
        bool ir24 = GuestTaskCancellationErrorValidator.Supports(module)
            && module.AsyncExceptionTransfers is { Count: > 0 };
        if (!ir23 && !ir24 && !GuestTaskLocalLifetimeValidator.HasDirectCleanup(module))
        {
            if (module.DirectAwaitRoutes is not null)
                Add(context, "Direct await routes require IR 23/1.22.");
            return;
        }
        if (module.DirectAwaitRoutes is not { } routes || ir23 && routes.Count == 0)
        {
            Add(context, "IR 23 requires at least one protected direct await route.");
            return;
        }

        HashSet<int> callbacks = module.AsyncExceptionRoutes?
            .Select(route => route.CallbackId).ToHashSet() ?? new HashSet<int>();
        HashSet<string> awaitBlocks = new(StringComparer.Ordinal);
        HashSet<(string Function, string Block)> cancellationProducers = new();
        int previous = -1;
        foreach (GuestDirectAwaitRoute route in routes)
        {
            if (route.CallbackId <= previous || route.CallbackId <= 0
                || !callbacks.Add(route.CallbackId)
                || !awaitBlocks.Add(route.AwaitBlockId)
                || route.ProducerKind is not ("delay" or "next_tick")
                || route.NormalTargetBlockId == route.CancellationTargetBlockId
                || (ir24 ? route.Cancellation is null : route.Cancellation is not null)
                || !context.Imports.TryGetValue(route.ScheduleImportId,
                    out GuestImport? schedule)
                || schedule.Module != "avidscript"
                || schedule.Name != "avid_continuation_delay_cancel_resume_v1"
                || !schedule.ParameterTypeIds.SequenceEqual(new[]
                    { "type:float32", "type:int32" }, StringComparer.Ordinal)
                || schedule.ReturnTypeId != "type:int64"
                || schedule.DispatchClass != "semantic"
                || schedule.OptimizationClass != "none"
                || schedule.BindingOrdinal != -1)
            {
                Add(context, "IR 23 has an invalid or unordered direct await route.");
                continue;
            }
            previous = route.CallbackId;
            cancellationProducers.Add((ResumePrefix + route.CallbackId,
                route.NormalTargetBlockId + ":entry:cancel_path:task_created"));

            if (!context.Functions.ContainsKey(route.MethodFunctionId)
                || !context.Functions.TryGetValue(ResumePrefix + route.CallbackId,
                    out GuestFunction? resume)
                || resume.Parameters.Count != 2
                || resume.Parameters[0].TypeId != "type:int64"
                || resume.Parameters[1].TypeId != "type:int32"
                || resume.EntryBlockId != route.NormalTargetBlockId + ":entry"
                || !HasStatusRoutes(module, GuestTaskLocalLifetimeValidator.ResolveScopeExitRoutes(module, resume), route, ir24))
            {
                Add(context, $"Direct await callback {route.CallbackId} has no validated status-aware resume.");
                continue;
            }
            GuestFunction[] schedulers = module.Functions.Where(function =>
                    function.Id == route.MethodFunctionId
                    || function.Id.StartsWith(ResumePrefix, StringComparison.Ordinal))
                .Where(function => function.Blocks.Any(block => block.Id == route.AwaitBlockId))
                .ToArray();
            if (schedulers.Length == 0 || schedulers.Any(function =>
                    function.Blocks.First(block => block.Id == route.AwaitBlockId)
                        .Instructions.Count(instruction => instruction.Op == "call"
                            && instruction.TargetId == route.ScheduleImportId) != 1))
                Add(context, $"Direct await callback {route.CallbackId} does not use its cancel-resume Timer import.");
        }
        if (module.Imports.Count(import => import.Module == "avidscript"
                && import.Name == "avid_continuation_delay_cancel_resume_v1") != (routes.Count > 0 ? 1 : 0)
            || module.Functions.SelectMany(function => function.Blocks)
                .Any(block => block.Instructions.Any(instruction => instruction.Op == "call"
                    && context.Imports.TryGetValue(instruction.TargetId ?? string.Empty,
                        out GuestImport? import)
                    && import.Name == "avid_continuation_delay_cancel_resume_v1")
                    && !awaitBlocks.Contains(block.Id)))
            Add(context, "IR 23 contains an unlisted cancel-resume Timer call.");
        if (ir24 && module.Functions.Any(function => function.Blocks.Any(block =>
                block.Instructions.Any(instruction => instruction.Op == "call"
                    && instruction.TargetId == GuestTaskCancellationErrorValidator.ImportId)
                && !cancellationProducers.Contains((function.Id, block.Id)))))
            Add(context, "IR 24 contains an unlisted typed cancellation producer.");
    }

    private static bool HasStatusRoutes(GuestModule module, GuestFunction resume,
        GuestDirectAwaitRoute route, bool typedCancellation)
    {
        Dictionary<string, GuestBasicBlock> blocks = resume.Blocks
            .GroupBy(block => block.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(),
                StringComparer.Ordinal);
        string entry = route.NormalTargetBlockId + ":entry";
        string normal = entry + ":normal_path";
        string checkCancel = entry + ":cancel_check";
        string cancel = entry + ":cancel_path";
        string invalid = entry + ":invalid_status";
        if (!blocks.TryGetValue(entry, out GuestBasicBlock? first)) return false;
        GuestBasicBlock statusBlock = first;
        string stateAccepted = entry + ":state_accepted";
        string stateRejected = entry + ":state_rejected";
        if (blocks.ContainsKey(stateAccepted))
        {
            if (first.Terminator.Kind != "branch_if"
                || first.Terminator.TargetBlockId != stateAccepted
                || first.Terminator.FalseTargetBlockId != stateRejected
                || !blocks.TryGetValue(stateRejected, out GuestBasicBlock? rejectedState)
                || rejectedState.Terminator.Kind != "trap") return false;
            statusBlock = blocks[stateAccepted];
        }
        return blocks.ContainsKey(route.NormalTargetBlockId)
            && blocks.ContainsKey(route.CancellationTargetBlockId)
            && IsStatusBranch(statusBlock, resume.Parameters[1].Id,
                "1", normal, checkCancel)
            && blocks.TryGetValue(checkCancel, out GuestBasicBlock? cancellationCheck)
            && IsStatusBranch(cancellationCheck, resume.Parameters[1].Id,
                "3", cancel, invalid)
            && blocks.TryGetValue(normal, out GuestBasicBlock? normalPath)
            && normalPath.Terminator.Kind == "branch"
            && normalPath.Terminator.TargetBlockId == route.NormalTargetBlockId
            && blocks.TryGetValue(cancel, out GuestBasicBlock? cancellationPath)
            && (typedCancellation
                ? GuestDirectAwaitCancellationValidator.Validate(module, resume, blocks, cancel, route)
                : cancellationPath.Terminator.Kind == "branch"
                    && cancellationPath.Terminator.TargetBlockId == route.CancellationTargetBlockId)
            && blocks.TryGetValue(invalid, out GuestBasicBlock? rejected)
            && rejected.Terminator.Kind == "trap";
    }

    private static bool IsStatusBranch(GuestBasicBlock block, string statusId,
        string expected, string yes, string no)
    {
        if (block.Terminator.Kind != "branch_if"
            || block.Terminator.TargetBlockId != yes
            || block.Terminator.FalseTargetBlockId != no)
            return false;
        return block.Instructions.Any(compare => compare.Op == "binary"
            && compare.OperatorKind == "equals"
            && compare.ResultId == block.Terminator.ConditionValueId
            && compare.OperandIds.Count == 2
            && compare.OperandIds[0] == statusId
            && block.Instructions.Any(constant => constant.Op == "constant"
                && constant.ResultId == compare.OperandIds[1]
                && constant.Constant is { Kind: "int32", Value: var value }
                && value == expected));
    }

    private static void Add(GuestValidationContext context, string message) =>
        context.Add(DiagnosticCode, message);
}
