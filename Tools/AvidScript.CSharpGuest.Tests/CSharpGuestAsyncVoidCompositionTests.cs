using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestAsyncVoidCompositionTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message) {
            if (!condition) throw new InvalidOperationException("Async void composition: " + message);
            count++;
        }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_VOID_OWNER_FIXTURE_DIR");
        var fixtures = new List<object>();
        foreach (var profile in new[] { (Name: "static", Static: true, Tokens: false),
            (Name: "token", Static: false, Tokens: true), (Name: "combined", Static: true, Tokens: true) })
        foreach (var scenario in Cases(profile.Tokens, profile.Static))
        {
            string name = profile.Name + "-" + scenario.Name;
            string source = Source(scenario.Body, profile.Static, profile.Tokens);
            var expected = Reference(source);
            Check(expected == (scenario.Error, scenario.Trace), name + " identical-source .NET: " + expected);
            string id = "Scripts/AsyncVoidOwner_" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, id, FrontendAnalyzer.Analyze(source, id).Source.Sha256,
                new[] { new SemanticReferenceSource(profile.Tokens ? CSharpGuestCancellationTokenTests.AsyncFacade
                    : CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableAsyncSynchronousExceptions: true, enableStaticInitialization: profile.Static,
                enableCancellationTokens: profile.Tokens, enableAsyncVoidErrorOwner: true);
            Check(SemanticComposableCapabilities.IsAsyncVoidVersion(semantic)
                && SemanticComposableCapabilityValidator.IsValid(semantic)
                && SemanticAsyncVoidErrorOwnerValidator.IsValid(semantic), name + " source: "
                + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
            string hash = Hash(SemanticSerializer.Serialize(semantic));
            GuestModule? module;
            string? error;
            bool lowered = profile.Static ? CSharpStaticInitializationCompiler.TryLower(semantic, hash, out module, out error)
                : CSharpCancellationTokenCompiler.TryLower(semantic, hash, out module, out error);
            Check(lowered && module is not null, name + " lowering: " + error);
            Check(GuestAsyncVoidErrorOwners.IsCompositionVersion(module!) && GuestAsyncVoidErrorOwners.HasSourceContract(module!)
                && GuestModuleValidator.Validate(module!).Succeeded, name + " independent IR37 contract");
            Check(module!.StaticStorage is not null == profile.Static && module.CancellationTokens is not null == profile.Tokens,
                name + " only the actual static/token plans exist");
            Check(!CSharpGuestLowerer.Lower(semantic, hash, enableAsyncLanguageErrors: true).Succeeded,
                name + " ordinary public lowering remains closed after composition");
            byte[] ir = GuestIrSerializer.Serialize(module);
            var restored = GuestIrSerializer.Deserialize(ir);
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            Check(ir.SequenceEqual(GuestIrSerializer.Serialize(restored))
                && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(restored).Bytes), name + " canonical round trip");
            Check(System.Text.Encoding.UTF8.GetString(wasm.Bytes).Contains("source_execution=55/1.64", StringComparison.Ordinal),
                name + " WASM contains the composition source-base marker");
            if (scenario.Name == "success") Mutants(semantic, module, Check);
            if (string.IsNullOrWhiteSpace(directory)) continue;
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
            File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), ir);
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            string traceGlobal;
            if (profile.Static)
            {
                var trace = semantic.Symbols.Single(symbol => symbol.Name == "Trace" && symbol.ContainingSymbolId == "symbol:type:global::Script");
                string suffix = Hash(System.Text.Encoding.UTF8.GetBytes("type:global::Script\n" + trace.Id));
                traceGlobal = "global:symbol:field:$static:" + suffix;
            }
            else traceGlobal = module.Globals.Single(global => global.Id.Contains(".Trace:", StringComparison.Ordinal)).Id;
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel = false, errorType = expected.Error, trace = expected.Trace,
                traceOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId == traceGlobal).Offset,
                staticRoots = module.StaticStorage?.Slots.Count ?? 0,
                staticSuccessDeferred = profile.Static && scenario.Name == "success",
                staticCacheFault = scenario.CacheFault, cacheDeferred = scenario.CacheDeferred });
        }
        Check(fixtures.Count == 0 || fixtures.Count == 24, "three independent source profiles, eight cases each");
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;
    }

    private sealed record Scenario(string Name, string Body, string Error, int Trace, bool CacheFault = false, bool CacheDeferred = false);
    private static IEnumerable<Scenario> Cases(bool tokens, bool statics)
    {
        string wait = tokens ? "await AvidContinuations.NextTickAsync().WithCancellation(token);" : "await AvidContinuations.NextTickAsync();";
        string value = statics ? "Cache.Value" : "Sync(7)";
        yield return new("success", wait + " Trace = " + value + ";", "", 71);
        yield return new("sync-before", "Sync(-1); " + wait + " Trace = 2;", "ArgumentException", 1);
        yield return new("sync-after", wait + " Sync(-1);", "ArgumentException", 1);
        yield return new("caught", wait + " try { Sync(-1); } catch (ArgumentException error) { Trace = error == null ? 9 : 3; }", "", 31);
        yield return new("finally-replace", "try { " + wait + " Sync(-1); } finally { Trace = 4; throw new InvalidOperationException(); }", "InvalidOperationException", 41);
        if (tokens)
        {
            yield return new("cancel-before", "source.Cancel(); try { " + wait + " } catch (OperationCanceledException error) { Trace = error.CancellationToken == token ? 3 : 9; }", "", 31);
            yield return new("cancel-after", wait + " source.Cancel(); try { " + wait + " } catch (OperationCanceledException error) { Trace = error.CancellationToken == token ? 3 : 9; }", "", 31);
            yield return statics ? new("cache-after", wait + " Trace = Broken.Value;", "TypeInitializationException", 1, true, true)
                : new("uncaught-cancel", "source.Cancel(); " + wait, "TaskCanceledException", 1);
        }
        else
        {
            yield return new("cache-before", "Trace = Broken.Value; " + wait, "TypeInitializationException", 1, true);
            yield return new("cache-after", wait + " Trace = Broken.Value;", "TypeInitializationException", 1, true, true);
            yield return new("cache-retry", "try { Trace = Broken.Value; } catch (TypeInitializationException error) { Trace = 1; } "
                + wait + " try { Trace = Broken.Value; } catch (TypeInitializationException error) { Trace = Trace * 10 + 2; }", "", 121, true);
        }
    }

    private static string Source(string body, bool statics, bool tokens) =>
        "using AvidScript; using System; using System.Threading; using System.Threading.Tasks; using System.Runtime.InteropServices; "
        + "public static class Script { public static int Trace; "
        + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static void BeginPlay() { Run(); } "
        + "public static async void Run() { "
        + (tokens ? "var source = AvidCancellationSource.Create(); CancellationToken token = source.Token; " : "")
        + "try { " + body + " } finally { Trace = Trace * 10 + 1; " + (tokens ? "source.Release(); " : "") + "} } "
        + "public static int Sync(int value) { if (value < 0) throw new ArgumentException(); return value; } } "
        + (statics ? "public static class Cache { public static int Value; static Cache() { Value = Script.Sync(7); } } "
            + "public static class Broken { public static int Value; static Broken() { Value = Script.Sync(-1); } } " : "");

    private static void Mutants(SemanticDocument source, GuestModule valid, Action<bool, string> check)
    {
        void RejectSource(SemanticDocument candidate, string name) => check(!SemanticAsyncVoidErrorOwnerValidator.IsValid(candidate)
            || !SemanticComposableCapabilityValidator.IsValid(candidate), "reject source " + name);
        RejectSource(source with { SchemaVersion = 55, SemanticVersion = "1.64" }, "downgrade retaining composition manifest");
        RejectSource(source with { SemanticVersion = "1.64" }, "half version pair");
        RejectSource(source with { CapabilityManifest = source.CapabilityManifest! with { BaseSchemaVersion = 50, BaseSemanticVersion = "1.59" } }, "wrong source base");
        RejectSource(source with { CapabilityManifest = source.CapabilityManifest! with { Capabilities = source.CapabilityManifest.Capabilities
            .Where(capability => capability.Id != SemanticComposableCapabilities.AsyncVoidOwner).ToArray() } }, "missing owner capability");
        RejectSource(source with { CapabilityManifest = source.CapabilityManifest! with { Capabilities = source.CapabilityManifest.Capabilities
            .Append(source.CapabilityManifest.Capabilities[0]).ToArray() } }, "duplicate capability");
        void Reject(GuestModule candidate, string name) => check(!GuestModuleValidator.Validate(candidate).Succeeded,
            "reject IR " + name);
        Reject(valid with { SchemaVersion = 36, IrVersion = "1.35", Provenance = valid.Provenance with { SemanticSchemaVersion = 55, SemanticVersion = "1.64" } }, "downgrade retaining marker");
        Reject(valid with { IrVersion = "1.35" }, "half version pair");
        Reject(valid with { AsyncVoidComposition = null }, "missing composition marker");
        Reject(valid with { AsyncVoidComposition = valid.AsyncVoidComposition! with { ContractVersion = 2 } }, "unknown composition contract");
        Reject(valid with { AsyncVoidComposition = valid.AsyncVoidComposition! with { SourceBaseSchemaVersion = 50 } }, "wrong composition source base");
        Reject(valid with { CapabilityManifest = valid.CapabilityManifest! with { Capabilities = valid.CapabilityManifest.Capabilities
            .Where(capability => capability.Id != GuestAsyncVoidErrorOwners.CapabilityId).ToArray() } }, "missing owner capability");
        Reject(valid with { CapabilityManifest = valid.CapabilityManifest! with { ExecutionBaseSchemaVersion = 30, ExecutionBaseIrVersion = "1.29" } }, "wrong execution base");
        if (valid.StaticStorage is { } storage) Reject(valid with { StaticStorage = storage with { BaseSchemaVersion = 30, BaseIrVersion = "1.29" } }, "static plan base mismatch");
        if (valid.CancellationTokens is { } tokens) Reject(valid with { CancellationTokens = tokens with { BaseSchemaVersion = 30, BaseIrVersion = "1.29" } }, "token plan base mismatch");
    }

    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    private static (string Error, int Trace) Reference(string source)
    {
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("AsyncVoidCompositionReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(CSharpGuestCancellationTokenReference.Facade) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("async-void-composition-reference", isCollectible: true);
        var previous = SynchronizationContext.Current;
        var pump = new ErrorPump();
        try {
            SynchronizationContext.SetSynchronizationContext(pump);
            var assembly = context.LoadFromStream(bytes);
            var script = assembly.GetType("Script")!;
            script.GetMethod("Run")!.Invoke(null, null);
            var step = assembly.GetType("AvidScript.AvidContinuations")!.GetMethod("Step")!;
            for (int tick = 0; ; tick++) {
                if (tick >= 256) throw new InvalidOperationException("Composition reference exceeded scheduler budget.");
                if (pump.Drain() || (bool)step.Invoke(null, null)!) continue;
                if (pump.Active != 0) throw new InvalidOperationException("Composition reference stalled.");
                break;
            }
            if (pump.Errors.Count > 1) throw new InvalidOperationException("Composition reference reported twice.");
            return (pump.Errors.SingleOrDefault()?.GetType().Name ?? "", (int)script.GetField("Trace")!.GetValue(null)!);
        } finally { SynchronizationContext.SetSynchronizationContext(previous); context.Unload(); }
    }

    private sealed class ErrorPump : SynchronizationContext
    {
        private readonly Queue<(SendOrPostCallback Callback, object? State)> posts = new();
        public readonly List<Exception> Errors = new();
        public int Active { get; private set; }
        public override void OperationStarted() => Active++;
        public override void OperationCompleted() => Active--;
        public override void Post(SendOrPostCallback callback, object? state) => posts.Enqueue((callback, state));
        public bool Drain() {
            if (!posts.TryDequeue(out var post)) return false;
            try { post.Callback(post.State); } catch (Exception error) { Errors.Add(error); }
            return true;
        }
    }
}
