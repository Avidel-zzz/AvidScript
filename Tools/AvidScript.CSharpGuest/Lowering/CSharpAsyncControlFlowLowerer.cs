using System;
using System.Collections.Generic;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed class CSharpAsyncControlFlowLowerer
{
    private readonly CSharpFunctionLoweringContext context;
    private readonly int segmentOrdinal;
    private readonly string segmentBlockId;
    private readonly List<GuestBasicBlock> blocks;
    private int nextBlockOrdinal;
    private int nextDebugOrdinal;

    public CSharpAsyncControlFlowLowerer(
        CSharpFunctionLoweringContext context,
        int segmentOrdinal,
        string segmentBlockId,
        List<GuestBasicBlock> blocks)
    {
        this.context = context;
        this.segmentOrdinal = segmentOrdinal;
        this.segmentBlockId = segmentBlockId;
        this.blocks = blocks;
    }

    public bool Emit(
        SemanticOperation operation,
        string activeBlockId,
        List<GuestInstruction> instructions,
        out string nextBlockId,
        out List<GuestInstruction> nextInstructions,
        out bool canContinue)
    {
        Cursor? cursor = EmitOperation(
            operation,
            new Cursor(activeBlockId, instructions, true),
            FlowTargets.Method);
        nextBlockId = cursor?.BlockId ?? activeBlockId;
        nextInstructions = cursor?.Instructions ?? instructions;
        canContinue = cursor?.CanContinue ?? false;
        return cursor is not null;
    }

    public static bool IsStructuredFlow(SemanticOperation operation)
    {
        return operation.Kind is
            SemanticAsyncMethod.BlockOperationKind or
            SemanticAsyncMethod.LocalDeclarationOperationKind or
            SemanticAsyncMethod.IfOperationKind or
            SemanticAsyncMethod.WhileOperationKind or
            SemanticAsyncMethod.DoWhileOperationKind or
            SemanticAsyncMethod.ForOperationKind or
            SemanticAsyncMethod.BreakOperationKind or
            SemanticAsyncMethod.ContinueOperationKind or
            SemanticAsyncMethod.ReturnOperationKind;
    }

    private Cursor? EmitOperation(
        SemanticOperation operation,
        Cursor cursor,
        FlowTargets targets)
    {
        if (!cursor.CanContinue)
        {
            return cursor;
        }

        return operation.Kind switch
        {
            SemanticAsyncMethod.BlockOperationKind => EmitBlock(operation, cursor, targets),
            SemanticAsyncMethod.LocalDeclarationOperationKind => EmitLocalDeclaration(operation, cursor),
            SemanticAsyncMethod.IfOperationKind => EmitIf(operation, cursor, targets),
            SemanticAsyncMethod.WhileOperationKind => EmitWhile(operation, cursor, targets),
            SemanticAsyncMethod.DoWhileOperationKind => EmitDoWhile(operation, cursor, targets),
            SemanticAsyncMethod.ForOperationKind => EmitFor(operation, cursor, targets),
            SemanticAsyncMethod.BreakOperationKind => EmitLoopTransfer(operation, cursor, targets.Break),
            SemanticAsyncMethod.ContinueOperationKind => EmitLoopTransfer(operation, cursor, targets.Continue),
            SemanticAsyncMethod.ReturnOperationKind => EmitReturn(operation, cursor),
            _ => EmitValue(operation, cursor),
        };
    }

    private Cursor? EmitBlock(
        SemanticOperation operation,
        Cursor cursor,
        FlowTargets targets)
    {
        Cursor? current = cursor;
        foreach (SemanticOperation child in operation.Children)
        {
            current = EmitOperation(child, current, targets);
            if (current is null || !current.CanContinue)
            {
                return current;
            }
        }
        return current;
    }

    private Cursor? EmitLocalDeclaration(
        SemanticOperation operation,
        Cursor cursor)
    {
        if (operation.SymbolId is null
            || operation.TypeId is null
            || operation.Children.Count > 1)
        {
            Add("Async local declaration has an invalid structured-flow shape.");
            return null;
        }
        if (operation.Children.Count == 0)
        {
            return cursor;
        }

        int instructionStart = cursor.Instructions.Count;
        GuestRegister? value = CSharpOperationLowerer.LowerValue(
            context,
            operation.Children[0],
            segmentOrdinal,
            cursor.Instructions);
        if (value is null
            || !CSharpOperationLowerer.StoreLocal(
                context,
                operation.SymbolId,
                value,
                segmentOrdinal,
                cursor.Instructions))
        {
            return null;
        }
        TagFirstEmitted(cursor.Instructions, instructionStart, operation);
        return cursor;
    }

    private Cursor? EmitIf(
        SemanticOperation operation,
        Cursor cursor,
        FlowTargets targets)
    {
        if (operation.Children.Count is not (2 or 3))
        {
            Add("Async if operation has an invalid structured-flow shape.");
            return null;
        }

        GuestRegister? condition = LowerCondition(operation.Children[0], cursor.Instructions);
        if (condition is null)
        {
            return null;
        }

        string whenTrueId = NextBlockId("if_true");
        string joinId = NextBlockId("if_join");
        string whenFalseId = operation.Children.Count == 3
            ? NextBlockId("if_false")
            : joinId;
        blocks.Add(new GuestBasicBlock(
            cursor.BlockId,
            cursor.Instructions.ToArray(),
            new GuestTerminator(
                "branch_if",
                condition.Id,
                whenTrueId,
                whenFalseId,
                null)));

        Cursor? whenTrue = EmitOperation(
            operation.Children[1],
            Empty(whenTrueId),
            targets);
        if (whenTrue is null)
        {
            return null;
        }
        bool trueContinues = whenTrue.CanContinue;
        if (trueContinues)
        {
            BranchTo(whenTrue, joinId);
        }

        bool falseContinues = operation.Children.Count == 2;
        if (operation.Children.Count == 3)
        {
            Cursor? whenFalse = EmitOperation(
                operation.Children[2],
                Empty(whenFalseId),
                targets);
            if (whenFalse is null)
            {
                return null;
            }
            falseContinues = whenFalse.CanContinue;
            if (falseContinues)
            {
                BranchTo(whenFalse, joinId);
            }
        }

        return trueContinues || falseContinues
            ? Empty(joinId)
            : new Cursor(joinId, new List<GuestInstruction>(), false);
    }

    private Cursor? EmitWhile(
        SemanticOperation operation,
        Cursor cursor,
        FlowTargets outerTargets)
    {
        if (operation.Children.Count != 2)
        {
            Add("Async while operation has an invalid structured-flow shape.");
            return null;
        }

        string conditionId = NextBlockId("while_condition");
        string bodyId = NextBlockId("while_body");
        string exitId = NextBlockId("while_exit");
        BranchTo(cursor, conditionId);

        List<GuestInstruction> conditionInstructions = new();
        GuestRegister? condition = LowerCondition(operation.Children[0], conditionInstructions);
        if (condition is null)
        {
            return null;
        }
        blocks.Add(new GuestBasicBlock(
            conditionId,
            conditionInstructions,
            new GuestTerminator("branch_if", condition.Id, bodyId, exitId, null)));

        Cursor? body = EmitOperation(
            operation.Children[1],
            Empty(bodyId),
            outerTargets with { Break = exitId, Continue = conditionId });
        if (body is null)
        {
            return null;
        }
        if (body.CanContinue)
        {
            BranchTo(body, conditionId);
        }
        return Empty(exitId);
    }

    private Cursor? EmitDoWhile(
        SemanticOperation operation,
        Cursor cursor,
        FlowTargets outerTargets)
    {
        if (operation.Children.Count != 2)
        {
            Add("Async do/while operation has an invalid structured-flow shape.");
            return null;
        }

        string bodyId = NextBlockId("do_body");
        string conditionId = NextBlockId("do_condition");
        string exitId = NextBlockId("do_exit");
        BranchTo(cursor, bodyId);

        Cursor? body = EmitOperation(
            operation.Children[0],
            Empty(bodyId),
            outerTargets with { Break = exitId, Continue = conditionId });
        if (body is null)
        {
            return null;
        }
        if (body.CanContinue)
        {
            BranchTo(body, conditionId);
        }

        List<GuestInstruction> conditionInstructions = new();
        GuestRegister? condition = LowerCondition(operation.Children[1], conditionInstructions);
        if (condition is null)
        {
            return null;
        }
        blocks.Add(new GuestBasicBlock(
            conditionId,
            conditionInstructions,
            new GuestTerminator("branch_if", condition.Id, bodyId, exitId, null)));
        return Empty(exitId);
    }

    private Cursor? EmitFor(
        SemanticOperation operation,
        Cursor cursor,
        FlowTargets outerTargets)
    {
        if (operation.Children.Count != 4)
        {
            Add("Async for operation has an invalid structured-flow shape.");
            return null;
        }

        Cursor? initialized = EmitOperation(operation.Children[0], cursor, outerTargets);
        if (initialized is null || !initialized.CanContinue)
        {
            return initialized;
        }

        string conditionId = NextBlockId("for_condition");
        string bodyId = NextBlockId("for_body");
        string incrementId = NextBlockId("for_increment");
        string exitId = NextBlockId("for_exit");
        BranchTo(initialized, conditionId);

        List<GuestInstruction> conditionInstructions = new();
        GuestRegister? condition = LowerCondition(operation.Children[1], conditionInstructions);
        if (condition is null)
        {
            return null;
        }
        blocks.Add(new GuestBasicBlock(
            conditionId,
            conditionInstructions,
            new GuestTerminator("branch_if", condition.Id, bodyId, exitId, null)));

        Cursor? body = EmitOperation(
            operation.Children[3],
            Empty(bodyId),
            outerTargets with { Break = exitId, Continue = incrementId });
        if (body is null)
        {
            return null;
        }
        if (body.CanContinue)
        {
            BranchTo(body, incrementId);
        }

        Cursor? increment = EmitOperation(
            operation.Children[2],
            Empty(incrementId),
            outerTargets);
        if (increment is null)
        {
            return null;
        }
        if (increment.CanContinue)
        {
            BranchTo(increment, conditionId);
        }
        return Empty(exitId);
    }

    private Cursor? EmitLoopTransfer(
        SemanticOperation operation,
        Cursor cursor,
        string? targetBlockId)
    {
        if (operation.Children.Count != 0 || targetBlockId is null)
        {
            Add("Async loop transfer has no compatible loop target.");
            return null;
        }
        blocks.Add(new GuestBasicBlock(
            cursor.BlockId,
            cursor.Instructions.ToArray(),
            new GuestTerminator(
                "branch",
                null,
                targetBlockId,
                null,
                null,
                DebugLocation(operation, "statement"))));
        return new Cursor(cursor.BlockId, new List<GuestInstruction>(), false);
    }

    private Cursor? EmitReturn(
        SemanticOperation operation,
        Cursor cursor)
    {
        if (operation.Children.Count != 0)
        {
            Add("Async return operation must not carry a value.");
            return null;
        }
        blocks.Add(new GuestBasicBlock(
            cursor.BlockId,
            cursor.Instructions.ToArray(),
            new GuestTerminator(
                "return",
                null,
                null,
                null,
                null,
                DebugLocation(operation, "return"))));
        return new Cursor(cursor.BlockId, new List<GuestInstruction>(), false);
    }

    private Cursor? EmitValue(
        SemanticOperation operation,
        Cursor cursor)
    {
        int before = context.Diagnostics.Count;
        int instructionStart = cursor.Instructions.Count;
        _ = CSharpOperationLowerer.LowerValue(
            context,
            operation,
            segmentOrdinal,
            cursor.Instructions);
        if (context.Diagnostics.Count == before)
        {
            TagFirstEmitted(cursor.Instructions, instructionStart, operation);
        }
        return context.Diagnostics.Count == before ? cursor : null;
    }

    private GuestRegister? LowerCondition(
        SemanticOperation operation,
        List<GuestInstruction> instructions)
    {
        int instructionStart = instructions.Count;
        GuestRegister? condition = CSharpOperationLowerer.LowerValue(
            context,
            operation,
            segmentOrdinal,
            instructions);
        if (condition?.TypeId == "type:bool")
        {
            TagFirstEmitted(instructions, instructionStart, operation);
            return condition;
        }
        Add("Async structured-flow condition did not lower to canonical bool storage.");
        return null;
    }

    private void BranchTo(Cursor cursor, string targetBlockId)
    {
        blocks.Add(new GuestBasicBlock(
            cursor.BlockId,
            cursor.Instructions.ToArray(),
            new GuestTerminator("branch", null, targetBlockId, null, null)));
    }

    private Cursor Empty(string blockId)
    {
        return new Cursor(blockId, new List<GuestInstruction>(), true);
    }

    private string NextBlockId(string kind)
    {
        return $"{segmentBlockId}:flow_{nextBlockOrdinal++}_{kind}";
    }

    private void TagFirstEmitted(
        IList<GuestInstruction> instructions,
        int startIndex,
        SemanticOperation operation)
    {
        CSharpGuestDebugTagger.TagFirstEmitted(
            instructions,
            startIndex,
            operation,
            NextDebugOperationId());
    }

    private GuestDebugLocation DebugLocation(
        SemanticOperation operation,
        string kind)
    {
        return CSharpGuestDebugTagger.Create(
            operation.Span,
            NextDebugOperationId(),
            kind);
    }

    private string NextDebugOperationId()
    {
        return CSharpGuestDebugTagger.OperationId(
            context.Callable.MethodSymbolId,
            $"async:{segmentOrdinal}:structured",
            nextDebugOrdinal++);
    }

    private void Add(string message)
    {
        context.Add("ASCG1015", $"Async segment {segmentOrdinal}: {message}");
    }

    private sealed record Cursor(
        string BlockId,
        List<GuestInstruction> Instructions,
        bool CanContinue);

    private sealed record FlowTargets(
        string? Break,
        string? Continue)
    {
        public static FlowTargets Method { get; } = new(null, null);
    }
}
