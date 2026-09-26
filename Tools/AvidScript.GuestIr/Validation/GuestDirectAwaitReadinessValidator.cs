using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestDirectAwaitReadinessValidator
{
    private const string Code = "ASIR1038";

    internal static void Validate(GuestValidationContext context)
    {
        var artifact = context.InputArtifact;
        var queryImports = artifact.Imports.Where(import => import.Id == GuestDirectAwaitReadiness.ImportId
            || import.Name == GuestDirectAwaitReadiness.ImportName).ToArray();
        if (!GuestDirectAwaitReadiness.IsVersion(artifact))
        {
            if (artifact.DirectAwaitReadiness is not null || queryImports.Length != 0)
                Add(context, "Direct await readiness requires Guest IR 31/1.30.");
            return;
        }
        if (artifact.DirectAwaitReadiness is not { Guards.Count: > 0 and <= 4096 } plan
            || !GuestDirectAwaitReadiness.IsBase(plan.BaseSchemaVersion, plan.BaseIrVersion)
            || artifact.Language != "csharp" || !GuestTaskCancellationErrorValidator.Supports(context.Module)
            || artifact.DirectAwaitRoutes is null || queryImports.Length != 1
            || queryImports[0] is not { Id: GuestDirectAwaitReadiness.ImportId, Module: "avidscript",
                Name: GuestDirectAwaitReadiness.ImportName, ReturnTypeId: "type:int32",
                DispatchClass: "semantic", OptimizationClass: "none", BindingOrdinal: -1 }
            || !queryImports[0].ParameterTypeIds.SequenceEqual(new[] { "type:int64" }))
        {
            Add(context, "Readiness needs its exact base contract, bounded guards and versioned status import.");
            return;
        }
        HashSet<(string Function, int Callback)> guards = new();
        HashSet<(string Function, string Block)> queries = new();
        (string Function, int Callback)? previous = null;
        foreach (var guard in plan.Guards)
        {
            bool ordered = previous is null || string.CompareOrdinal(previous.Value.Function, guard.FunctionId) < 0
                || previous.Value.Function == guard.FunctionId && previous.Value.Callback < guard.CallbackId;
            previous = (guard.FunctionId, guard.CallbackId);
            if (!ordered || !guards.Add((guard.FunctionId, guard.CallbackId))
                || !queries.Add((guard.FunctionId, guard.CheckBlockId)) || !ValidateGuard(context, guard))
                Add(context, "Invalid or unordered readiness guard: " + guard.FunctionId + "/" + guard.CallbackId);
            else context.CheckedDirectCancellationProducers.Add((guard.FunctionId, guard.CancellationBlockId + ":task_created"));
        }
        foreach (var function in artifact.Functions)
        foreach (var block in function.Blocks)
        {
            if (block.Instructions.Any(instruction => instruction.Op == "call"
                    && instruction.TargetId == GuestDirectAwaitReadiness.ImportId)
                && !queries.Contains((function.Id, block.Id)))
                Add(context, "Unlisted cancellation status query: " + function.Id + "/" + block.Id);
            foreach (var route in artifact.DirectAwaitRoutes.Where(route => route.AwaitBlockId == block.Id))
                if (block.Instructions.Any(instruction => IsBinding(context, instruction))
                    && !guards.Contains((function.Id, route.CallbackId)))
                    Add(context, "A source-bound Timer await has no readiness guard: " + function.Id + "/" + route.CallbackId);
        }
    }

    private static bool ValidateGuard(GuestValidationContext context, GuestDirectAwaitReadinessGuard guard)
    {
        var matches = context.Module.DirectAwaitRoutes!.Where(route => route.CallbackId == guard.CallbackId).ToArray();
        if (matches.Length != 1 || matches[0].Cancellation is null
            || matches[0].AwaitBlockId != guard.ScheduleBlockId
            || !context.Functions.TryGetValue(guard.FunctionId, out var original)) return false;
        var route = matches[0];
        if (guard.FunctionId != route.MethodFunctionId
            && !guard.FunctionId.StartsWith("function:synthetic:async_resume:", StringComparison.Ordinal)) return false;
        var function = GuestTaskLocalLifetimeValidator.ResolveScopeExitRoutes(context.Module, original);
        var blocks = function.Blocks.GroupBy(block => block.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        if (!blocks.TryGetValue(guard.CheckBlockId, out var check)
            || !blocks.TryGetValue(guard.ScheduleBlockId, out var schedule)
            || check.Terminator.FalseTargetBlockId is not { } cancelCheckId
            || !blocks.TryGetValue(cancelCheckId, out var cancelCheck)
            || cancelCheck.Terminator.FalseTargetBlockId is not { } invalidId
            || !blocks.TryGetValue(invalidId, out var invalid)
            || invalid.Instructions.Count != 0 || invalid.Terminator.Kind != "trap") return false;
        var queries = check.Instructions.Where(instruction => instruction.Op == "call"
            && instruction.TargetId == GuestDirectAwaitReadiness.ImportId).ToArray();
        if (queries.Length != 1 || queries[0] is not { ResultId: { } status, OperandIds.Count: 1 }
            || check.Instructions.Count < 3 || check.Instructions[^3] != queries[0]
            || cancelCheck.Instructions.Count != 2
            || !StatusBranch(check, status, 1, guard.ScheduleBlockId, cancelCheckId)
            || !StatusBranch(cancelCheck, status, 2, guard.CancellationBlockId, invalidId)
            || !OnlyIncoming(function, guard.ScheduleBlockId, guard.CheckBlockId)
            || !OnlyIncoming(function, cancelCheckId, guard.CheckBlockId)
            || !OnlyIncoming(function, guard.CancellationBlockId, cancelCheckId)
            || !OnlyIncoming(function, invalidId, cancelCheckId)) return false;
        string token = queries[0].OperandIds[0];
        var allInstructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();
        if (!function.Parameters.Concat(function.Locals).Any(register => register.Id == token && register.TypeId == "type:int64")
            || allInstructions.Count(instruction => instruction.ResultId == status) != 1
            || allInstructions.Count(instruction => instruction.ResultId == token)
                + function.Parameters.Count(parameter => parameter.Id == token) != 1
            || allInstructions.Any(instruction => instruction.TargetId == status || instruction.TargetId == token)) return false;
        var bindings = schedule.Instructions.Where(instruction => IsBinding(context, instruction)).ToArray();
        if (schedule.Instructions.FirstOrDefault() is not { Op: "call", ResultId: { } scheduled } producer
            || producer.TargetId != route.ScheduleImportId
            || bindings.Length != 1 || !bindings[0].OperandIds.SequenceEqual(new[] { token, scheduled })
            || !GuestDirectAwaitCancellationValidator.Validate(context.Module, function, blocks, guard.CancellationBlockId, route)) return false;
        return true;
    }

    private static bool IsBinding(GuestValidationContext context, GuestInstruction instruction) =>
        instruction.Op == "call" && context.Imports.TryGetValue(instruction.TargetId ?? "", out var import)
        && import is { Module: "env", Name: "continuation_bind_cancel", ReturnTypeId: "type:int32" }
        && import.ParameterTypeIds.SequenceEqual(new[] { "type:int64", "type:int64" });

    private static bool StatusBranch(GuestBasicBlock block, string status, int value, string yes, string no) =>
        GuestDirectAwaitCancellationValidator.Branches(block, yes, no)
        && block.Instructions.LastOrDefault() is { Op: "binary", OperatorKind: "equals", OperandIds.Count: 2 } compare
        && compare.ResultId == block.Terminator.ConditionValueId && compare.OperandIds[0] == status
        && GuestDirectAwaitCancellationValidator.Literal(block, compare.OperandIds[1], "int32", value);

    private static bool OnlyIncoming(GuestFunction function, string target, string source) =>
        function.EntryBlockId != target
        && function.Blocks.Where(block => block.Terminator.TargetBlockId == target || block.Terminator.FalseTargetBlockId == target)
            .Select(block => block.Id).SequenceEqual(new[] { source });

    private static void Add(GuestValidationContext context, string message) => context.Add(Code, message);
}
