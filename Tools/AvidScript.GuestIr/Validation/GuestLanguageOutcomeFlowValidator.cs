using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

// IR 16 and later admit outcome-returning calls only when the call result is
// split by its status before any normal value or error payload is read.
internal static class GuestLanguageOutcomeFlowValidator
{
    public const int SchemaVersion = 16;
    public const string IrVersion = "1.15";
    private const string DiagnosticCode = "ASIR1025";

    public static void Validate(GuestValidationContext context)
    {
        IReadOnlyList<GuestLanguageOutcomeType>? declarations = context.Module.LanguageOutcomeTypes;
        if (declarations is null || declarations.Count == 0) return;
        HashSet<string> outcomeTypes = declarations.Select(item => item.TypeId)
            .ToHashSet(StringComparer.Ordinal);
        bool flowVersion = context.Module.SchemaVersion == SchemaVersion
                && context.Module.IrVersion == IrVersion
            || context.Module.SchemaVersion == GuestLanguageErrorCatalogValidator.SchemaVersion
                && context.Module.IrVersion == GuestLanguageErrorCatalogValidator.IrVersion
            || context.Module.SchemaVersion == GuestTaskLanguageErrorValidator.SchemaVersion
                && context.Module.IrVersion == GuestTaskLanguageErrorValidator.IrVersion;

        foreach (GuestImport import in context.Module.Imports)
        {
            if (outcomeTypes.Contains(import.ReturnTypeId))
                Add(context, $"Host import '{import.Id}' cannot return a language outcome; Host failures remain fatal.");
            if (import.ParameterTypeIds.Any(outcomeTypes.Contains))
                Add(context, $"Host import '{import.Id}' cannot receive a language outcome without a boundary adapter.");
        }
        foreach (GuestFunctionReference reference in context.Module.FunctionReferences)
        {
            if (outcomeTypes.Contains(reference.ReturnTypeId) && !flowVersion)
                Add(context, $"Function reference '{reference.TypeId}' requires Guest IR 16/1.15 for language outcomes.");
            if (reference.ParameterTypeIds.Any(outcomeTypes.Contains))
                Add(context, $"Function reference '{reference.TypeId}' cannot pass an unchecked language outcome.");
        }
        foreach (GuestFunction function in context.Module.Functions)
        {
            if (outcomeTypes.Contains(function.ReturnTypeId) && !flowVersion)
                Add(context, $"Function '{function.Id}' requires Guest IR 16/1.15 to return a language outcome.");
            if (function.Parameters.Any(parameter => outcomeTypes.Contains(parameter.TypeId)))
                Add(context, $"Function '{function.Id}' cannot receive an unchecked language outcome parameter.");
        }
        if (!flowVersion) return;

        foreach (GuestExport export in context.Module.Exports)
            if (context.Functions.TryGetValue(export.FunctionId, out GuestFunction? target)
                && outcomeTypes.Contains(target.ReturnTypeId))
                Add(context, $"Export '{export.Name}' needs an explicit Host boundary adapter for language errors.");
        foreach (GuestFramedExport export in context.Module.FramedExports)
            if (context.Functions.TryGetValue(export.FunctionId, out GuestFunction? target)
                && outcomeTypes.Contains(target.ReturnTypeId))
                Add(context, $"Framed export '{export.Name}' needs an explicit Host boundary adapter for language errors.");

        foreach (GuestFunction function in context.Module.Functions)
        {
            Dictionary<string, GuestBasicBlock> blocks = function.Blocks
                .Where(block => !string.IsNullOrWhiteSpace(block.Id))
                .GroupBy(block => block.Id, StringComparer.Ordinal)
                .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
            foreach (GuestBasicBlock block in function.Blocks)
            {
                for (int index = 0; index < block.Instructions.Count; ++index)
                {
                    GuestInstruction instruction = block.Instructions[index];
                    string? returnTypeId = CallReturnType(context, instruction);
                    if (returnTypeId is null || !outcomeTypes.Contains(returnTypeId)) continue;
                    if (instruction.ResultId is null
                        || !context.Types.TryGetValue(returnTypeId, out GuestType? outcome)
                        || outcome.Fields.Count is not (4 or 5)
                        || index != block.Instructions.Count - 2
                        || block.Instructions[index + 1] is not { Op: "field_load" } statusLoad
                        || statusLoad.OperandIds.Count != 1
                        || statusLoad.OperandIds[0] != instruction.ResultId
                        || statusLoad.TargetId != outcome.Fields[0].Id
                        || statusLoad.ResultId is null
                        || block.Terminator.Kind != "branch_if"
                        || block.Terminator.ConditionValueId != statusLoad.ResultId
                        || block.Terminator.TargetBlockId is null
                        || block.Terminator.FalseTargetBlockId is null
                        || block.Terminator.TargetBlockId == block.Terminator.FalseTargetBlockId)
                    {
                        Add(context, $"Function '{function.Id}' must branch on the status immediately after outcome call '{instruction.TargetId}'.");
                        continue;
                    }

                    ValidateResultUses(context, function, blocks, block, index,
                        instruction.ResultId, outcome);
                }
            }
        }
        GuestLanguageOutcomeProducerValidator.Validate(context, outcomeTypes);
    }

    private static string? CallReturnType(GuestValidationContext context, GuestInstruction instruction)
    {
        if (instruction.Op == "call" && instruction.TargetId is { } targetId)
        {
            if (context.Functions.TryGetValue(targetId, out GuestFunction? function))
                return function.ReturnTypeId;
            if (context.Imports.TryGetValue(targetId, out GuestImport? import))
                return import.ReturnTypeId;
        }
        if (instruction.Op == "call_indirect" && instruction.TargetId is { } typeId)
            return context.Module.FunctionReferences.FirstOrDefault(reference => reference.TypeId == typeId)?.ReturnTypeId;
        return null;
    }

    private static void ValidateResultUses(
        GuestValidationContext context,
        GuestFunction function,
        IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        GuestBasicBlock callBlock,
        int callIndex,
        string resultId,
        GuestType outcome)
    {
        HashSet<string> errorReach = Reachable(blocks, callBlock.Terminator.TargetBlockId!);
        HashSet<string> successReach = Reachable(blocks, callBlock.Terminator.FalseTargetBlockId!);
        for (int blockIndex = 0; blockIndex < function.Blocks.Count; ++blockIndex)
        {
            GuestBasicBlock block = function.Blocks[blockIndex];
            for (int instructionIndex = 0; instructionIndex < block.Instructions.Count; ++instructionIndex)
            {
                GuestInstruction use = block.Instructions[instructionIndex];
                if (!use.OperandIds.Contains(resultId, StringComparer.Ordinal)
                    && use.TargetId != resultId) continue;
                if (ReferenceEquals(block, callBlock) && instructionIndex == callIndex + 1) continue;
                bool successOnly = successReach.Contains(block.Id) && !errorReach.Contains(block.Id);
                bool errorOnly = errorReach.Contains(block.Id) && !successReach.Contains(block.Id);
                bool valid = use.Op == "field_load" && use.OperandIds.Count == 1
                    && ((outcome.Fields.Count == 5 && use.TargetId == outcome.Fields[4].Id && successOnly)
                        || (outcome.Fields.Skip(1).Take(3).Any(field => field.Id == use.TargetId) && errorOnly));
                if (!valid)
                    Add(context, $"Function '{function.Id}' reads or escapes outcome '{resultId}' outside its status-qualified path.");
            }
            if (block.Terminator.ReturnValueId == resultId
                && (function.ReturnTypeId != outcome.Id
                    || !errorReach.Contains(block.Id) || successReach.Contains(block.Id)))
                Add(context, $"Function '{function.Id}' propagates outcome '{resultId}' outside the error path.");
        }
    }

    private static HashSet<string> Reachable(
        IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        string start)
    {
        HashSet<string> visited = new(StringComparer.Ordinal);
        Stack<string> pending = new();
        pending.Push(start);
        while (pending.TryPop(out string? id))
        {
            if (!visited.Add(id) || !blocks.TryGetValue(id, out GuestBasicBlock? block)) continue;
            if (block.Terminator.Kind is "branch" or "branch_if"
                && block.Terminator.TargetBlockId is { } target) pending.Push(target);
            if (block.Terminator.Kind == "branch_if"
                && block.Terminator.FalseTargetBlockId is { } falseTarget) pending.Push(falseTarget);
        }
        return visited;
    }

    private static void Add(GuestValidationContext context, string message) =>
        context.Add(DiagnosticCode, message);
}
