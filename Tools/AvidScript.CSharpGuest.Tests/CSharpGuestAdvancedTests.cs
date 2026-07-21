using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestAdvancedTests
{
    private const string PointTypeId = "type:global::Game.Point";
    private const string PointXFieldId = "symbol:field:global::Game.Point.X:int32";
    private const string PointYFieldId = "symbol:field:global::Game.Point.Y:int32";
    private const string PointLocalId = "symbol:local:main:point";
    private const string PointConstructorId = "symbol:method:global::Game.Point..ctor(int32,int32):void";
    private const string OutPointId = "symbol:method:global::Game.Host.GetPoint(out Point):int32";
    private const string ConvertMethodId = "symbol:method:global::Game.Script.Convert(int32):float32";
    private const string ExternalMethodId = "symbol:method:global::Game.External.Read():int32";
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        StructConstructionFieldsAndOutAddressesAreLowered();
        ConversionAndNonVoidReturnAreLowered();
        UnsupportedExternalCallFailsClosed();
        GameplaySourceControlFlowAndStateCompileToWasm();
        return 4;
    }

    private static void GameplaySourceControlFlowAndStateCompileToWasm()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript
            {
                public readonly struct AActor
                {
                    public readonly int Slot;
                    public readonly int Generation;
                }

                public readonly struct FVector
                {
                    public readonly float X;
                    public readonly float Y;
                    public readonly float Z;
                }

                public static class PickupScript
                {
                    private static bool Collected;
                    private static int LastCallbackId;
                    private static float Elapsed;

                    public static void OnBeginOverlap(AActor otherActor, FVector location)
                    {
                        if (!Collected && otherActor.Slot != 0)
                        {
                            Collected = true;
                            LastCallbackId = otherActor.Slot;
                        }
                        else if (Collected && location.Z < 0.0f)
                        {
                            Elapsed = 0.0f;
                        }
                    }

                    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                    public static void Tick(float deltaSeconds)
                    {
                        if (Collected)
                        {
                            Elapsed += deltaSeconds;
                        }
                    }

                    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
                    public static void OnTimer(int callbackId, int timerHandle)
                    {
                        if (!Collected || callbackId != 7)
                        {
                            return;
                        }

                        Collected = false;
                        LastCallbackId = callbackId;
                    }
                }
            }
            """;
        const string sourceId = "Scripts/PickupScript.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded,
            "gameplay control-flow source should pass semantic analysis: " + FormatSemanticDiagnostics(semantic));

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, new string('c', 64));
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(FormatDiagnostics(lowering));
        GuestValidationResult validation = GuestModuleValidator.Validate(module);
        Assert(lowering.Succeeded && validation.Succeeded,
            "gameplay control-flow source should produce valid Guest IR");
        Assert(module.Globals.Count == 3
            && module.Globals.Any(global => global.Id.Contains("Collected", StringComparison.Ordinal))
            && module.Globals.Any(global => global.Id.Contains("LastCallbackId", StringComparison.Ordinal))
            && module.Globals.Any(global => global.Id.Contains("Elapsed", StringComparison.Ordinal)),
            "gameplay state fields should become three stable Guest globals");

        GuestFunction overlap = module.Functions.Single(function =>
            function.Id.Contains(".OnBeginOverlap(", StringComparison.Ordinal));
        GuestInstruction[] overlapInstructions = overlap.Blocks
            .SelectMany(block => block.Instructions)
            .ToArray();
        Assert(overlap.Blocks.Count(block => block.Terminator.Kind == "branch_if") >= 3,
            "short-circuit overlap logic should retain conditional Guest branches");
        Assert(overlapInstructions.Any(instruction => instruction.Op == "global_store")
            && overlapInstructions.Any(instruction => instruction.Op == "field_load"),
            "overlap logic should write state and read actor/vector fields");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded
            && wasm.Bytes.Length > 8
            && wasm.Bytes[0] == 0x00
            && wasm.Bytes[1] == 0x61
            && wasm.Bytes[2] == 0x73
            && wasm.Bytes[3] == 0x6d,
            "gameplay control-flow Guest IR should compile to a WASM module");
        Assert(module.Exports.Count(export => export.Name == "avid_on_gameplay_event") == 1,
            "natural gameplay source should publish exactly one generated gameplay router");
    }

    private static void StructConstructionFieldsAndOutAddressesAreLowered()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticCallable main = baseline.Callables.Single(item =>
            item.MethodSymbolId == CSharpGuestSemanticFixture.MainMethodId);
        SemanticCallable constructor = new(
            PointConstructorId,
            PointTypeId,
            "type:void",
            new[]
            {
                new SemanticCallableParameter(0, Parameter(PointConstructorId, 0), "x", "type:int32", "none"),
                new SemanticCallableParameter(1, Parameter(PointConstructorId, 1), "y", "type:int32", "none"),
            },
            false,
            true,
            true,
            null,
            null,
            null);
        SemanticCallable outPoint = new(
            OutPointId,
            "type:global::Game.Host",
            "type:int32",
            new[]
            {
                new SemanticCallableParameter(0, Parameter(OutPointId, 0), "point", PointTypeId, "out"),
            },
            true,
            false,
            false,
            null,
            new SemanticCallableImport("env", "host_get_point"),
            null);
        SemanticSymbol pointLocal = new(
            PointLocalId,
            "local",
            "point",
            CSharpGuestSemanticFixture.MainMethodId,
            PointTypeId,
            "Point point",
            false,
            "private",
            Span());

        SemanticOperation construct = Operation(
            "object_creation",
            PointTypeId,
            PointConstructorId,
            new[] { Argument(Literal(4)), Argument(Literal(5)) });
        SemanticOperation assignPoint = Assign(Local(PointTypeId, PointLocalId), construct);
        SemanticOperation passOut = Operation(
            "invocation",
            "type:int32",
            OutPointId,
            new[]
            {
                Argument(Operation(
                    "declaration_expression",
                    PointTypeId,
                    children: new[] { Local(PointTypeId, PointLocalId) })),
            });
        SemanticOperation readX = Field(
            PointXFieldId,
            "type:int32",
            Local(PointTypeId, PointLocalId));
        SemanticOperation storeScore = Assign(
            Field(CSharpGuestSemanticFixture.StateFieldId, "type:int32"),
            readX);
        SemanticControlFlowGraph mainGraph = ReturnGraph(
            CSharpGuestSemanticFixture.MainMethodId,
            new[] { assignPoint, passOut, storeScore });

        SemanticOperation assignX = Assign(
            Field(PointXFieldId, "type:int32", Instance(PointTypeId)),
            ParameterReference(PointConstructorId, 0, "type:int32"));
        SemanticOperation assignY = Assign(
            Field(PointYFieldId, "type:int32", Instance(PointTypeId)),
            ParameterReference(PointConstructorId, 1, "type:int32"));
        SemanticControlFlowGraph constructorGraph = ReturnGraph(
            PointConstructorId,
            new[] { assignX, assignY });
        SemanticDocument document = baseline with
        {
            Symbols = baseline.Symbols.Append(pointLocal).ToArray(),
            Callables = new[] { main, constructor, outPoint },
            ControlFlowGraphs = new[] { mainGraph, constructorGraph },
        };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(FormatDiagnostics(result));
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "aggregate module should lower and validate");
        Assert(instructions.Any(item => item.Op == "stack_alloc")
            && instructions.Any(item => item.Op == "field_store")
            && instructions.Any(item => item.Op == "field_load"),
            "struct construction and field access should use explicit aggregate instructions");
        Assert(instructions.Any(item => item.Op == "address_of")
            && instructions.Any(item => item.Op == "call" && item.TargetId == $"import:{OutPointId}"),
            "out argument should pass an explicit local address to the import");
    }

    private static void ConversionAndNonVoidReturnAreLowered()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticType floatType = new("type:float32", "float32", "float", "primitive", true, false);
        string parameterId = Parameter(ConvertMethodId, 0);
        SemanticCallable convert = new(
            ConvertMethodId,
            "type:global::Game.Script",
            floatType.Id,
            new[] { new SemanticCallableParameter(0, parameterId, "value", "type:int32", "none") },
            true,
            false,
            true,
            null,
            null,
            null);
        SemanticOperation conversion = Operation(
            "conversion",
            floatType.Id,
            children: new[] { ParameterReference(ConvertMethodId, 0, "type:int32") },
            conversion: new SemanticConversion("numeric", true, false, true, true, false, false, false, null));
        SemanticControlFlowGraph convertGraph = ReturnGraph(
            ConvertMethodId,
            Array.Empty<SemanticOperation>(),
            conversion);
        SemanticDocument document = baseline with
        {
            Types = baseline.Types.Append(floatType).ToArray(),
            Callables = baseline.Callables.Append(convert).ToArray(),
            ControlFlowGraphs = baseline.ControlFlowGraphs.Append(convertGraph).ToArray(),
        };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(FormatDiagnostics(result));
        GuestFunction function = module.Functions.Single(item => item.Id == $"function:{ConvertMethodId}");
        GuestBasicBlock returnBlock = function.Blocks.Single(block => block.Terminator.Kind == "return");

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "numeric conversion module should lower and validate");
        Assert(returnBlock.Instructions.Any(item => item.Op == "convert")
            && returnBlock.Terminator.ReturnValueId is not null,
            "non-void return should use the converted value");
    }

    private static void UnsupportedExternalCallFailsClosed()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticCallable external = new(
            ExternalMethodId,
            "type:global::Game.External",
            "type:int32",
            Array.Empty<SemanticCallableParameter>(),
            true,
            false,
            false,
            null,
            null,
            null);
        SemanticOperation call = Operation("invocation", "type:int32", ExternalMethodId);
        SemanticDocument document = baseline with
        {
            Callables = baseline.Callables.Append(external).ToArray(),
            ControlFlowGraphs = new[]
            {
                ReturnGraph(CSharpGuestSemanticFixture.MainMethodId, new[] { call }),
            },
        };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);

        Assert(!result.Succeeded && result.Module is null,
            "unbound external call must not produce Guest IR");
        Assert(result.Diagnostics.Any(item => item.Code == "ASCG1005"),
            "unbound external call should report ASCG1005");
    }

    private static SemanticControlFlowGraph ReturnGraph(
        string methodId,
        SemanticOperation[] operations,
        SemanticOperation? returnValue = null)
    {
        SemanticControlFlowEdge entry = new(0, 1, "fallthrough", "regular");
        SemanticControlFlowEdge returned = new(1, 2, "fallthrough", "return");
        return new SemanticControlFlowGraph(
            methodId,
            0,
            2,
            new[]
            {
                new SemanticBasicBlock(0, "entry", true, "none", Array.Empty<SemanticOperation>(), null,
                    Array.Empty<SemanticControlFlowEdge>(), new[] { entry }),
                new SemanticBasicBlock(1, "block", true, "none", operations, returnValue,
                    new[] { entry }, new[] { returned }),
                new SemanticBasicBlock(2, "exit", true, "none", Array.Empty<SemanticOperation>(), null,
                    new[] { returned }, Array.Empty<SemanticControlFlowEdge>()),
            });
    }

    private static SemanticOperation Assign(SemanticOperation target, SemanticOperation value)
    {
        return Operation("assignment", value.TypeId, children: new[] { target, value });
    }

    private static SemanticOperation Field(string symbolId, string typeId, SemanticOperation? instance = null)
    {
        return Operation(
            "field_reference",
            typeId,
            symbolId,
            instance is null ? Array.Empty<SemanticOperation>() : new[] { instance });
    }

    private static SemanticOperation Local(string typeId, string symbolId)
    {
        return Operation("local_reference", typeId, symbolId);
    }

    private static SemanticOperation Instance(string typeId)
    {
        return Operation("instance_reference", typeId, $"symbol:{typeId}");
    }

    private static SemanticOperation ParameterReference(string methodId, int ordinal, string typeId)
    {
        return Operation("parameter_reference", typeId, Parameter(methodId, ordinal));
    }

    private static SemanticOperation Literal(int value)
    {
        return Operation(
            "literal",
            "type:int32",
            constant: new SemanticConstant("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    }

    private static SemanticOperation Argument(SemanticOperation value)
    {
        return Operation("argument", null, children: new[] { value });
    }

    private static SemanticOperation Operation(
        string kind,
        string? typeId,
        string? symbolId = null,
        IReadOnlyList<SemanticOperation>? children = null,
        SemanticConstant? constant = null,
        SemanticConversion? conversion = null)
    {
        return CSharpGuestSemanticFixture.Operation(
            kind,
            typeId,
            symbolId,
            children,
            constant,
            conversion: conversion);
    }

    private static string Parameter(string methodId, int ordinal) => $"symbol:parameter:{methodId}:{ordinal}";

    private static SemanticSpan Span() => new(0, 0, 0, 0, 0, 0);

    private static string FormatDiagnostics(CSharpGuestLoweringResult result)
    {
        return string.Join(" | ", result.Diagnostics.Select(item => $"{item.Code}:{item.Message}"));
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private static string FormatSemanticDiagnostics(SemanticDocument document)
    {
        return string.Join(" | ", document.Diagnostics.Select(diagnostic =>
            $"{diagnostic.Code}:{diagnostic.Message}"));
    }
}
