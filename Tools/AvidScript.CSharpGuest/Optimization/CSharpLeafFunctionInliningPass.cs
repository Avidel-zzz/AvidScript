using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpLeafFunctionInliningResult(
    IReadOnlyList<GuestFunction> Functions,
    int InlinedCallCount);

internal static class CSharpLeafFunctionInliningPass
{
    private const int MaximumCandidateInstructionCount = 8;
    private const int MaximumExpandedInstructionCountPerFunction = 96;
    private const int MinimumArithmeticInstructionCount = 2;

    public static CSharpLeafFunctionInliningResult Run(
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestFunction> functions)
    {
        ArgumentNullException.ThrowIfNull(types);
        ArgumentNullException.ThrowIfNull(functions);

        IReadOnlyDictionary<string, GuestType> typesById = types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        Dictionary<string, InlineCandidate> candidates = functions
            .Select(function => TryCreateCandidate(typesById, function))
            .Where(candidate => candidate is not null)
            .Select(candidate => candidate!)
            .ToDictionary(candidate => candidate.Function.Id, StringComparer.Ordinal);
        if (candidates.Count == 0)
        {
            return new CSharpLeafFunctionInliningResult(functions, 0);
        }

        int inlinedCallCount = 0;
        GuestFunction[] transformed = functions
            .Select(function => RewriteFunction(function, candidates, ref inlinedCallCount))
            .ToArray();
        return new CSharpLeafFunctionInliningResult(transformed, inlinedCallCount);
    }

    private static InlineCandidate? TryCreateCandidate(
        IReadOnlyDictionary<string, GuestType> types,
        GuestFunction function)
    {
        Dictionary<string, GuestRegister> registers = function.Parameters
            .Concat(function.Locals)
            .ToDictionary(register => register.Id, StringComparer.Ordinal);
        if (!registers.Values.All(register => IsScalar(types, register.TypeId))
            || !IsScalar(types, function.ReturnTypeId)
            || !TryFlatten(function, out IReadOnlyList<GuestInstruction> instructions, out string returnValueId)
            || instructions.Count > MaximumCandidateInstructionCount
            || instructions.Count(instruction => instruction.Op is "binary" or "convert")
                < MinimumArithmeticInstructionCount
            || !registers.ContainsKey(returnValueId))
        {
            return null;
        }

        HashSet<string> parameterIds = function.Parameters
            .Select(parameter => parameter.Id)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> availableValues = new(parameterIds, StringComparer.Ordinal);
        for (int index = 0; index < instructions.Count; ++index)
        {
            GuestInstruction instruction = instructions[index];
            if (instruction.Op == "local_load")
            {
                if (instruction.ResultId is null
                    || instruction.TargetId is null
                    || !parameterIds.Contains(instruction.TargetId))
                {
                    return null;
                }
            }
            else if (instruction.Op is not ("constant" or "copy" or "binary" or "convert")
                || instruction.ResultId is null
                || instruction.OperandIds.Any(operandId => !availableValues.Contains(operandId)))
            {
                return null;
            }

            availableValues.Add(instruction.ResultId);
            if (instruction.ResultId == returnValueId && index != instructions.Count - 1)
            {
                return null;
            }
        }

        return availableValues.Contains(returnValueId)
            ? new InlineCandidate(function, instructions, returnValueId, registers)
            : null;
    }

    private static bool TryFlatten(
        GuestFunction function,
        out IReadOnlyList<GuestInstruction> instructions,
        out string returnValueId)
    {
        Dictionary<string, GuestBasicBlock> blocks = function.Blocks.ToDictionary(
            block => block.Id,
            StringComparer.Ordinal);
        HashSet<string> visited = new(StringComparer.Ordinal);
        List<GuestInstruction> flattened = new();
        string blockId = function.EntryBlockId;
        while (blocks.TryGetValue(blockId, out GuestBasicBlock? block)
            && visited.Add(blockId))
        {
            flattened.AddRange(block.Instructions);
            if (block.Terminator.Kind == "branch"
                && block.Terminator.TargetBlockId is not null)
            {
                blockId = block.Terminator.TargetBlockId;
                continue;
            }
            if (block.Terminator.Kind == "return"
                && block.Terminator.ReturnValueId is not null)
            {
                instructions = flattened;
                returnValueId = block.Terminator.ReturnValueId;
                return true;
            }
            break;
        }

        instructions = Array.Empty<GuestInstruction>();
        returnValueId = string.Empty;
        return false;
    }

    private static GuestFunction RewriteFunction(
        GuestFunction function,
        IReadOnlyDictionary<string, InlineCandidate> candidates,
        ref int totalInlinedCallCount)
    {
        List<GuestRegister> locals = function.Locals.ToList();
        HashSet<string> usedIds = function.Parameters
            .Concat(function.Locals)
            .Select(register => register.Id)
            .ToHashSet(StringComparer.Ordinal);
        int expandedInstructionCount = 0;
        int functionInlineOrdinal = 0;
        bool changed = false;
        List<GuestBasicBlock> blocks = new(function.Blocks.Count);
        foreach (GuestBasicBlock block in function.Blocks)
        {
            List<GuestInstruction> rewritten = new(block.Instructions.Count);
            for (int instructionIndex = 0; instructionIndex < block.Instructions.Count; ++instructionIndex)
            {
                GuestInstruction instruction = block.Instructions[instructionIndex];
                if (instruction.Op == "call"
                    && instruction.TargetId is not null
                    && instruction.ResultId is not null
                    && candidates.TryGetValue(instruction.TargetId, out InlineCandidate? candidate)
                    && expandedInstructionCount + candidate.Instructions.Count
                        <= MaximumExpandedInstructionCountPerFunction
                    && TryInline(
                        instruction,
                        candidate,
                        functionInlineOrdinal,
                        usedIds,
                        out IReadOnlyList<GuestInstruction> expanded,
                        out IReadOnlyList<GuestRegister> addedLocals))
                {
                    rewritten.AddRange(expanded);
                    locals.AddRange(addedLocals);
                    expandedInstructionCount += expanded.Count;
                    ++functionInlineOrdinal;
                    ++totalInlinedCallCount;
                    changed = true;
                    continue;
                }

                rewritten.Add(instruction);
            }
            blocks.Add(changed
                ? block with { Instructions = rewritten }
                : block);
        }

        return changed
            ? function with { Locals = locals, Blocks = blocks }
            : function;
    }

    private static bool TryInline(
        GuestInstruction call,
        InlineCandidate candidate,
        int inlineOrdinal,
        HashSet<string> usedIds,
        out IReadOnlyList<GuestInstruction> expanded,
        out IReadOnlyList<GuestRegister> addedLocals)
    {
        if (call.OperandIds.Count != candidate.Function.Parameters.Count)
        {
            expanded = Array.Empty<GuestInstruction>();
            addedLocals = Array.Empty<GuestRegister>();
            return false;
        }

        Dictionary<string, string> values = new(StringComparer.Ordinal);
        for (int index = 0; index < call.OperandIds.Count; ++index)
        {
            values.Add(candidate.Function.Parameters[index].Id, call.OperandIds[index]);
        }

        List<GuestInstruction> instructions = new(candidate.Instructions.Count);
        List<GuestRegister> locals = new();
        int localOrdinal = 0;
        foreach (GuestInstruction instruction in candidate.Instructions)
        {
            if (instruction.Op == "local_load")
            {
                if (instruction.ResultId is null
                    || instruction.TargetId is null
                    || !values.TryGetValue(instruction.TargetId, out string? sourceId))
                {
                    expanded = Array.Empty<GuestInstruction>();
                    addedLocals = Array.Empty<GuestRegister>();
                    return false;
                }
                values.Add(instruction.ResultId, sourceId);
                if (instruction.ResultId == candidate.ReturnValueId)
                {
                    instructions.Add(new GuestInstruction(
                        "copy",
                        call.ResultId,
                        new[] { sourceId },
                        null,
                        null,
                        null,
                        call.DebugLocation));
                }
                continue;
            }

            if (instruction.ResultId is null
                || instruction.OperandIds.Any(operandId => !values.ContainsKey(operandId)))
            {
                expanded = Array.Empty<GuestInstruction>();
                addedLocals = Array.Empty<GuestRegister>();
                return false;
            }

            string resultId;
            if (instruction.ResultId == candidate.ReturnValueId)
            {
                resultId = call.ResultId!;
            }
            else
            {
                string prefix = $"{call.ResultId}:inline:{inlineOrdinal}:{localOrdinal}";
                resultId = AllocateId(prefix, usedIds);
                GuestRegister sourceRegister = candidate.Registers[instruction.ResultId];
                locals.Add(new GuestRegister(resultId, sourceRegister.TypeId));
                ++localOrdinal;
            }

            string[] operands = instruction.OperandIds
                .Select(operandId => values[operandId])
                .ToArray();
            values.Add(instruction.ResultId, resultId);
            instructions.Add(instruction with
            {
                ResultId = resultId,
                OperandIds = operands,
                DebugLocation = call.DebugLocation,
            });
        }

        expanded = instructions;
        addedLocals = locals;
        return values.ContainsKey(candidate.ReturnValueId) && instructions.Count != 0;
    }

    private static string AllocateId(string prefix, HashSet<string> usedIds)
    {
        string candidate = prefix;
        int collisionOrdinal = 0;
        while (!usedIds.Add(candidate))
        {
            candidate = $"{prefix}:{++collisionOrdinal}";
        }
        return candidate;
    }

    private static bool IsScalar(
        IReadOnlyDictionary<string, GuestType> types,
        string typeId)
    {
        return types.TryGetValue(typeId, out GuestType? type)
            && type.Kind == "scalar"
            && type.Storage is "i32" or "i64" or "f32" or "f64";
    }

    private sealed record InlineCandidate(
        GuestFunction Function,
        IReadOnlyList<GuestInstruction> Instructions,
        string ReturnValueId,
        IReadOnlyDictionary<string, GuestRegister> Registers);
}
