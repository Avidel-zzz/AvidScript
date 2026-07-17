using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestOperationTests
{
    private const string LocalId = "symbol:local:main:value";
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        ScalarStateLocalsCallsAndConditionalsAreLowered();
        LoopBackEdgesArePreserved();
        DiscardAssignmentsEvaluateTheirRightHandSide();
        return 3;
    }

    private static void ScalarStateLocalsCallsAndConditionalsAreLowered()
    {
        SemanticDocument document = WithGraph(CreateConditionalGraph());

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(FormatDiagnostics(result));
        GuestFunction function = module.Functions.Single(
            item => item.Id == CSharpGuestIdsForTests.Function(CSharpGuestSemanticFixture.MainMethodId));
        GuestInstruction[] instructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "scalar operation module should lower and validate");
        Assert(instructions.Any(instruction => instruction.Op == "global_store"),
            "state assignment should lower to global_store");
        Assert(instructions.Any(instruction => instruction.Op == "local_store")
            && instructions.Any(instruction => instruction.Op == "local_load"),
            "mutable C# local should lower through explicit local storage");
        Assert(instructions.Any(instruction => instruction.Op == "call"
            && instruction.TargetId == $"import:{CSharpGuestSemanticFixture.HostMethodId}"),
            "semantic host invocation should target projected import identity");
        Assert(instructions.Any(instruction => instruction.Op == "binary"
            && instruction.OperatorKind == "greater_than"),
            "comparison should remain a typed Guest binary instruction");
        Assert(function.Blocks.Any(block => block.Terminator.Kind == "branch_if"),
            "semantic conditional edges should lower to branch_if");
    }

    private static void LoopBackEdgesArePreserved()
    {
        SemanticDocument document = WithGraph(CreateLoopGraph());

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(FormatDiagnostics(result));
        GuestFunction function = module.Functions.Single(
            item => item.Id == CSharpGuestIdsForTests.Function(CSharpGuestSemanticFixture.MainMethodId));
        string conditionBlockId = CSharpGuestIdsForTests.Block(CSharpGuestSemanticFixture.MainMethodId, 1);
        GuestBasicBlock body = function.Blocks.Single(
            block => block.Id == CSharpGuestIdsForTests.Block(CSharpGuestSemanticFixture.MainMethodId, 2));

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "loop operation module should lower and validate");
        Assert(body.Terminator.Kind == "branch" && body.Terminator.TargetBlockId == conditionBlockId,
            "loop body should retain its CFG back-edge");
    }

    private static void DiscardAssignmentsEvaluateTheirRightHandSide()
    {
        SemanticOperation call = CSharpGuestSemanticFixture.Operation(
            "invocation",
            "type:int32",
            CSharpGuestSemanticFixture.HostMethodId,
            new[]
            {
                Argument(Literal(2)),
                Argument(Literal(3)),
            });
        SemanticOperation discard = CSharpGuestSemanticFixture.Operation("discard", null);
        SemanticOperation assignment = Assign(discard, call);
        SemanticControlFlowEdge returned = Edge(0, 1, "fallthrough", "return");
        SemanticControlFlowGraph graph = new(
            CSharpGuestSemanticFixture.MainMethodId,
            0,
            1,
            new[]
            {
                Block(0, new[] { assignment }, null, "none", Array.Empty<SemanticControlFlowEdge>(), new[] { returned }),
                Block(1, Array.Empty<SemanticOperation>(), null, "none", new[] { returned }, Array.Empty<SemanticControlFlowEdge>(), "exit"),
            });

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(WithGraph(graph), SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(FormatDiagnostics(result));
        GuestInstruction[] instructions = module.Functions
            .Single(item => item.Id == CSharpGuestIdsForTests.Function(CSharpGuestSemanticFixture.MainMethodId))
            .Blocks.SelectMany(block => block.Instructions)
            .ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "discard assignment module should lower and validate");
        Assert(instructions.Count(instruction => instruction.Op == "call") == 1,
            "discard assignment should evaluate its invocation exactly once");
    }
    private static SemanticDocument WithGraph(SemanticControlFlowGraph graph)
    {
        SemanticDocument document = CSharpGuestSemanticFixture.Create();
        SemanticSymbol local = new(
            LocalId,
            "local",
            "value",
            CSharpGuestSemanticFixture.MainMethodId,
            "type:int32",
            "int32 value",
            false,
            "private",
            new SemanticSpan(1, 1, 0, 0, 0, 1));
        return document with
        {
            Symbols = document.Symbols.Append(local).ToArray(),
            ControlFlowGraphs = new[] { graph },
        };
    }

    private static SemanticControlFlowGraph CreateConditionalGraph()
    {
        SemanticOperation initializeState = Assign(Field(), Literal(1));
        SemanticOperation call = CSharpGuestSemanticFixture.Operation(
            "invocation",
            "type:int32",
            CSharpGuestSemanticFixture.HostMethodId,
            new[]
            {
                Argument(Literal(2)),
                Argument(Literal(3)),
            });
        SemanticOperation storeLocal = Assign(Local(), call);
        SemanticOperation condition = Binary("greater_than", Local(), Literal(0), "type:bool");
        SemanticOperation thenStore = Assign(Field(), Binary("add", Local(), Literal(1), "type:int32"));
        SemanticOperation elseStore = Assign(Field(), Literal(0));

        SemanticControlFlowEdge entry = Edge(0, 1);
        SemanticControlFlowEdge whenTrue = Edge(1, 2);
        SemanticControlFlowEdge whenFalse = Edge(1, 3, "conditional");
        SemanticControlFlowEdge thenJoin = Edge(2, 4);
        SemanticControlFlowEdge elseJoin = Edge(3, 4);
        SemanticControlFlowEdge returned = Edge(4, 5, "fallthrough", "return");
        return new SemanticControlFlowGraph(
            CSharpGuestSemanticFixture.MainMethodId,
            0,
            5,
            new[]
            {
                Block(0, new[] { initializeState }, null, "none", Array.Empty<SemanticControlFlowEdge>(), new[] { entry }),
                Block(1, new[] { storeLocal }, condition, "when_false", new[] { entry }, new[] { whenTrue, whenFalse }),
                Block(2, new[] { thenStore }, null, "none", new[] { whenTrue }, new[] { thenJoin }),
                Block(3, new[] { elseStore }, null, "none", new[] { whenFalse }, new[] { elseJoin }),
                Block(4, Array.Empty<SemanticOperation>(), null, "none", new[] { thenJoin, elseJoin }, new[] { returned }),
                Block(5, Array.Empty<SemanticOperation>(), null, "none", new[] { returned }, Array.Empty<SemanticControlFlowEdge>(), "exit"),
            });
    }

    private static SemanticControlFlowGraph CreateLoopGraph()
    {
        SemanticOperation initialize = Assign(Local(), Literal(0));
        SemanticOperation condition = Binary("less_than", Local(), Literal(3), "type:bool");
        SemanticOperation increment = Assign(Local(), Binary("add", Local(), Literal(1), "type:int32"));
        SemanticControlFlowEdge entry = Edge(0, 1);
        SemanticControlFlowEdge body = Edge(1, 2);
        SemanticControlFlowEdge exit = Edge(1, 3, "conditional");
        SemanticControlFlowEdge back = Edge(2, 1);
        SemanticControlFlowEdge returned = Edge(3, 4, "fallthrough", "return");
        return new SemanticControlFlowGraph(
            CSharpGuestSemanticFixture.MainMethodId,
            0,
            4,
            new[]
            {
                Block(0, new[] { initialize }, null, "none", Array.Empty<SemanticControlFlowEdge>(), new[] { entry }),
                Block(1, Array.Empty<SemanticOperation>(), condition, "when_false", new[] { entry, back }, new[] { body, exit }),
                Block(2, new[] { increment }, null, "none", new[] { body }, new[] { back }),
                Block(3, Array.Empty<SemanticOperation>(), null, "none", new[] { exit }, new[] { returned }),
                Block(4, Array.Empty<SemanticOperation>(), null, "none", new[] { returned }, Array.Empty<SemanticControlFlowEdge>(), "exit"),
            });
    }

    private static SemanticBasicBlock Block(
        int ordinal,
        SemanticOperation[] operations,
        SemanticOperation? branchValue,
        string conditionKind,
        SemanticControlFlowEdge[] predecessors,
        SemanticControlFlowEdge[] successors,
        string kind = "block")
    {
        return new SemanticBasicBlock(
            ordinal,
            ordinal == 0 ? "entry" : kind,
            true,
            conditionKind,
            operations,
            branchValue,
            predecessors,
            successors);
    }

    private static SemanticControlFlowEdge Edge(
        int source,
        int destination,
        string kind = "fallthrough",
        string semantics = "regular")
    {
        return new SemanticControlFlowEdge(source, destination, kind, semantics);
    }

    private static SemanticOperation Assign(SemanticOperation target, SemanticOperation value)
    {
        return CSharpGuestSemanticFixture.Operation(
            "assignment",
            value.TypeId,
            children: new[] { target, value });
    }

    private static SemanticOperation Binary(
        string operatorKind,
        SemanticOperation left,
        SemanticOperation right,
        string typeId)
    {
        return CSharpGuestSemanticFixture.Operation(
            "binary",
            typeId,
            children: new[] { left, right },
            operatorKind: operatorKind);
    }

    private static SemanticOperation Field()
    {
        return CSharpGuestSemanticFixture.Operation(
            "field_reference",
            "type:int32",
            CSharpGuestSemanticFixture.StateFieldId);
    }

    private static SemanticOperation Local()
    {
        return CSharpGuestSemanticFixture.Operation("local_reference", "type:int32", LocalId);
    }

    private static SemanticOperation Literal(int value)
    {
        return CSharpGuestSemanticFixture.Operation(
            "literal",
            "type:int32",
            constant: new SemanticConstant("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    }

    private static SemanticOperation Argument(SemanticOperation value)
    {
        return CSharpGuestSemanticFixture.Operation("argument", null, children: new[] { value });
    }

    private static string FormatDiagnostics(CSharpGuestLoweringResult result)
    {
        return string.Join(" | ", result.Diagnostics.Select(diagnostic => $"{diagnostic.Code}:{diagnostic.Message}"));
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

internal static class CSharpGuestIdsForTests
{
    public static string Function(string methodId) => $"function:{methodId}";

    public static string Block(string methodId, int ordinal) => $"block:{methodId}:{ordinal}";
}
