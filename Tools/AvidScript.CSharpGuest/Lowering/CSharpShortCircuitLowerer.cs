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
        expressions.Add(result.Id, new Expression(
            result, left, right, merge, rightInstructionCount, isAnd));
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
                int start = index - expression.RightInstructionCount;
                if (start < 0 || instruction.Op != "binary"
                    || instruction.OperatorKind != (expression.IsAnd ? "logical_and" : "logical_or")
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

            // Ranges describe existing instructions, not an additional IR opcode. The outer
            // RHS comes first when nested expressions begin at the same instruction.
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
                    string rightId = prefix + ":right";
                    string joinId = prefix + ":join";
                    current.Add(new GuestInstruction(
                        "local_store", null, new[] { expression.Left.Id }, expression.Merge.Id,
                        null, null));
                    rewritten.Add(new GuestBasicBlock(currentId, current.ToArray(),
                        new GuestTerminator("branch_if", expression.Left.Id,
                            expression.IsAnd ? rightId : joinId,
                            expression.IsAnd ? joinId : rightId, null)));

                    currentId = rightId;
                    current = new List<GuestInstruction>();
                    if (!Emit(range.Start, range.End))
                    {
                        return false;
                    }
                    current.Add(new GuestInstruction(
                        "local_store", null, new[] { expression.Right.Id }, expression.Merge.Id,
                        null, null));
                    rewritten.Add(new GuestBasicBlock(currentId, current.ToArray(),
                        new GuestTerminator("branch", null, joinId, null, null)));
                    currentId = joinId;
                    current = new List<GuestInstruction>
                    {
                        new("local_load", expression.Result.Id, Array.Empty<string>(),
                            expression.Merge.Id, null, null, location),
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
        context.Add("ASCG1004", "Short-circuit expression instructions do not form nested ranges within a basic block.");
        return false;
    }

    private sealed record Expression(
        GuestRegister Result,
        GuestRegister Left,
        GuestRegister Right,
        GuestRegister Merge,
        int RightInstructionCount,
        bool IsAnd);

    private sealed record Range(int Start, int End, Expression Expression);
}
