using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestDebugMapTests
{
    public static int Run()
    {
        RealSemanticFunctionsProjectToDeterministicWasmIndices();
        NonMethodGuestFunctionsRetainIndexSpaceWithoutFakeSourceLocations();
        GeneratedGameplayRouterRetainsIndexSpaceWithoutFakeSourceLocation();
        GeneratedContinuationRouterRetainsIndexSpaceWithoutFakeSourceLocation();
        DebugInstrumentationEmitsStableBackendNeutralProbeCalls();
        DebugFramesPublishBoundedLexicalVariables();
        ReversedSameLineSpanFailsClosed();
        return 7;
    }

    private static void DebugFramesPublishBoundedLexicalVariables()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_main")]
                public static void Main(int input)
                {
                    int outer = input + 1;
                    if (outer > 0)
                    {
                        int inner = outer + 2;
                        outer = inner;
                    }
                }
            }
            """;
        const string sourceId = "Scripts/DebugVariables.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        GuestModule module = CSharpGuestLowerer.Lower(
            semantic,
            new string('c', 64),
            enableDebugInstrumentation: true).Module
            ?? throw new InvalidOperationException("debug variable fixture should lower");
        string guestIrSha256 = Convert.ToHexString(
            SHA256.HashData(GuestIrSerializer.Serialize(module))).ToLowerInvariant();

        CSharpGuestDebugMap first = CSharpGuestDebugMapProjector.Project(
            semantic,
            module,
            guestIrSha256,
            new string('e', 64));
        CSharpGuestDebugMap second = CSharpGuestDebugMapProjector.Project(
            semantic,
            module,
            guestIrSha256,
            new string('e', 64));
        CSharpGuestDebugFrameLayout frame = first.Functions.Single(function =>
            function.GuestFunctionId.Contains(".Main(", StringComparison.Ordinal)).Frame
            ?? throw new InvalidOperationException("resumable source function should publish a frame");
        CSharpGuestDebugVariable[] variables = frame.Variables.ToArray();
        CSharpGuestDebugVariable input = variables.Single(variable => variable.Name == "input");
        CSharpGuestDebugVariable outer = variables.Single(variable => variable.Name == "outer");
        CSharpGuestDebugVariable inner = variables.Single(variable => variable.Name == "inner");

        Assert(frame.ByteSize > 0
            && frame.ByteSize <= 4096
            && variables.All(variable => variable.Offset >= 0
                && variable.ByteSize > 0
                && variable.Offset <= frame.ByteSize - variable.ByteSize),
            "debug variable slots should remain inside the bounded suspension frame");
        Assert(input.Kind == "parameter"
            && input.Storage == "i32"
            && input.Scope.Start <= outer.Declaration.Start
            && input.Scope.End >= inner.Scope.End,
            "parameters should retain method-wide scope and scalar storage metadata");
        Assert(outer.Kind == "local"
            && inner.Kind == "local"
            && inner.Scope.Start > outer.Scope.Start
            && inner.Scope.End < outer.Scope.End,
            "nested locals should retain their narrower lexical block scope");
        Assert(CSharpGuestDebugMapSerializer.Serialize(first)
                .SequenceEqual(CSharpGuestDebugMapSerializer.Serialize(second)),
            "equivalent debug frame metadata should serialize byte-identically");
    }

    private static void DebugInstrumentationEmitsStableBackendNeutralProbeCalls()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_main")]
                public static void Main(int value)
                {
                    int result = value + 1;
                }
            }
            """;
        const string sourceId = "Scripts/DebugProbe.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded, "debug probe source should analyze successfully");

        string semanticSha256 = new('c', 64);
        GuestModule disabled = CSharpGuestLowerer.Lower(semantic, semanticSha256).Module
            ?? throw new InvalidOperationException("disabled debug probe fixture should lower");
        CSharpGuestLoweringResult enabledResult = CSharpGuestLowerer.Lower(
            semantic,
            semanticSha256,
            enableDebugInstrumentation: true);
        GuestModule enabled = enabledResult.Module
            ?? throw new InvalidOperationException(
                "enabled debug probe fixture should lower: "
                + string.Join(" | ", enabledResult.Diagnostics.Select(item => item.Message)));

        Assert(!disabled.Imports.Any(item => item.Id == CSharpGuestDebugProbeAbi.ImportId)
            && disabled.Functions.SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .All(instruction => instruction.TargetId != CSharpGuestDebugProbeAbi.ImportId),
            "debug instrumentation should remain disabled by default");
        GuestImport probeImport = enabled.Imports.Single(item =>
            item.Id == CSharpGuestDebugProbeAbi.ImportId);
        Assert(probeImport.Module == CSharpGuestDebugProbeAbi.ModuleName
            && probeImport.Name == CSharpGuestDebugProbeAbi.ImportName
            && probeImport.ParameterTypeIds.SequenceEqual(new[] { "type:int64" })
            && probeImport.ReturnTypeId == "type:int32"
            && probeImport.DispatchClass == "debug",
            "enabled instrumentation should publish the versioned backend-neutral probe ABI");

        GuestInstruction[] instructions = enabled.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        GuestInstruction[] probeCalls = instructions.Where(instruction =>
            instruction.Op == "call"
            && instruction.TargetId == CSharpGuestDebugProbeAbi.ImportId).ToArray();
        Dictionary<string, GuestInstruction> constants = instructions
            .Where(instruction => instruction.Op == "constant" && instruction.ResultId is not null)
            .ToDictionary(instruction => instruction.ResultId!, StringComparer.Ordinal);
        string[] emittedProbeIds = probeCalls.Select(call =>
        {
            GuestInstruction constant = constants[call.OperandIds.Single()];
            long bits = long.Parse(constant.Constant!.Value!, System.Globalization.CultureInfo.InvariantCulture);
            return unchecked((ulong)bits).ToString("x16", System.Globalization.CultureInfo.InvariantCulture);
        }).ToArray();

        string guestIrSha256 = Convert.ToHexString(
            SHA256.HashData(GuestIrSerializer.Serialize(enabled))).ToLowerInvariant();
        CSharpGuestDebugMap debugMap = CSharpGuestDebugMapProjector.Project(
            semantic,
            enabled,
            guestIrSha256,
            new string('e', 64));
        string[] mappedProbeIds = debugMap.Functions
            .SelectMany(function => function.SequencePoints ?? Array.Empty<CSharpGuestDebugSequencePoint>())
            .Where(point => !point.Hidden)
            .Select(point => point.ProbeId!)
            .ToArray();
        Assert(probeCalls.Length > 0
            && probeCalls.All(call => call.DebugLocation is { Hidden: false })
            && emittedProbeIds.OrderBy(item => item, StringComparer.Ordinal)
                .SequenceEqual(mappedProbeIds.OrderBy(item => item, StringComparer.Ordinal)),
            "every source-visible sequence point should map to exactly one stable probe call");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(enabled);
        Assert(GuestModuleValidator.Validate(enabled).Succeeded && wasm.Succeeded,
            "instrumented Guest IR should remain valid and compile through the backend");
    }

    private static void GeneratedContinuationRouterRetainsIndexSpaceWithoutFakeSourceLocation()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                private static int CallbackId;
                private static long Token;
                private static float Status;

                [AvidContinuation(1)]
                public static void Resume() { }
            }
            """;
        const string facade = """
            using System;

            namespace AvidScript;

            [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
            public sealed class AvidContinuationAttribute : Attribute
            {
                public AvidContinuationAttribute(int callbackId) { }
            }
            """;
        const string sourceId = "Scripts/ContinuationDebugMap.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[] { new SemanticReferenceSource(facade, "generated://AvidScript.Continuation.cs", true) });
        Assert(semantic.Succeeded, "continuation debug-map source should analyze successfully");

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, new string('c', 64));
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(
                "continuation debug-map source should lower successfully: "
                + string.Join(" | ", lowering.Diagnostics.Select(diagnostic => diagnostic.Message)));
        const string routerId = "function:synthetic:continuation_v2";
        Assert(module.Functions.Any(function => function.Id == routerId),
            "continuation callbacks should synthesize the continuation router");

        string guestIrSha256 = Convert.ToHexString(
            SHA256.HashData(GuestIrSerializer.Serialize(module))).ToLowerInvariant();
        CSharpGuestDebugMap debugMap = CSharpGuestDebugMapProjector.Project(
            semantic,
            module,
            guestIrSha256,
            new string('e', 64));

        Assert(debugMap.DefinedFunctionCount == module.Functions.Count,
            "continuation routers should retain the complete WASM function index range");
        Assert(debugMap.Functions.All(function =>
                function.GuestFunctionId != routerId),
            "the generated continuation router must not publish a fake C# source location");
        Assert(debugMap.Functions.Count == module.Functions.Count - 1,
            "only the generated continuation router should be omitted from the source map");
    }

    private static void GeneratedGameplayRouterRetainsIndexSpaceWithoutFakeSourceLocation()
    {
        const string source = """
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

                public readonly struct InputEvent
                {
                    public readonly int ActionId;
                    public readonly int TriggerEvent;
                    public readonly FVector Value;
                }
            }

            namespace Game
            {
                public static class Script
                {
                    public static void OnInput(AvidScript.InputEvent input)
                    {
                    }
                }
            }
            """;
        const string sourceId = "Scripts/GameplayEvents.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded, "gameplay-event debug-map source should analyze successfully");

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, new string('c', 64));
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException("gameplay-event debug-map source should lower successfully");
        const string routerId = "function:synthetic:gameplay_event";
        Assert(module.Functions.Any(function => function.Id == routerId),
            "natural gameplay callbacks should synthesize the gameplay-event router");

        string guestIrSha256 = Convert.ToHexString(
            SHA256.HashData(GuestIrSerializer.Serialize(module))).ToLowerInvariant();
        CSharpGuestDebugMap debugMap = CSharpGuestDebugMapProjector.Project(
            semantic,
            module,
            guestIrSha256,
            new string('e', 64));

        Assert(debugMap.DefinedFunctionCount == module.Functions.Count,
            "source-less generated functions should retain the complete WASM function index range");
        Assert(debugMap.Functions.All(function => function.GuestFunctionId != routerId),
            "the generated gameplay router must not publish a fake C# source location");
        Assert(debugMap.Functions.Count == module.Functions.Count - 1,
            "only the generated gameplay router should be omitted from the source map");
        Assert(debugMap.Functions.Single().WasmFunctionIndex == module.Imports.Count,
            "omitting the generated router must not renumber source-backed functions");
    }

    private static void NonMethodGuestFunctionsRetainIndexSpaceWithoutFakeSourceLocations()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticCallable mainCallable = baseline.Callables.Single(callable =>
            callable.MethodSymbolId == CSharpGuestSemanticFixture.MainMethodId);
        const string constructorId = "symbol:method:global::Game.Script..ctor():void";
        const string ownerId = "symbol:type:global::Game.Script";
        SemanticSpan span = new(0, 1, 0, 0, 0, 1);
        SemanticDocument semantic = baseline with
        {
            Callables = baseline.Callables.Concat(new[]
            {
                mainCallable with { MethodSymbolId = constructorId },
            }).ToArray(),
            Symbols = baseline.Symbols.Concat(new[]
            {
                new SemanticSymbol(
                    ownerId,
                    "type",
                    "Script",
                    null,
                    "type:global::Game.Script",
                    "global::Game.Script",
                    true,
                    "public",
                    span),
                new SemanticSymbol(
                    CSharpGuestSemanticFixture.MainMethodId,
                    "method",
                    "Main",
                    ownerId,
                    "type:void",
                    "Main():void",
                    true,
                    "public",
                    span),
                new SemanticSymbol(
                    constructorId,
                    "constructor",
                    ".ctor",
                    ownerId,
                    "type:void",
                    ".ctor():void",
                    false,
                    "public",
                    span),
            }).ToArray(),
        };
        GuestModule lowered = CSharpGuestLowerer.Lower(baseline, new string('c', 64)).Module
            ?? throw new InvalidOperationException("non-method fixture should lower successfully");
        GuestFunction synthetic = lowered.Functions[0] with { Id = "function:" + constructorId };
        GuestModule module = lowered with
        {
            Functions = lowered.Functions.Concat(new[] { synthetic }).ToArray(),
        };

        CSharpGuestDebugMap debugMap = CSharpGuestDebugMapProjector.Project(
            semantic,
            module,
            new string('d', 64),
            new string('e', 64));

        Assert(debugMap.Functions.Count == lowered.Functions.Count,
            "non-method backend functions should not publish fake C# source locations");
        Assert(debugMap.ImportedFunctionCount == module.Imports.Count
            && debugMap.DefinedFunctionCount == module.Functions.Count,
            "debug map should retain the complete WASM function index range despite omitted constructors");
        Assert(debugMap.Functions.Select(function => function.WasmFunctionIndex)
            .SequenceEqual(Enumerable.Range(module.Imports.Count, lowered.Functions.Count)),
            "skipped non-method functions must not collapse the WASM function index space");
    }

    private static void ReversedSameLineSpanFailsClosed()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticSpan invalidSpan = new(0, 1, 2, 5, 2, 4);
        SemanticDocument semantic = baseline with
        {
            Symbols = baseline.Symbols.Concat(new[]
            {
                new SemanticSymbol(
                    "symbol:type:global::Game.Script",
                    "type",
                    "Script",
                    null,
                    "type:global::Game.Script",
                    "global::Game.Script",
                    true,
                    "public",
                    invalidSpan),
                new SemanticSymbol(
                    CSharpGuestSemanticFixture.MainMethodId,
                    "method",
                    "Main",
                    "symbol:type:global::Game.Script",
                    "type:void",
                    "Main():void",
                    true,
                    "public",
                    invalidSpan),
            }).ToArray(),
        };
        GuestModule module = CSharpGuestLowerer.Lower(semantic, new string('d', 64)).Module
            ?? throw new InvalidOperationException("malformed-span fixture should lower before debug projection");

        InvalidDataException exception = ExpectInvalidData(() =>
            CSharpGuestDebugMapProjector.Project(
                semantic,
                module,
                new string('e', 64),
                new string('f', 64)));

        Assert(exception.Message.StartsWith("ASDEBUG1003:", StringComparison.Ordinal),
            "reversed same-line source spans should fail with the source mapping category");
    }

    private static InvalidDataException ExpectInvalidData(Action action)
    {
        try
        {
            action();
        }
        catch (InvalidDataException exception)
        {
            return exception;
        }

        throw new InvalidOperationException("Expected InvalidDataException.");
    }

    private static void RealSemanticFunctionsProjectToDeterministicWasmIndices()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace Game;

            public static class Script
            {
                [DllImport("env", EntryPoint = "host_probe")]
                private static extern int HostProbe(int value);

                private static void Helper()
                {
                    HostProbe(41);
                }

                [UnmanagedCallersOnly(EntryPoint = "guest_main")]
                public static void Main()
                {
                    Helper();
                }
            }
            """;
        const string sourceId = "Scripts/DebugMap.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded, "debug-map source should analyze successfully");

        string semanticSha256 = new('c', 64);
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, semanticSha256);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException("debug-map source should lower successfully");
        string guestIrSha256 = Convert.ToHexString(
            SHA256.HashData(GuestIrSerializer.Serialize(module))).ToLowerInvariant();

        CSharpGuestDebugMap first = CSharpGuestDebugMapProjector.Project(
            semantic,
            module,
            guestIrSha256,
            new string('e', 64));
        CSharpGuestDebugMap second = CSharpGuestDebugMapProjector.Project(
            semantic,
            module,
            guestIrSha256,
            new string('e', 64));

        Assert(module.Imports.Count == 1 && module.Functions.Count == 2,
            "fixture should retain one reachable import and two defined functions");
        Assert(first.SchemaVersion == 2
            && first.DebugVersion == "2.0"
            && first.ModuleId == module.ModuleId
            && first.ImportedFunctionCount == module.Imports.Count
            && first.DefinedFunctionCount == module.Functions.Count,
            "debug map should publish its stable root contract");
        Assert(first.Source.Id == sourceId
            && first.Source.Sha256 == semantic.Source.Sha256,
            "debug map should retain project-relative source provenance");
        Assert(first.Provenance.FrontendArtifactSha256 == new string('e', 64)
            && first.Provenance.SemanticSha256 == semanticSha256
            && first.Provenance.GuestIrSha256 == guestIrSha256,
            "debug map should bind frontend artifact, semantic artifact, and Guest IR provenance");
        Assert(first.Functions.Select(function => function.WasmFunctionIndex)
            .SequenceEqual(new[] { 1, 2 }),
            "defined function indices should begin after the imported function index space");

        CSharpGuestDebugFunction main = first.Functions.Single(function =>
            function.DisplayName == "Game.Script.Main():void");
        SemanticSpan expectedSpan = semantic.Symbols.Single(symbol =>
            symbol.Id == main.MethodSymbolId).Span;
        Assert(main.GuestFunctionId == "function:" + main.MethodSymbolId
            && main.Span == expectedSpan,
            "method identity and zero-based Roslyn declaration span should be retained");
        Assert(main.SequencePoints is { Count: > 0 }
            && main.SequencePoints.All(point => point.WasmFunctionOffset == -1)
            && main.SequencePoints.Where(point => !point.Hidden).All(point =>
                point.ProbeId is { Length: 16 }
                && point.ProbeId.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f'))
            && main.SequencePoints.Select(point => point.ProbeId)
                .Where(probeId => probeId is not null)
                .Distinct(StringComparer.Ordinal)
                .Count() == main.SequencePoints.Count(point => !point.Hidden)
            && main.SequencePoints.Any(point => point.Kind == "call"),
            "v2 draft maps should retain stable semantic sequence points and probe ids before backend offset backfill");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.DebugOffsets.Count > 0,
            "WASM backend should publish authoritative instruction offsets");
        string wasmSha256 = Convert.ToHexString(SHA256.HashData(wasm.Bytes)).ToLowerInvariant();
        CSharpGuestDebugMap finalized = CSharpGuestDebugMapFinalizer.Finalize(
            first,
            new GuestWasmDebugOffsetMap(
                1,
                module.ModuleId,
                guestIrSha256,
                wasmSha256,
                module.Imports.Count,
                module.Functions.Count,
                wasm.DebugOffsets));
        CSharpGuestDebugFunction finalizedMain = finalized.Functions.Single(function =>
            function.MethodSymbolId == main.MethodSymbolId);
        Assert(finalized.Provenance.WasmSha256 == wasmSha256
            && finalizedMain.SequencePoints is { Count: > 0 }
            && finalizedMain.SequencePoints.All(point => point.WasmFunctionOffset >= 0)
            && finalizedMain.SequencePoints.Select(point => point.WasmFunctionOffset)
                .SequenceEqual(finalizedMain.SequencePoints
                    .Select(point => point.WasmFunctionOffset)
                    .OrderBy(offset => offset)),
            "backend finalization should bind the WASM hash and order resolved function-relative offsets");
        Assert(CSharpGuestDebugMapSerializer.Serialize(first)
                .SequenceEqual(CSharpGuestDebugMapSerializer.Serialize(second)),
            "equivalent semantic and Guest IR inputs should produce byte-identical debug maps and probe ids");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
