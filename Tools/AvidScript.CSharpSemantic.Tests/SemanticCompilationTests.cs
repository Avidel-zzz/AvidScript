using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticCompilationTests
{
    public static int Run()
    {
        ActorLifecycleProducesStableSymbolsAndTypes();
        TypeErrorsProduceSemanticDiagnostics();
        FrontendHashMismatchFailsClosed();
        InvalidReferenceSourceFailsClosed();
        ExecutableReferenceSourceProjectsBodiesAndImports();
        SemanticSerializationIsDeterministic();
        SemanticDiagnosticsAreCultureInvariant();
        return 7;
    }

    private static void ActorLifecycleProducesStableSymbolsAndTypes()
    {
        const string sourceId = "Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs";
        string source = ReadActorLifecycleSource();
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(document.Succeeded, "ActorLifecycle semantic analysis should succeed");
        Assert(document.Source.Sha256 == frontend.Source.Sha256, "semantic source hash should match frontend");
        Assert(document.Source.FrontendSha256 == frontend.Source.Sha256, "semantic artifact should retain frontend source hash");
        Assert(document.Diagnostics.All(diagnostic => diagnostic.Severity != "error"), "ActorLifecycle should have no semantic errors");
        Assert(document.ControlFlowGraphs.Count == document.Methods.Count,
            "every ActorLifecycle executable method body should have a CFG");
        Assert(document.ControlFlowGraphs.Select(graph => graph.MethodSymbolId)
            .SequenceEqual(document.Methods.Select(method => method.MethodSymbolId)),
            "ActorLifecycle CFGs should align one-to-one with sorted executable method bodies");

        SemanticType floatType = FindType(document.Types, "type:float32");
        Assert(floatType.CanonicalName == "float32" && floatType.IsValueType, "float should use canonical float32 identity");
        SemanticType vectorType = FindType(document.Types, "type:global::AvidScript.FVector");
        Assert(vectorType.Kind == "struct" && vectorType.IsValueType, "FVector should be a canonical user struct");

        SemanticSymbol script = FindSymbol(document.Symbols, "symbol:type:global::AvidScript.ActorLifecycleScript");
        Assert(script.Kind == "type" && script.IsStatic, "ActorLifecycleScript should be a static type symbol");
        SemanticSymbol field = FindSymbol(document.Symbols, "symbol:field:global::AvidScript.ActorLifecycleScript.ElapsedSeconds:float32");
        Assert(field.TypeId == "type:float32" && field.Span.Length > 0, "ElapsedSeconds should retain type and declaration span");
        SemanticSymbol tick = FindSymbol(document.Symbols, "symbol:method:global::AvidScript.ActorLifecycleScript.Tick(float32):void");
        Assert(tick.Signature == "Tick(float32):void" && tick.Span.Length > 0, "Tick should retain a stable signature and declaration span");
    }

    private static void TypeErrorsProduceSemanticDiagnostics()
    {
        const string source = "class Script { void Tick() { int value = \"bad\"; } }";
        const string sourceId = "Scripts/TypeError.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(!document.Succeeded, "type errors should fail semantic analysis");
        SemanticDiagnostic diagnostic = document.Diagnostics.Single(item => item.Code == "CS0029");
        Assert(diagnostic.Severity == "error", "CS0029 should remain an error");
        Assert(diagnostic.Span.Start == source.IndexOf("\"bad\"", StringComparison.Ordinal), "type diagnostic should retain exact UTF-16 start");
        Assert(diagnostic.Span.Length == 5, "type diagnostic should retain exact UTF-16 length");
    }

    private static void FrontendHashMismatchFailsClosed()
    {
        const string source = "class Script { void Tick() { } }";

        SemanticDocument document = SemanticAnalyzer.Analyze(source, "Scripts/Stale.cs", new string('0', 64));

        Assert(!document.Succeeded, "frontend hash mismatch should fail semantic analysis");
        Assert(document.Diagnostics.Single().Code == "ASCS1001", "hash mismatch should use the stable ASCS1001 diagnostic");
        Assert(document.Symbols.Count == 0 && document.Types.Count == 0 &&
            document.TypeShapes.Count == 0 && document.Callables.Count == 0,
            "hash mismatch should not expose stale semantic tables");
    }

    private static void InvalidReferenceSourceFailsClosed()
    {
        const string source = "class Script { void Tick() { } }";
        const string sourceId = "Scripts/InvalidReference.cs";
        const string invalidReference = "namespace AvidScript; public static class BrokenFacade { public static int Value => MissingName; }";
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;

        SemanticDocument document = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            hash,
            new[] { new SemanticReferenceSource(invalidReference, "generated://BrokenFacade.cs") });

        Assert(!document.Succeeded, "compiler errors in generated reference sources should fail semantic analysis");
        SemanticDiagnostic diagnostic = document.Diagnostics.Single(item => item.Code == "CS0103");
        Assert(diagnostic.Severity == "error", "reference-source compiler diagnostics should remain errors");
        Assert(diagnostic.Span.Start == 0 && diagnostic.Span.Length == 0,
            "reference-source diagnostics should not project spans against the primary script");
        Assert(document.ControlFlowGraphs.Count == 0,
            "invalid reference compilations should not expose control-flow graphs");
    }

    private static void ExecutableReferenceSourceProjectsBodiesAndImports()
    {
        const string source = """
using System.Runtime.InteropServices;

namespace AvidScript;

public static class Script
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        Actor.SetScale(2.0f);
    }
}
""";
        const string generatedSource = """
using System.Runtime.InteropServices;

namespace AvidScript;

public static class Actor
{
    public static void SetScale(float value)
    {
        Native.SetScale(value);
    }
}

internal static class Native
{
    [DllImport("avidscript", EntryPoint = "avid_ue_test_scale")]
    internal static extern void SetScale(float value);
}
""";
        const string sourceId = "Scripts/ExecutableReference.cs";
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;

        SemanticDocument document = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            hash,
            new[]
            {
                new SemanticReferenceSource(
                    generatedSource,
                    "generated://AvidScript.Bindings.generated.cs",
                    IsExecutable: true),
            });

        Assert(document.Succeeded, "executable reference source should pass semantic analysis");
        SemanticCallable import = document.Callables.Single(callable =>
            callable.Import?.Name == "avid_ue_test_scale");
        Assert(import.Import!.Module == "avidscript", "generated DllImport should enter semantic imports");
        SemanticCallable wrapper = document.Callables.Single(callable =>
            callable.MethodSymbolId.EndsWith(".SetScale(float32):void", StringComparison.Ordinal)
            && callable.Import is null);
        Assert(wrapper.HasBody, "generated facade wrapper should retain an executable body");
        Assert(document.Methods.Any(method => method.MethodSymbolId == wrapper.MethodSymbolId),
            "generated facade body should enter semantic method projection");
        Assert(document.ControlFlowGraphs.Any(graph => graph.MethodSymbolId == wrapper.MethodSymbolId),
            "generated facade body should enter control-flow projection");
    }
    private static void SemanticSerializationIsDeterministic()
    {
        const string source = "namespace Game; class Script { float Speed; void Tick(float dt) { Speed = dt; } }";
        const string sourceId = "Scripts/Deterministic.cs";
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;

        byte[] first = SemanticSerializer.Serialize(SemanticAnalyzer.Analyze(source, sourceId, hash));
        byte[] second = SemanticSerializer.Serialize(SemanticAnalyzer.Analyze(source, sourceId, hash));

        Assert(first.SequenceEqual(second), "semantic JSON should be byte-for-byte deterministic");
        Assert(first.Length > 0 && first[^1] == (byte)'\n', "semantic JSON should end with LF");
    }

    private static void SemanticDiagnosticsAreCultureInvariant()
    {
        const string source = "class Script { void Tick() { int value = \"bad\"; } }";
        const string sourceId = "Scripts/CultureInvariant.cs";
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        CultureInfo originalCulture = CultureInfo.CurrentCulture;
        CultureInfo originalUiCulture = CultureInfo.CurrentUICulture;
        try
        {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("en-US");
            CultureInfo.CurrentUICulture = CultureInfo.GetCultureInfo("en-US");
            byte[] english = SemanticSerializer.Serialize(SemanticAnalyzer.Analyze(source, sourceId, hash));
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("zh-CN");
            CultureInfo.CurrentUICulture = CultureInfo.GetCultureInfo("zh-CN");
            byte[] chinese = SemanticSerializer.Serialize(SemanticAnalyzer.Analyze(source, sourceId, hash));
            Assert(english.SequenceEqual(chinese), "semantic diagnostics should be culture invariant");
        }
        finally
        {
            CultureInfo.CurrentCulture = originalCulture;
            CultureInfo.CurrentUICulture = originalUiCulture;
        }
    }
    private static string ReadActorLifecycleSource()
    {
        return File.ReadAllText(Path.Combine(
            Directory.GetCurrentDirectory(),
            "Samples",
            "CSharp",
            "ActorLifecycle",
            "ActorLifecycleScript.cs"));
    }

    private static SemanticType FindType(IEnumerable<SemanticType> types, string id)
    {
        return types.Single(type => type.Id == id);
    }

    private static SemanticSymbol FindSymbol(IEnumerable<SemanticSymbol> symbols, string id)
    {
        return symbols.Single(symbol => symbol.Id == id);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
