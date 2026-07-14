using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestOperatorTests
{
    private const string PointTypeId = "type:global::Game.Point";
    private const string OperatorId = "symbol:method:global::Game.Point.op_Addition(Point,Point):Point";
    private const string CallerId = "symbol:method:global::Game.Script.Add(Point,Point):Point";
    private const string LeftId = "symbol:parameter:caller:0";
    private const string RightId = "symbol:parameter:caller:1";
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        BoundUserOperatorLowersToCallable();
        return 1;
    }

    private static void BoundUserOperatorLowersToCallable()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticCallable operatorCallable = new(
            OperatorId,
            PointTypeId,
            PointTypeId,
            new[]
            {
                new SemanticCallableParameter(0, "symbol:parameter:operator:0", "left", PointTypeId, "none"),
                new SemanticCallableParameter(1, "symbol:parameter:operator:1", "right", PointTypeId, "none"),
            },
            true,
            false,
            false,
            null,
            new SemanticCallableImport("env", "point_add"),
            null);
        SemanticCallable caller = new(
            CallerId,
            "type:global::Game.Script",
            PointTypeId,
            new[]
            {
                new SemanticCallableParameter(0, LeftId, "left", PointTypeId, "none"),
                new SemanticCallableParameter(1, RightId, "right", PointTypeId, "none"),
            },
            true,
            false,
            true,
            null,
            null,
            null);
        SemanticOperation addition = CSharpGuestSemanticFixture.Operation(
            "binary",
            PointTypeId,
            OperatorId,
            new[]
            {
                CSharpGuestSemanticFixture.Operation("parameter_reference", PointTypeId, LeftId),
                CSharpGuestSemanticFixture.Operation("parameter_reference", PointTypeId, RightId),
            },
            operatorKind: "add");
        SemanticDocument document = baseline with
        {
            Callables = baseline.Callables.Concat(new[] { operatorCallable, caller }).ToArray(),
            ControlFlowGraphs = baseline.ControlFlowGraphs.Append(ReturnGraph(addition)).ToArray(),
        };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestFunction function = module.Functions.Single(item => item.Id == $"function:{CallerId}");
        GuestInstruction[] instructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "bound operator module should lower and validate");
        Assert(instructions.Any(item => item.Op == "call" && item.TargetId == $"import:{OperatorId}")
            && instructions.All(item => item.Op != "binary"),
            "user-defined operator should lower to its Roslyn-bound callable");
    }

    private static SemanticControlFlowGraph ReturnGraph(SemanticOperation returnValue)
    {
        SemanticControlFlowEdge entry = new(0, 1, "fallthrough", "regular");
        SemanticControlFlowEdge returned = new(1, 2, "fallthrough", "return");
        return new SemanticControlFlowGraph(
            CallerId,
            0,
            2,
            new[]
            {
                new SemanticBasicBlock(0, "entry", true, "none", Array.Empty<SemanticOperation>(), null,
                    Array.Empty<SemanticControlFlowEdge>(), new[] { entry }),
                new SemanticBasicBlock(1, "block", true, "none", Array.Empty<SemanticOperation>(), returnValue,
                    new[] { entry }, new[] { returned }),
                new SemanticBasicBlock(2, "exit", true, "none", Array.Empty<SemanticOperation>(), null,
                    new[] { returned }, Array.Empty<SemanticControlFlowEdge>()),
            });
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
