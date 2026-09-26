using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestAsyncSynchronousExceptionValidator
{
    private const string TaskImport = "import:$async:task_i32_v1";

    internal static void Validate(GuestValidationContext context)
    {
        var artifact = context.Artifact;
        if (!GuestAsyncSynchronousExceptions.IsVersion(artifact))
        {
            if (artifact.AsyncSynchronousExceptions is not null
                || artifact.Provenance.SemanticSchemaVersion == GuestAsyncSynchronousExceptions.SemanticSchemaVersion
                || artifact.Provenance.SemanticVersion == GuestAsyncSynchronousExceptions.SemanticVersion)
                Add(context, "Synchronous async execution requires paired Semantic 50/1.59 and IR 29/1.28.");
            return;
        }
        if (!GuestAsyncSynchronousExceptions.HasSourceContract(artifact) || artifact.Language != "csharp"
            || artifact.StaticStorage is not null || artifact.TaskErrorTransfers is not null
            || artifact.AsyncSynchronousExceptions is not { Sites.Count: <= 4096 } plan
            || artifact.TaskLocalLifetimes?.ExceptionModel != "cancellation"
            || artifact.LanguageOutcomeTypes is null || artifact.LanguageErrorCatalog is null)
        {
            Add(context, "Synchronous async execution requires its exact provenance, outcome plan and cancellation ownership profile.");
            return;
        }
        (string Function, string Block)? previous = null;
        foreach (var site in plan.Sites)
        {
            bool ordered = previous is null || string.CompareOrdinal(previous.Value.Function, site.FunctionId) < 0
                || previous.Value.Function == site.FunctionId && string.CompareOrdinal(previous.Value.Block, site.CallBlockId) < 0;
            previous = (site.FunctionId, site.CallBlockId);
            if (!ordered || !ValidateSite(context, site))
                Add(context, "Invalid, duplicate or unordered synchronous exception route: " + site.FunctionId + "/" + site.CallBlockId);
            else context.CheckedTaskErrorTransfers.Add((site.FunctionId, site.CallBlockId + ":synchronous_exception:task_created", 3));
        }
        foreach (var function in artifact.Functions)
        foreach (var block in function.Blocks)
        for (int index = 0; index < block.Instructions.Count; ++index)
            if (block.Instructions[index] is { Op: "call", TargetId: GuestTaskLanguageErrorValidator.ImportId }
                && !GuestTaskLanguageErrorValidator.HasFreshRoot(block, index)
                && !context.CheckedTaskErrorTransfers.Contains((function.Id, block.Id, index)))
                Add(context, "An outcome fault is missing its checked synchronous exception route: " + function.Id);
    }

    private static bool ValidateSite(GuestValidationContext context, GuestAsyncSynchronousExceptionSite site)
    {
        if (!context.Functions.ContainsKey(site.MethodFunctionId)
            || !context.Functions.TryGetValue(site.FunctionId, out var original)
            || site.FunctionId != site.MethodFunctionId && !site.FunctionId.StartsWith("function:synthetic:async_resume:", StringComparison.Ordinal)
            || !site.MethodFunctionId.StartsWith("function:", StringComparison.Ordinal)
            || site.OwnerLocalId != "value:local:$async:exception_source:" + site.MethodFunctionId[9..]
            || site.TypeLocalId != "value:local:$async:exception_type:" + site.MethodFunctionId[9..]
            || !original.Locals.Any(local => local.Id == site.OwnerLocalId && local.TypeId == "type:int64")
            || !original.Locals.Any(local => local.Id == site.TypeLocalId && local.TypeId == "type:int32")) return false;
        var function = GuestTaskLocalLifetimeValidator.ResolveScopeExitRoutes(context.Module, original);
        var blocks = function.Blocks.GroupBy(block => block.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        string marker = site.CallBlockId + ":synchronous_exception";
        if (!blocks.ContainsKey(site.TargetBlockId)
            || !blocks.TryGetValue(site.CallBlockId, out var callBlock)
            || !blocks.TryGetValue(marker, out var create)
            || !blocks.TryGetValue(marker + ":task_created", out var fault)
            || !blocks.TryGetValue(marker + ":task_create_rejected", out var invalid)
            || !blocks.TryGetValue(marker + ":fault_acquired", out var acquired)
            || !blocks.TryGetValue(marker + ":fault_rejected", out var rejected)
            || !blocks.TryGetValue(marker + ":publish", out var publish)
            || site.CallInstructionIndex < 0 || site.CallInstructionIndex + 2 != callBlock.Instructions.Count
            || invalid.Instructions.Count != 0 || invalid.Terminator.Kind != "trap"
            || rejected.Terminator.Kind != "trap" || fault.Instructions.Count != 4) return false;
        var call = callBlock.Instructions[site.CallInstructionIndex];
        var status = callBlock.Instructions[site.CallInstructionIndex + 1];
        if (call.Op != "call" || call.ResultId is not { } outcome || call.TargetId is not { } callee
            || !context.Functions.TryGetValue(callee, out var target)
            || !context.Module.LanguageOutcomeTypes!.Any(type => type.TypeId == target.ReturnTypeId)
            || status.Op != "field_load" || status.ResultId is not { } condition || status.TargetId != "field:status"
            || !status.OperandIds.SequenceEqual(new[] { outcome })
            || callBlock.Terminator.Kind != "branch_if" || callBlock.Terminator.ConditionValueId != condition
            || callBlock.Terminator.TargetBlockId != marker || callBlock.Terminator.FalseTargetBlockId == marker) return false;
        var creates = create.Instructions.Where(item => item.Op == "call").ToArray();
        if (creates.Length != 1 || creates[0] is not { TargetId: TaskImport, OperandIds.Count: 4, ResultId: { } task }
            || create.Instructions.Count != 7
            || !Literal(create, creates[0].OperandIds[0], "int32", 1)
            || !Literal(create, creates[0].OperandIds[1], "int64", 0)
            || !Literal(create, creates[0].OperandIds[2], "int32", 0)
            || !Literal(create, creates[0].OperandIds[3], "int32", 0)
            || !GuestDirectAwaitCancellationValidator.Nonzero(create, task, "int64")
            || !Branches(create, fault.Id, invalid.Id)) return false;
        var submit = fault.Instructions[3];
        if (submit is not { Op: "call", TargetId: GuestTaskLanguageErrorValidator.ImportId, OperandIds.Count: 4, ResultId: { } accepted }
            || submit.OperandIds[0] != task || fault.Terminator.ConditionValueId != accepted
            || !Branches(fault, acquired.Id, rejected.Id)) return false;
        var all = function.Blocks.SelectMany(block => block.Instructions).ToArray();
        bool Unique(string id) => all.Count(item => item.ResultId == id) == 1 && all.All(item => item.TargetId != id);
        if (!Unique(outcome) || !Unique(condition) || !Unique(task) || !Unique(accepted)
            || all.Any(item => item.OperandIds.Contains(outcome) && (item.Op != "field_load" || item.OperandIds.Count != 1))) return false;
        string[] fields = { "field:error_type", "field:source", "field:error_root" };
        for (int index = 0; index < fields.Length; ++index)
        {
            var load = fault.Instructions[index];
            if (load.Op != "field_load" || load.TargetId != fields[index]
                || !load.OperandIds.SequenceEqual(new[] { outcome }) || load.ResultId != submit.OperandIds[index + 1]
                || load.ResultId is null || !Unique(load.ResultId)) return false;
        }
        var transfer = new GuestAsyncExceptionTransfer(site.MethodFunctionId, site.CallBlockId, "synchronous_exception",
            site.TargetBlockId, site.OwnerLocalId, site.TypeLocalId);
        if (!GuestAsyncExceptionTransferValidator.Release(blocks, acquired, transfer, out var cleared)
            || acquired.Instructions.Count != 3 || cleared!.Instructions.Count != 4
            || cleared.Terminator is not { Kind: "branch" } || cleared.Terminator.TargetBlockId != publish.Id
            || publish.Instructions.Count != 2 || publish.Terminator.Kind != "branch"
            || publish.Terminator.TargetBlockId != site.TargetBlockId
            || !GuestDirectAwaitCancellationValidator.Store(publish, site.OwnerLocalId, task)
            || !GuestDirectAwaitCancellationValidator.Store(publish, site.TypeLocalId, submit.OperandIds[1])) return false;
        var releases = rejected.Instructions.Where(item => item.Op == "call").ToArray();
        if (releases.Length != 1 || releases[0] is not { TargetId: TaskImport, OperandIds.Count: 4 }
            || rejected.Instructions.Count != 4 || releases[0].OperandIds[1] != task
            || !Literal(rejected, releases[0].OperandIds[0], "int32", 3)
            || !Literal(rejected, releases[0].OperandIds[2], "int32", 0)
            || !Literal(rejected, releases[0].OperandIds[3], "int32", 0)) return false;
        var taskUses = all.Where(item => item.OperandIds.Contains(task)).ToArray();
        var allowedTaskUses = new[] { create.Instructions[^1], submit, releases[0], publish.Instructions[0] };
        if (taskUses.Length != allowedTaskUses.Length || taskUses.Any(item => !allowedTaskUses.Contains(item))
            || function.Blocks.Any(block => block.Terminator.ReturnValueId == task
                || block.Terminator.ConditionValueId == task)) return false;
        bool Parents(string block, params string[] parents) => function.EntryBlockId != block
            && function.Blocks.Where(item => item.Terminator.TargetBlockId == block || item.Terminator.FalseTargetBlockId == block)
                .Select(item => item.Id).ToHashSet(StringComparer.Ordinal).SetEquals(parents);
        string release = acquired.Id + ":release_error_owner";
        if (blocks[release].Instructions.Count != 6 || blocks[release + ":rejected"].Instructions.Count != 0)
            return false;
        var releaseCall = blocks[release].Instructions[3];
        if (releaseCall is not { Op: "call", TargetId: TaskImport, OperandIds.Count: 4 }
            || !Literal(blocks[release], releaseCall.OperandIds[2], "int32", 0)
            || !Literal(blocks[release], releaseCall.OperandIds[3], "int32", 0)) return false;
        return Parents(marker, callBlock.Id) && Parents(fault.Id, marker) && Parents(invalid.Id, marker)
            && Parents(acquired.Id, fault.Id) && Parents(rejected.Id, fault.Id)
            && Parents(release, acquired.Id) && Parents(release + ":rejected", release)
            && Parents(cleared.Id, acquired.Id, release) && Parents(publish.Id, cleared.Id);
    }

    private static bool Literal(GuestBasicBlock block, string value, string kind, int expected) =>
        GuestDirectAwaitCancellationValidator.Literal(block, value, kind, expected);
    private static bool Branches(GuestBasicBlock block, string yes, string no) =>
        GuestDirectAwaitCancellationValidator.Branches(block, yes, no);
    private static void Add(GuestValidationContext context, string message) => context.Add("ASIR1037", message);
}
