using System;
using System.Globalization;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestTaskErrorTransferValidator
{
    internal static void Validate(GuestValidationContext context)
    {
        GuestModule artifact = context.Artifact;
        if (!GuestTaskErrorTransfers.IsVersion(artifact))
        {
            if (artifact.TaskErrorTransfers is not null)
                Add(context, "Task error transfers require Guest IR 28/1.27.");
            return;
        }
        if (artifact.TaskErrorTransfers is not { BaseSchemaVersion: >= 20 and <= 26,
                Sites: { Count: > 0 and <= 4096 } } plan
            || plan.BaseIrVersion != "1." + (plan.BaseSchemaVersion - 1).ToString(CultureInfo.InvariantCulture)
            || artifact.LanguageErrorCatalog is not { Types.Count: > 0, Sources.Count: > 0 }
            || artifact.LanguageOutcomeTypes is not { Count: > 0 }
            || artifact.StaticStorage is { } storage && (storage.BaseSchemaVersion != plan.BaseSchemaVersion
                || storage.BaseIrVersion != plan.BaseIrVersion))
        {
            Add(context, "Task error transfers require a bounded plan, catalog, outcomes and one exact execution profile.");
            return;
        }
        foreach (var site in plan.Sites)
        {
            var key = (site.FunctionId, site.ErrorBlockId, site.FaultInstructionIndex);
            if (context.CheckedTaskErrorTransfers.Contains(key) || !ValidSite(context, site))
                Add(context, "Task error transfer has an invalid or duplicate checked outcome path: " + site.FunctionId);
            else context.CheckedTaskErrorTransfers.Add(key);
        }
        foreach (var function in artifact.Functions)
        foreach (var block in function.Blocks)
        for (int index = 0; index < block.Instructions.Count; ++index)
        {
            var instruction = block.Instructions[index];
            if (instruction.Op == "call" && instruction.TargetId == GuestTaskLanguageErrorValidator.ImportId
                && !GuestTaskLanguageErrorValidator.HasFreshRoot(block, index)
                && !context.CheckedTaskErrorTransfers.Contains((function.Id, block.Id, index)))
                Add(context, "Task error transfer is missing a checked outcome site: " + function.Id);
        }
    }

    private static bool ValidSite(GuestValidationContext context, GuestTaskErrorTransferSite site)
    {
        if (!context.Functions.TryGetValue(site.FunctionId, out var function)) return false;
        var calls = function.Blocks.Where(block => block.Id == site.CallBlockId).ToArray();
        var errors = function.Blocks.Where(block => block.Id == site.ErrorBlockId).ToArray();
        if (calls.Length != 1 || errors.Length != 1 || site.CallBlockId == site.ErrorBlockId
            || site.FaultInstructionIndex < 3 || site.FaultInstructionIndex >= errors[0].Instructions.Count
            || site.CallInstructionIndex < 0 || site.CallInstructionIndex + 2 != calls[0].Instructions.Count) return false;
        var callBlock = calls[0];
        var errorBlock = errors[0];
        var call = callBlock.Instructions[site.CallInstructionIndex];
        var status = callBlock.Instructions[site.CallInstructionIndex + 1];
        var fault = errorBlock.Instructions[site.FaultInstructionIndex];
        if (call.Op != "call" || call.ResultId is not { } outcome || call.TargetId is not { } callee
            || !context.Functions.TryGetValue(callee, out var target)
            || !context.Module.LanguageOutcomeTypes!.Any(type => type.TypeId == target.ReturnTypeId)
            || status.Op != "field_load" || status.ResultId is not { } condition
            || status.TargetId != "field:status" || !status.OperandIds.SequenceEqual(new[] { outcome })
            || callBlock.Terminator.Kind != "branch_if" || callBlock.Terminator.ConditionValueId != condition
            || callBlock.Terminator.TargetBlockId != site.ErrorBlockId
            || callBlock.Terminator.FalseTargetBlockId == site.ErrorBlockId
            || fault.Op != "call" || fault.TargetId != GuestTaskLanguageErrorValidator.ImportId
            || fault.OperandIds.Count != 4) return false;
        // An extra predecessor would bypass the failed-status check.
        if (function.EntryBlockId == site.ErrorBlockId || function.Blocks.Any(block => block.Id != site.CallBlockId
            && (block.Terminator.TargetBlockId == site.ErrorBlockId || block.Terminator.FalseTargetBlockId == site.ErrorBlockId))) return false;
        var instructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();
        bool Unique(string id) => instructions.Count(item => item.ResultId == id) == 1
            && instructions.All(item => item.TargetId != id);
        if (!Unique(outcome) || !Unique(condition)
            || instructions.Any(item => item.OperandIds.Contains(outcome)
                && (item.Op != "field_load" || item.OperandIds.Count != 1))) return false;
        string[] fields = { "field:error_type", "field:source", "field:error_root" };
        for (int index = 0; index < fields.Length; ++index)
        {
            var load = errorBlock.Instructions[site.FaultInstructionIndex - 3 + index];
            if (load.Op != "field_load" || load.TargetId != fields[index]
                || !load.OperandIds.SequenceEqual(new[] { outcome })
                || load.ResultId != fault.OperandIds[index + 1] || load.ResultId is null
                || !Unique(load.ResultId)) return false;
        }
        return true;
    }

    private static void Add(GuestValidationContext context, string message) => context.Add("ASIR1036", message);
}
