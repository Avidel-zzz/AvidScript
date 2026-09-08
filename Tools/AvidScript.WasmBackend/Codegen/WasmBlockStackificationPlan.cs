using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal sealed record WasmStackifiedExpression(
    int RootInstructionIndex,
    IReadOnlyDictionary<string, int> ProducerIndicesByResultId);

internal sealed class WasmBlockStackificationPlan
{
    private const int MinimumDeferredInstructionCount = 2;

    private readonly IReadOnlyDictionary<int, WasmStackifiedExpression> expressionsByRoot;
    private readonly IReadOnlySet<int> deferredInstructionIndices;

    private WasmBlockStackificationPlan(
        IReadOnlyDictionary<int, WasmStackifiedExpression> expressionsByRoot,
        IReadOnlySet<int> deferredInstructionIndices)
    {
        this.expressionsByRoot = expressionsByRoot;
        this.deferredInstructionIndices = deferredInstructionIndices;
    }

    public static WasmBlockStackificationPlan Create(
        GuestBasicBlock block,
        Func<string, bool> isScalarValue)
    {
        ArgumentNullException.ThrowIfNull(block);
        ArgumentNullException.ThrowIfNull(isScalarValue);

        Dictionary<string, int> producers = new(StringComparer.Ordinal);
        Dictionary<string, int> useCounts = new(StringComparer.Ordinal);
        Dictionary<string, List<int>> instructionUsers = new(StringComparer.Ordinal);
        for (int index = 0; index < block.Instructions.Count; ++index)
        {
            GuestInstruction instruction = block.Instructions[index];
            if (IsPureProducer(instruction, isScalarValue))
            {
                producers.Add(instruction.ResultId!, index);
            }
            foreach (string operandId in instruction.OperandIds)
            {
                Increment(useCounts, operandId);
                if (!instructionUsers.TryGetValue(operandId, out List<int>? users))
                {
                    users = new List<int>();
                    instructionUsers.Add(operandId, users);
                }
                users.Add(index);
            }
        }

        Increment(useCounts, block.Terminator.ConditionValueId);
        Increment(useCounts, block.Terminator.ReturnValueId);

        Dictionary<int, WasmStackifiedExpression> expressions = new();
        HashSet<int> deferred = new();
        for (int rootIndex = block.Instructions.Count - 1; rootIndex >= 0; --rootIndex)
        {
            GuestInstruction root = block.Instructions[rootIndex];
            if (!IsSupportedRoot(root, isScalarValue)
                || IsForwardedToLaterInstruction(root, rootIndex, useCounts, instructionUsers))
            {
                continue;
            }

            List<int> emissionOrder = new();
            Dictionary<string, int> expressionProducers = new(StringComparer.Ordinal);
            HashSet<int> visiting = new();
            foreach (string operandId in root.OperandIds)
            {
                CollectProducer(
                    operandId,
                    rootIndex,
                    block,
                    producers,
                    useCounts,
                    expressionProducers,
                    emissionOrder,
                    visiting);
            }

            if (emissionOrder.Count < MinimumDeferredInstructionCount
                || emissionOrder.Any(deferred.Contains))
            {
                continue;
            }
            int firstIndex = rootIndex - emissionOrder.Count;
            if (firstIndex < 0
                || !emissionOrder.SequenceEqual(
                    Enumerable.Range(firstIndex, emissionOrder.Count)))
            {
                continue;
            }

            expressions.Add(
                rootIndex,
                new WasmStackifiedExpression(rootIndex, expressionProducers));
            deferred.UnionWith(emissionOrder);
        }

        return new WasmBlockStackificationPlan(expressions, deferred);
    }

    public bool IsDeferred(int instructionIndex)
    {
        return deferredInstructionIndices.Contains(instructionIndex);
    }

    public bool TryGetExpression(
        int instructionIndex,
        out WasmStackifiedExpression expression)
    {
        return expressionsByRoot.TryGetValue(instructionIndex, out expression!);
    }

    private static void CollectProducer(
        string valueId,
        int consumerIndex,
        GuestBasicBlock block,
        IReadOnlyDictionary<string, int> producers,
        IReadOnlyDictionary<string, int> useCounts,
        IDictionary<string, int> expressionProducers,
        ICollection<int> emissionOrder,
        ISet<int> visiting)
    {
        if (!producers.TryGetValue(valueId, out int producerIndex)
            || producerIndex >= consumerIndex
            || !useCounts.TryGetValue(valueId, out int useCount)
            || useCount != 1
            || !visiting.Add(producerIndex))
        {
            return;
        }

        GuestInstruction producer = block.Instructions[producerIndex];
        foreach (string operandId in producer.OperandIds)
        {
            CollectProducer(
                operandId,
                producerIndex,
                block,
                producers,
                useCounts,
                expressionProducers,
                emissionOrder,
                visiting);
        }
        expressionProducers.Add(valueId, producerIndex);
        emissionOrder.Add(producerIndex);
        visiting.Remove(producerIndex);
    }

    private static bool IsForwardedToLaterInstruction(
        GuestInstruction instruction,
        int instructionIndex,
        IReadOnlyDictionary<string, int> useCounts,
        IReadOnlyDictionary<string, List<int>> instructionUsers)
    {
        return IsPureProducer(instruction, _ => true)
            && instruction.ResultId is not null
            && useCounts.TryGetValue(instruction.ResultId, out int useCount)
            && useCount == 1
            && instructionUsers.TryGetValue(instruction.ResultId, out List<int>? users)
            && users.Count == 1
            && users[0] > instructionIndex;
    }

    private static bool IsSupportedRoot(
        GuestInstruction instruction,
        Func<string, bool> isScalarValue)
    {
        if (IsPureProducer(instruction, isScalarValue))
        {
            return true;
        }
        return instruction.Op == "local_store"
            && instruction.ResultId is null
            && instruction.TargetId is not null
            && instruction.OperandIds.Count == 1
            && isScalarValue(instruction.TargetId);
    }

    private static bool IsPureProducer(
        GuestInstruction instruction,
        Func<string, bool> isScalarValue)
    {
        if (instruction.ResultId is null
            || !isScalarValue(instruction.ResultId)
            || instruction.OperandIds.Any(operandId => !isScalarValue(operandId)))
        {
            return false;
        }
        return instruction.Op switch
        {
            "constant" => true,
            "copy" => true,
            "binary" => true,
            "convert" => true,
            "local_load" => instruction.TargetId is not null
                && isScalarValue(instruction.TargetId),
            _ => false,
        };
    }

    private static void Increment(IDictionary<string, int> counts, string? valueId)
    {
        if (valueId is null)
        {
            return;
        }
        counts[valueId] = counts.TryGetValue(valueId, out int count) ? count + 1 : 1;
    }
}
