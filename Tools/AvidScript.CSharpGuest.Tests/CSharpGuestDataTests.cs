using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestDataTests
{
    private const string EnumTypeId = "type:global::Game.Mode";
    private const string ArrayTypeId = "type:int32[]";
    private const string EnumLocalId = "symbol:local:main:mode";
    private const string ArrayLocalId = "symbol:local:main:values";
    private const string LogMethodId = "symbol:method:global::Game.Host.Log(string):void";
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        StringEnumAndConstantArrayDataAreLowered();
        return 1;
    }

    private static void StringEnumAndConstantArrayDataAreLowered()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticCallable main = baseline.Callables.Single(item =>
            item.MethodSymbolId == CSharpGuestSemanticFixture.MainMethodId);
        SemanticCallable log = new(
            LogMethodId,
            "type:global::Game.Host",
            "type:void",
            new[]
            {
                new SemanticCallableParameter(0, "symbol:parameter:log:0", "message", "type:string", "none"),
            },
            true,
            false,
            false,
            null,
            new SemanticCallableImport("env", "host_log"),
            null);
        SemanticSymbol enumLocal = Local(EnumLocalId, "mode", EnumTypeId);
        SemanticSymbol arrayLocal = Local(ArrayLocalId, "values", ArrayTypeId);
        SemanticOperation stringLiteral = CSharpGuestSemanticFixture.Operation(
            "literal",
            "type:string",
            constant: new SemanticConstant("string", "hello guest"));
        SemanticOperation logCall = CSharpGuestSemanticFixture.Operation(
            "invocation",
            "type:void",
            LogMethodId,
            new[] { Argument(stringLiteral) });
        SemanticOperation enumLiteral = CSharpGuestSemanticFixture.Operation(
            "literal",
            EnumTypeId,
            constant: new SemanticConstant("global::Game.Mode", "1"));
        SemanticOperation enumAssignment = Assign(Reference(EnumLocalId, EnumTypeId), enumLiteral);
        SemanticOperation initializer = CSharpGuestSemanticFixture.Operation(
            "array_initializer",
            null,
            children: new[] { IntLiteral(3), IntLiteral(5), IntLiteral(8) });
        SemanticOperation arrayCreation = CSharpGuestSemanticFixture.Operation(
            "array_creation",
            ArrayTypeId,
            children: new[] { initializer });
        SemanticOperation arrayAssignment = Assign(Reference(ArrayLocalId, ArrayTypeId), arrayCreation);
        SemanticDocument document = baseline with
        {
            Symbols = baseline.Symbols.Concat(new[] { enumLocal, arrayLocal }).ToArray(),
            Callables = new[] { main, log },
            ControlFlowGraphs = new[] { ReturnGraph(new[] { logCall, enumAssignment, arrayAssignment }) },
        };

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
            "constant data module should lower and validate");
        Assert(module.DataSegments.Count(item => item.Kind == "utf8_string") == 1
            && module.DataSegments.Count(item => item.Kind == "constant_array") == 1,
            "string and array literals should become canonical data segments");
        Assert(instructions.Count(item => item.Op == "data_address") == 2,
            "reference constants should load placed data addresses");
        Assert(instructions.Any(item => item.Op == "constant"
            && item.ResultId is not null
            && item.Constant?.Kind == "int32"
            && item.Constant.Value == "1"),
            "enum literal should encode through its int32 underlying type");
    }

    private static SemanticSymbol Local(string id, string name, string typeId)
    {
        return new SemanticSymbol(
            id,
            "local",
            name,
            CSharpGuestSemanticFixture.MainMethodId,
            typeId,
            $"{typeId} {name}",
            false,
            "private",
            new SemanticSpan(0, 0, 0, 0, 0, 0));
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

    private static SemanticOperation Assign(SemanticOperation target, SemanticOperation value)
    {
        return CSharpGuestSemanticFixture.Operation(
            "assignment", value.TypeId, children: new[] { target, value });
    }

    private static SemanticOperation Reference(string symbolId, string typeId)
    {
        return CSharpGuestSemanticFixture.Operation("local_reference", typeId, symbolId);
    }

    private static SemanticOperation Argument(SemanticOperation value)
    {
        return CSharpGuestSemanticFixture.Operation("argument", null, children: new[] { value });
    }

    private static SemanticOperation IntLiteral(int value)
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
