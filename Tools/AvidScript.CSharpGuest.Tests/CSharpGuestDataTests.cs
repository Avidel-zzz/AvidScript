using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

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
        FNameFacadeSourceLowersToUtf8DataAndWasm();
        return 2;
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

    private static void FNameFacadeSourceLowersToUtf8DataAndWasm()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void BeginPlay()
                {
                    UE.Self.ActorHasTag("Player");
                }
            }
            """;
        string generatedFacadeSource = File.ReadAllText(Path.Combine(
            Directory.GetCurrentDirectory(),
            "Tests",
            "Fixtures",
            "BindingGeneration",
            "P48_4_FNameActorHasTag.generated.cs"));
        const string sourceId = "Scripts/FNameActorHasTag.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[] { new SemanticReferenceSource(generatedFacadeSource, "generated://AvidScript.Bindings.generated.cs", true) });
        Assert(frontend.Succeeded && semantic.Succeeded,
            "FName facade source should pass frontend and semantic analysis: "
            + string.Join(" | ", semantic.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(string.Join(" | ", lowering.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestValidationResult validation = GuestModuleValidator.Validate(module);
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        GuestDataSegment segment = module.DataSegments.Single(item => item.Kind == "utf8_string" && item.ElementCount == 6);
        byte[] expectedBytes = { 6, 0, 0, 0, (byte)'P', (byte)'l', (byte)'a', (byte)'y', (byte)'e', (byte)'r', 0 };
        GuestImport import = module.Imports.Single(item => item.Module == "avidscript");
        GuestInstruction[] instructions = module.Functions.SelectMany(item => item.Blocks).SelectMany(item => item.Instructions).ToArray();

        Assert(lowering.Succeeded && validation.Succeeded,
            "FName facade source should produce valid Guest IR");
        Assert(segment.Bytes.SequenceEqual(expectedBytes)
            && instructions.Any(item => item.Op == "data_address" && item.TargetId == segment.Id),
            "FName string literal should use its placed UTF-8 string data address");
        Assert(import.ParameterTypeIds.SequenceEqual(new[] { "type:int32", "type:int32", "type:string", "type:address" }),
            "FName native import should retain one string address parameter between self and return storage");
        Assert(import.Name.StartsWith("avid_ue_", StringComparison.Ordinal)
            && generatedFacadeSource.Contains($"EntryPoint = \"{import.Name}\"", StringComparison.Ordinal),
            "FName WASM import should retain the exact entry point emitted by the renderer fixture");
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8
            && wasm.Bytes[0] == 0x00 && wasm.Bytes[1] == 0x61 && wasm.Bytes[2] == 0x73 && wasm.Bytes[3] == 0x6d,
            "FName facade Guest IR should compile to WASM");
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
