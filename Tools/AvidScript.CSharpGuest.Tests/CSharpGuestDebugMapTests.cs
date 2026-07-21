using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestDebugMapTests
{
    public static int Run()
    {
        RealSemanticFunctionsProjectToDeterministicWasmIndices();
        NonMethodGuestFunctionsRetainIndexSpaceWithoutFakeSourceLocations();
        GeneratedGameplayRouterRetainsIndexSpaceWithoutFakeSourceLocation();
        ReversedSameLineSpanFailsClosed();
        return 4;
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
        Assert(first.SchemaVersion == 1
            && first.DebugVersion == "1.0"
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
        Assert(CSharpGuestDebugMapSerializer.Serialize(first)
                .SequenceEqual(CSharpGuestDebugMapSerializer.Serialize(second)),
            "equivalent semantic and Guest IR inputs should produce byte-identical debug maps");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
