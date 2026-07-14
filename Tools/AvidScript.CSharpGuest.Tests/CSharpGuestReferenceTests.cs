using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestReferenceTests
{
    private const string MethodId = "symbol:method:global::Game.Script.Bump(ref int32):void";
    private const string ParameterId = "symbol:parameter:bump:0";
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        ReferenceParameterReadsAndWritesAreIndirect();
        return 1;
    }

    private static void ReferenceParameterReadsAndWritesAreIndirect()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticCallable bump = new(
            MethodId,
            "type:global::Game.Script",
            "type:void",
            new[] { new SemanticCallableParameter(0, ParameterId, "value", "type:int32", "ref") },
            true,
            false,
            true,
            null,
            null,
            null);
        SemanticOperation parameter = CSharpGuestSemanticFixture.Operation(
            "parameter_reference", "type:int32", ParameterId);
        SemanticOperation assignment = CSharpGuestSemanticFixture.Operation(
            "assignment",
            "type:int32",
            children: new[] { parameter, Literal(9) });
        SemanticOperation read = CSharpGuestSemanticFixture.Operation(
            "assignment",
            "type:int32",
            children: new[]
            {
                CSharpGuestSemanticFixture.Operation(
                    "field_reference", "type:int32", CSharpGuestSemanticFixture.StateFieldId),
                parameter,
            });
        SemanticDocument document = baseline with
        {
            Callables = baseline.Callables.Append(bump).ToArray(),
            ControlFlowGraphs = baseline.ControlFlowGraphs.Append(
                ReturnGraph(new[] { assignment, read })).ToArray(),
        };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestFunction function = module.Functions.Single(item => item.Id == $"function:{MethodId}");
        GuestInstruction[] instructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "reference parameter module should lower and validate");
        Assert(function.Parameters.Single().TypeId == "type:address",
            "reference parameter ABI should expose an address value");
        Assert(instructions.Any(item => item.Op == "indirect_store" && item.TargetId == "type:int32")
            && instructions.Any(item => item.Op == "indirect_load" && item.TargetId == "type:int32"),
            "reference parameter access should preserve its pointee type");
    }

    private static SemanticControlFlowGraph ReturnGraph(SemanticOperation[] operations)
    {
        SemanticControlFlowEdge entry = new(0, 1, "fallthrough", "regular");
        SemanticControlFlowEdge returned = new(1, 2, "fallthrough", "return");
        return new SemanticControlFlowGraph(
            MethodId,
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
