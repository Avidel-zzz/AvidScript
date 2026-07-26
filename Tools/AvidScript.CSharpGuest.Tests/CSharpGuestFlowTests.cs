using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestFlowTests
{
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        FlowCapturesAndCompoundAssignmentsUseExplicitStorage();
        AssignmentsCanStoreIntoExistingFlowCaptures();
        return 2;
    }

    private static void FlowCapturesAndCompoundAssignmentsUseExplicitStorage()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticOperation state = CSharpGuestSemanticFixture.Operation(
            "field_reference",
            "type:int32",
            CSharpGuestSemanticFixture.StateFieldId);
        SemanticOperation compound = CSharpGuestSemanticFixture.Operation(
            "compound_assignment",
            "type:int32",
            children: new[] { state, Literal(2) },
            operatorKind: "add");
        SemanticOperation capture = CSharpGuestSemanticFixture.Operation(
            "flow_capture",
            null,
            children: new[] { Literal(7) },
            captureId: "capture:0");
        SemanticOperation captureReference = CSharpGuestSemanticFixture.Operation(
            "flow_capture_reference",
            "type:int32",
            captureId: "capture:0");
        SemanticOperation assignment = CSharpGuestSemanticFixture.Operation(
            "assignment",
            "type:int32",
            children: new[] { state, captureReference });
        SemanticControlFlowGraph graph = ReturnGraph(new[] { compound, capture, assignment });
        SemanticDocument document = baseline with { ControlFlowGraphs = new[] { graph } };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "flow and compound assignment module should lower and validate");
        Assert(instructions.Any(item => item.Op == "binary" && item.OperatorKind == "add")
            && instructions.Count(item => item.Op == "global_store") == 2,
            "compound assignment should load, calculate, and store state exactly once");
        Assert(instructions.Any(item => item.Op == "local_store")
            && instructions.Any(item => item.Op == "local_load"),
            "flow capture should use a stable explicit storage slot");
    }

    private static void AssignmentsCanStoreIntoExistingFlowCaptures()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticOperation capture = CSharpGuestSemanticFixture.Operation(
            "flow_capture",
            null,
            children: new[] { Literal(3) },
            captureId: "capture:0");
        SemanticOperation captureReference = CSharpGuestSemanticFixture.Operation(
            "flow_capture_reference",
            "type:int32",
            captureId: "capture:0");
        SemanticOperation captureAssignment = CSharpGuestSemanticFixture.Operation(
            "assignment",
            "type:int32",
            children: new[] { captureReference, Literal(9) });
        SemanticOperation state = CSharpGuestSemanticFixture.Operation(
            "field_reference",
            "type:int32",
            CSharpGuestSemanticFixture.StateFieldId);
        SemanticOperation stateAssignment = CSharpGuestSemanticFixture.Operation(
            "assignment",
            "type:int32",
            children: new[] { state, captureReference });
        SemanticControlFlowGraph graph = ReturnGraph(
            new[] { capture, captureAssignment, stateAssignment });
        SemanticDocument document = baseline with { ControlFlowGraphs = new[] { graph } };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "flow capture assignment module should lower and validate");
        Assert(instructions.Count(item => item.Op == "local_store") == 2
            && instructions.Count(item => item.Op == "global_store") == 1,
            "capture creation and overwrite should share one explicit storage slot");
    }

    private static SemanticControlFlowGraph ReturnGraph(SemanticOperation[] operations)
    {
        SemanticControlFlowEdge entry = new(0, 1, "fallthrough", "regular");
        SemanticControlFlowEdge returned = new(1, 2, "fallthrough", "return");
        return new SemanticControlFlowGraph(
            CSharpGuestSemanticFixture.MainMethodId,
            0,
            2,
            new[]
            {
                new SemanticBasicBlock(0, "entry", true, "none", Array.Empty<SemanticOperation>(), null,
                    Array.Empty<SemanticControlFlowEdge>(), new[] { entry }),
                new SemanticBasicBlock(1, "block", true, "none", operations, null,
                    new[] { entry }, new[] { returned }),
                new SemanticBasicBlock(2, "exit", true, "none", Array.Empty<SemanticOperation>(), null,
                    new[] { returned }, Array.Empty<SemanticControlFlowEdge>()),
            });
    }

    private static SemanticOperation Literal(int value)
    {
        return CSharpGuestSemanticFixture.Operation(
            "literal",
            "type:int32",
            constant: new SemanticConstant(
                "int32",
                value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
