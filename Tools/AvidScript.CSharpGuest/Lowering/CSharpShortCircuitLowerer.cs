using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed class CSharpShortCircuitLowerer
{
    private readonly Dictionary<string, Expression> expressions = new(StringComparer.Ordinal);

    public void Record(
        GuestRegister result,
        GuestRegister left,
        GuestRegister right,
        GuestRegister merge,
        int rightInstructionCount,
        bool isAnd)
    {
        expressions.Add(result.Id, new Expression(result, left,
            isAnd ? right : left, isAnd ? left : right, merge,
            isAnd ? rightInstructionCount : 0, isAnd ? 0 : rightInstructionCount, isAnd));
    }

    public void RecordConditional(GuestRegister result, GuestRegister condition,
        GuestRegister whenTrue, GuestRegister whenFalse, GuestRegister merge,
        int trueInstructionCount, int falseInstructionCount)
    {
        expressions.Add(result.Id, new Expression(result, condition, whenTrue, whenFalse,
            merge, trueInstructionCount, falseInstructionCount, null));
    }

    public bool Rewrite(CSharpFunctionLoweringContext context, List<GuestBasicBlock> blocks)
    {
        if (expressions.Count == 0)
        {
            return true;
        }

        List<GuestBasicBlock> rewritten = new();
        HashSet<string> consumed = new(StringComparer.Ordinal);
        int nextBlockOrdinal = 0;
        foreach (GuestBasicBlock block in blocks)
        {
            List<Range> ranges = new();
            for (int index = 0; index < block.Instructions.Count; ++index)
            {
                GuestInstruction instruction = block.Instructions[index];
                if (instruction.ResultId is not { } resultId
                    || !expressions.TryGetValue(resultId, out Expression? expression))
                {
                    continue;
                }
                int start = index - expression.TrueInstructionCount - expression.FalseInstructionCount;
                bool validMarker = expression.ShortCircuitAnd is { } isAnd
                    ? instruction.Op == "binary" && instruction.OperatorKind == (isAnd ? "logical_and" : "logical_or")
                    : instruction.Op == "local_load" && instruction.TargetId == expression.Merge.Id;
                if (start < 0 || !validMarker
                    || !consumed.Add(resultId))
                {
                    return Fail(context);
                }
                ranges.Add(new Range(start, index, expression));
            }
            if (ranges.Count == 0)
            {
                rewritten.Add(block);
                continue;
            }

            // Private instruction ranges cover both ?: arms or the lazy operand
            // of && / ||. Nested choices share this pass so splitting one does not
            // strand another expression's instructions in different blocks.
            ranges = ranges.OrderBy(range => range.Start).ThenByDescending(range => range.End).ToList();
            int nextRange = 0;
            string currentId = block.Id;
            List<GuestInstruction> current = new();

            bool Emit(int start, int end)
            {
                int index = start;
                while (index < end)
                {
                    if (nextRange >= ranges.Count || ranges[nextRange].Start > index)
                    {
                        current.Add(block.Instructions[index++]);
                        continue;
                    }
                    Range range = ranges[nextRange++];
                    if (range.Start != index || range.End >= end)
                    {
                        return false;
                    }
                    Expression expression = range.Expression;
                    GuestDebugLocation? location = block.Instructions[range.End].DebugLocation;
                    string prefix = $"{block.Id}:short_circuit_{nextBlockOrdinal++}";
                    string trueId = prefix + ":true";
                    string falseId = prefix + ":false";
                    string joinId = prefix + ":join";
                    rewritten.Add(new GuestBasicBlock(currentId, current.ToArray(),
                        new GuestTerminator("branch_if", expression.Condition.Id, trueId, falseId, null)));

                    if (!context.TryGetGuestType(expression.Result.TypeId, out GuestType type)) return false;
                    GuestInstruction Store(GuestRegister value) => type.Storage == "memory"
                        ? new("memory_copy", null, new[] { expression.Merge.Id, value.Id }, type.Id, null, null, location)
                        : new("local_store", null, new[] { value.Id }, expression.Merge.Id, null, null, location);
                    int falseStart = range.Start + expression.TrueInstructionCount;
                    currentId = trueId;
                    current = new List<GuestInstruction>();
                    if (!Emit(range.Start, falseStart))
                    {
                        return false;
                    }
                    current.Add(Store(expression.WhenTrue));
                    rewritten.Add(new GuestBasicBlock(currentId, current.ToArray(),
                        new GuestTerminator("branch", null, joinId, null, null)));
                    currentId = falseId;
                    current = new List<GuestInstruction>();
                    if (!Emit(falseStart, range.End)) return false;
                    current.Add(Store(expression.WhenFalse));
                    rewritten.Add(new GuestBasicBlock(currentId, current.ToArray(),
                        new GuestTerminator("branch", null, joinId, null, null)));
                    currentId = joinId;
                    current = new List<GuestInstruction>
                    {
                        type.Storage == "memory"
                            ? new("memory_copy", null, new[] { expression.Result.Id, expression.Merge.Id }, type.Id, null, null, location)
                            : new("local_load", expression.Result.Id, Array.Empty<string>(), expression.Merge.Id, null, null, location),
                    };
                    index = range.End + 1;
                }
                return true;
            }

            if (!Emit(0, block.Instructions.Count) || nextRange != ranges.Count)
            {
                return Fail(context);
            }
            rewritten.Add(new GuestBasicBlock(currentId, current.ToArray(), block.Terminator));
        }
        if (consumed.Count != expressions.Count)
        {
            return Fail(context);
        }
        blocks.Clear();
        blocks.AddRange(rewritten);
        return true;
    }

    private static bool Fail(CSharpFunctionLoweringContext context)
    {
        context.Add("ASCG1004", "Conditional expression instructions do not form nested ranges within a basic block.");
        return false;
    }

    private sealed record Expression(
        GuestRegister Result,
        GuestRegister Condition,
        GuestRegister WhenTrue,
        GuestRegister WhenFalse,
        GuestRegister Merge,
        int TrueInstructionCount,
        int FalseInstructionCount,
        bool? ShortCircuitAnd);

    private sealed record Range(int Start, int End, Expression Expression);
}
