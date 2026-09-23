using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

// A returned outcome must be constructed on every path with status 0 plus a
// value, or status 1 plus error type and source. The managed root starts null
// in the backend frame and may be populated by a later object-owning contract.
internal static class GuestLanguageOutcomeProducerValidator
{
    private const int MaximumBlocks = 4096;
    private const int ValueField = 1;
    private const int ErrorTypeField = 2;
    private const int SourceField = 4;
    private const string DiagnosticCode = "ASIR1026";

    private readonly record struct ConstantDefinition(string BlockId, int InstructionIndex, int Value);
    private readonly record struct FlowState(string BlockId, bool Allocated, int Status, int Fields);

    public static void Validate(GuestValidationContext context, IReadOnlySet<string> outcomeTypes)
    {
        foreach (GuestFunction function in context.Module.Functions)
        {
            if (!outcomeTypes.Contains(function.ReturnTypeId)) continue;
            if (function.Blocks.Count > MaximumBlocks)
            {
                Add(context, $"Function '{function.Id}' exceeds the bounded outcome producer CFG.");
                continue;
            }
            if (!context.Types.TryGetValue(function.ReturnTypeId, out GuestType? outcome)
                || outcome.Fields.Count is not (4 or 5)) continue;

            Dictionary<string, GuestBasicBlock> blocks = function.Blocks
                .Where(block => !string.IsNullOrWhiteSpace(block.Id))
                .GroupBy(block => block.Id, StringComparer.Ordinal)
                .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
            Dictionary<string, string> definitions = new(StringComparer.Ordinal);
            Dictionary<string, ConstantDefinition> constants = new(StringComparer.Ordinal);
            HashSet<string> mutableConstants = new(StringComparer.Ordinal);
            foreach (GuestBasicBlock block in function.Blocks)
            {
                for (int index = 0; index < block.Instructions.Count; ++index)
                {
                    GuestInstruction instruction = block.Instructions[index];
                    if (instruction.ResultId is { } resultId)
                    {
                        definitions.TryAdd(resultId, instruction.Op);
                        if (instruction.Op == "constant" && instruction.Constant is
                            { Kind: "int32", Value: "0" or "1" } constant)
                            constants.TryAdd(resultId, new ConstantDefinition(block.Id, index,
                                constant.Value == "0" ? 0 : 1));
                    }
                    if (instruction.Op is "local_store" or "address_of" or "borrow_address"
                        && instruction.TargetId is { } targetId)
                        mutableConstants.Add(targetId);
                }
            }

            HashSet<string> checkedAllocations = new(StringComparer.Ordinal);
            foreach (GuestBasicBlock block in function.Blocks)
            {
                if (block.Terminator.Kind != "return" || block.Terminator.ReturnValueId is not { } resultId)
                    continue;
                if (!definitions.TryGetValue(resultId, out string? definition))
                {
                    Add(context, $"Function '{function.Id}' returns an outcome without a defined producer.");
                    continue;
                }
                if (definition is "call" or "call_indirect")
                {
                    // The call-flow validator requires the return to be on the
                    // error-only successor of the checked call.
                    continue;
                }
                if (definition != "stack_alloc")
                {
                    Add(context, $"Function '{function.Id}' returns an outcome through an untracked producer '{definition}'.");
                    continue;
                }
                if (checkedAllocations.Add(resultId))
                    ValidateAllocation(context, function, blocks, outcome, resultId,
                        constants, mutableConstants);
            }
        }
    }

    private static void ValidateAllocation(
        GuestValidationContext context,
        GuestFunction function,
        IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        GuestType outcome,
        string resultId,
        IReadOnlyDictionary<string, ConstantDefinition> constants,
        IReadOnlySet<string> mutableConstants)
    {
        if (!blocks.ContainsKey(function.EntryBlockId)) return;
        Queue<FlowState> pending = new();
        HashSet<FlowState> seen = new();
        pending.Enqueue(new FlowState(function.EntryBlockId, false, -1, 0));
        while (pending.TryDequeue(out FlowState state))
        {
            if (!seen.Add(state) || !blocks.TryGetValue(state.BlockId, out GuestBasicBlock? block)) continue;
            bool allocated = state.Allocated;
            int status = state.Status;
            int fields = state.Fields;
            for (int index = 0; index < block.Instructions.Count; ++index)
            {
                GuestInstruction instruction = block.Instructions[index];
                if (instruction.Op == "stack_alloc" && instruction.ResultId == resultId)
                {
                    allocated = true;
                    status = -1;
                    fields = 0;
                    continue;
                }
                if (instruction.Op == "field_store" && instruction.OperandIds.Count == 2
                    && instruction.OperandIds[0] == resultId)
                {
                    if (!allocated)
                    {
                        Add(context, $"Function '{function.Id}' writes outcome '{resultId}' before allocation.");
                        return;
                    }
                    if (instruction.TargetId == outcome.Fields[0].Id)
                    {
                        string valueId = instruction.OperandIds[1];
                        if (!constants.TryGetValue(valueId, out ConstantDefinition value)
                            || mutableConstants.Contains(valueId)
                            || !Dominates(blocks, function.EntryBlockId, value, block.Id, index))
                        {
                            Add(context, $"Function '{function.Id}' writes an unproven outcome status.");
                            return;
                        }
                        status = value.Value;
                    }
                    else if (instruction.TargetId == outcome.Fields[1].Id) fields |= ErrorTypeField;
                    else if (instruction.TargetId == outcome.Fields[2].Id) fields |= SourceField;
                    else if (outcome.Fields.Count == 5 && instruction.TargetId == outcome.Fields[4].Id)
                        fields |= ValueField;
                    continue;
                }
                if (instruction.OperandIds.Contains(resultId, StringComparer.Ordinal)
                    || instruction.TargetId == resultId)
                {
                    if (instruction.Op != "field_load" || instruction.OperandIds.Count != 1
                        || instruction.OperandIds[0] != resultId)
                    {
                        Add(context, $"Function '{function.Id}' aliases or escapes outcome producer '{resultId}'.");
                        return;
                    }
                    bool readingValue = outcome.Fields.Count == 5
                        && instruction.TargetId == outcome.Fields[4].Id;
                    bool readingUnsetStatus = instruction.TargetId == outcome.Fields[0].Id && status < 0;
                    bool readingError = outcome.Fields.Skip(1).Take(3)
                        .Any(field => field.Id == instruction.TargetId);
                    if (!allocated || readingUnsetStatus
                        || readingValue && status != GuestLanguageOutcomeType.SuccessStatus
                        || readingError && status != GuestLanguageOutcomeType.LanguageErrorStatus)
                    {
                        Add(context, $"Function '{function.Id}' reads outcome producer '{resultId}' before the matching state is established.");
                        return;
                    }
                }
            }

            GuestTerminator terminator = block.Terminator;
            if (terminator.Kind == "return" && terminator.ReturnValueId == resultId)
            {
                bool complete = allocated && (status == GuestLanguageOutcomeType.SuccessStatus
                    && (outcome.Fields.Count == 4 || (fields & ValueField) != 0)
                    || status == GuestLanguageOutcomeType.LanguageErrorStatus
                    && (fields & (ErrorTypeField | SourceField)) == (ErrorTypeField | SourceField));
                if (!complete)
                {
                    Add(context, $"Function '{function.Id}' returns outcome '{resultId}' with uninitialized or invalid state fields.");
                    return;
                }
            }
            if (terminator.Kind is "branch" or "branch_if"
                && terminator.TargetBlockId is { } target)
                pending.Enqueue(new FlowState(target, allocated, status, fields));
            if (terminator.Kind == "branch_if" && terminator.FalseTargetBlockId is { } falseTarget)
                pending.Enqueue(new FlowState(falseTarget, allocated, status, fields));
        }
    }

    private static bool Dominates(
        IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        string entry,
        ConstantDefinition definition,
        string storeBlockId,
        int storeInstructionIndex)
    {
        if (definition.BlockId == storeBlockId)
            return definition.InstructionIndex < storeInstructionIndex;
        Queue<string> pending = new();
        HashSet<string> seen = new(StringComparer.Ordinal);
        pending.Enqueue(entry);
        while (pending.TryDequeue(out string? blockId))
        {
            if (blockId == definition.BlockId || !seen.Add(blockId)) continue;
            if (blockId == storeBlockId) return false;
            if (!blocks.TryGetValue(blockId, out GuestBasicBlock? block)) continue;
            GuestTerminator terminator = block.Terminator;
            if (terminator.Kind is "branch" or "branch_if"
                && terminator.TargetBlockId is { } target) pending.Enqueue(target);
            if (terminator.Kind == "branch_if" && terminator.FalseTargetBlockId is { } falseTarget)
                pending.Enqueue(falseTarget);
        }
        return true;
    }

    private static void Add(GuestValidationContext context, string message) =>
        context.Add(DiagnosticCode, message);
}
