using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestStaticAsyncValueTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException("Static async values: " + message); count++; }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_STATIC_ASYNC_VALUE_FIXTURE_DIR");
        var fixtures = new List<object>();
        foreach (bool deferred in new[] { false, true })
        foreach (var scenario in new[] {
            ("named-success", false, "try { int value = await Read(7); return Cache.Value + value; } catch (ArgumentException error) { return error == null ? 99 : 3; }"),
            ("named-before", false, "try { int value = Broken.Value; await Read(value); return 0; } catch (TypeInitializationException error) { return error == null ? 99 : 41; }"),
            ("named-after", false, "int value = await Read(7); try { return Broken.Value + value; } catch (TypeInitializationException error) { return error == null ? 99 : 42; }"),
            ("named-identity", false, "TypeInitializationException first = null; try { int value = Broken.Value; } catch (TypeInitializationException error) { first = error; Mark(5); } int result = await Read(7); try { return Broken.Value; } catch (TypeInitializationException error) { return first == error ? result : 99; }"),
            ("token-success", true, "int value = await Read(7); await AvidContinuations.NextTickAsync().WithCancellation(token); return Cache.Value + value;"),
            ("token-before", true, "await Read(7); source.Cancel(); try { await AvidContinuations.NextTickAsync().WithCancellation(token); return 99; } catch (OperationCanceledException) { return 3; }"),
            ("token-after", true, "await Read(7); await AvidContinuations.NextTickAsync().WithCancellation(token); source.Cancel(); try { await AvidContinuations.NextTickAsync().WithCancellation(token); return 99; } catch (OperationCanceledException) { return 3; }"),
            ("token-fault", true, "await Read(7); await AvidContinuations.NextTickAsync().WithCancellation(token); try { return Broken.Value; } catch (TypeInitializationException) { return 41; }") })
        {
            string name = scenario.Item1 + (deferred ? "-deferred" : "-ready");
            string source = Source(scenario.Item3, scenario.Item2, deferred);
            var reference = CSharpGuestCancellationTokenReference.ExecuteState(source, true);
            string sourceId = "Scripts/StaticAsyncValue_" + name + ".cs";
            var frontend = FrontendAnalyzer.Analyze(source, sourceId);
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestCancellationTokenTests.AsyncFacade, "generated://Continuations.cs", true) },
                new SemanticCompilerWorkspace(), enableStaticInitialization: true, enableAsyncExceptionFlow: true,
                enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true, enableAsyncSynchronousExceptions: true,
                enableAsyncCatchVariables: true, enableCancellationTokens: true, enableAsyncVoidErrorOwner: true);
            int sourceBase = scenario.Item2 ? 53 : 52;
            Check(semantic.SchemaVersion == 57 && semantic.SemanticVersion == "1.66"
                && semantic.CapabilityManifest!.BaseSchemaVersion == sourceBase
                && SemanticStaticInitializationValidator.IsValid(semantic), name + " actual source envelope");
            byte[] sourceBytes = SemanticSerializer.Serialize(semantic);
            Check(CSharpLanguageCapabilityCompiler.TryLower(semantic, Hash(sourceBytes), out var module, out var error)
                && module is not null, name + " compilation: " + error);
            Check(module!.SchemaVersion == 38 && module.IrVersion == "1.37"
                && module.StaticAsyncValueComposition!.SourceBaseSchemaVersion == sourceBase
                && (module.ExceptionValues is not null) != scenario.Item2
                && (module.CancellationTokens is not null) == scenario.Item2
                && GuestModuleValidator.Validate(module).Succeeded, name + " actual independent plans only");
            var entry = module.Functions.Single(function => function.Id.Contains("Script.BeginPlay", StringComparison.Ordinal));
            Check(entry.Blocks.SelectMany(block => block.Instructions).Count(instruction =>
                instruction.Op == "call" && instruction.TargetId == "import:$async:task_i32_v1") == 1,
                name + " discarded producer caller lease is released exactly once");
            byte[] ir = GuestIrSerializer.Serialize(module);
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded && ir.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(ir)))
                && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(ir)).Bytes), name + " canonical IR/WASM");
            Check(Encoding.UTF8.GetString(wasm.Bytes).Contains("source_execution=" + sourceBase + (scenario.Item2 ? "/1.62" : "/1.61"), StringComparison.Ordinal),
                name + " exact source execution marker");
            var debug = CSharpGuestDebugMapProjector.Project(semantic, module, Hash(ir), Hash(FrontendSerializer.Serialize(frontend)));
            Check(debug.Functions.Count > 0, name + " source debug sidecar");
            if (name == "named-success-ready" || name == "token-success-ready") Mutations(semantic, module, Check);
            if (string.IsNullOrWhiteSpace(directory)) continue;
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
            File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), sourceBytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), ir);
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            int Offset(string fieldName) {
                var field = semantic.Symbols.Single(symbol => symbol.Name == fieldName && symbol.ContainingSymbolId == "symbol:type:global::Script");
                string global = "global:symbol:field:$static:" + Hash(Encoding.UTF8.GetBytes("type:global::Script\n" + field.Id));
                return module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId == global).Offset;
            }
            int[] objects = scenario.Item1 switch {
                "named-success" => new[] { 1, deferred ? 0 : 1, deferred ? 0 : 1 },
                "named-before" or "named-identity" => new[] { 3, 3, 3 },
                "named-after" => new[] { 3, deferred ? 0 : 3, deferred ? 0 : 3 },
                "token-success" => new[] { 1, 0, deferred ? 0 : 1 },
                "token-fault" => new[] { 3, 0, deferred ? 0 : 3 },
                _ => new[] { 0, 0, 0 },
            };
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel = false, expected = reference.Result, trace = reference.Trace,
                resultOffset = Offset("Result"), traceOffset = Offset("Trace"), staticSlots = module.StaticStorage!.Slots.Count, liveStaticObjects = objects });
        }
        Check(fixtures.Count == 0 || fixtures.Count == 16, "complete ready/deferred source matrix");
        if (!string.IsNullOrWhiteSpace(directory)) File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures));
        return count;
    }
    private static string Source(string body, bool tokens, bool deferred) =>
        "using AvidScript; using System; using System.Threading; using System.Threading.Tasks; using System.Runtime.InteropServices; "
        + "public static class Cache { public static int Value; static Cache() { Script.Mark(1); Value = 7; } } "
        + "public static class Broken { public static int Value; static Broken() { Script.Mark(4); throw new InvalidOperationException(); } } "
        + "public static class Script { public static int Result; public static int Trace; public static int Mark(int value) { Trace = Trace * 10 + value; return value; } "
        + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static void BeginPlay() { " + (tokens ? "_ = Start();" : "Start();") + " } "
        + "public static async Task<int> Start() { Result = await Run(); return Result; } "
        + "public static async Task<int> Read(int value) { Mark(2); " + (deferred ? "await AvidContinuations.NextTickAsync(); Mark(3); " : "")
        + "return value; } public static async Task<int> Run() { "
        + (tokens ? "var source = AvidCancellationSource.Create(); CancellationToken token = source.Token; " : "")
        + "try { " + body + " } finally { Mark(8); " + (tokens ? "source.Release(); " : "") + "} } }";

    private static void Mutations(SemanticDocument source, GuestModule module, Action<bool, string> check)
    {
        foreach (var invalid in new[] { source with { SemanticVersion = "1.65" }, source with { CapabilityManifest = null },
            source with { StaticInitialization = null }, source with { StaticInitialization = source.StaticInitialization! with { BaseSchemaVersion = 50, BaseSemanticVersion = "1.59" } } })
            check(!CSharpLanguageCapabilityCompiler.TryLower(invalid, new string('a', 64), out _, out _), "reject incomplete source composition");
        foreach (var invalid in new[] { module with { IrVersion = "1.36" }, module with { SchemaVersion = 35, IrVersion = "1.34" },
            module with { StaticAsyncValueComposition = null }, module with { StaticStorage = null }, module with { CancellationIdentity = null },
            module with { StaticAsyncValueComposition = new(50, "1.59") }, module with { CapabilityManifest = module.CapabilityManifest! with { ExecutionBaseIrVersion = "1.29" } },
            module with { Provenance = module.Provenance with { SemanticSchemaVersion = 56, SemanticVersion = "1.65" } } })
            check(!GuestModuleValidator.Validate(invalid).Succeeded && !WasmModuleCompiler.Compile(invalid).Succeeded, "reject incomplete execution composition");
        if (module.DirectAwaitReadiness is not null)
        {
            var missingGuards = module with { DirectAwaitReadiness = null,
                CapabilityManifest = module.CapabilityManifest! with { Capabilities = module.CapabilityManifest.Capabilities
                    .Where(capability => capability.Id != GuestComposableCapabilities.AwaitReadiness).ToArray() } };
            check(!GuestModuleValidator.Validate(missingGuards).Succeeded && !WasmModuleCompiler.Compile(missingGuards).Succeeded,
                "cancel-bound awaits cannot erase their readiness contract");
        }
    }
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
