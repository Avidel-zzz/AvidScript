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

internal static class CSharpGuestCapabilityCliTests
{
    private sealed record Profile(bool Static = false, bool Tokens = false, bool Void = false, bool Catch = false);
    public static int RunGameplayCoverage()
    {
        int count = 0;
        void Check(bool result, string message) { if (!result) throw new InvalidOperationException("Gameplay coverage: " + message); count++; }
        foreach (bool statics in new[] { false, true })
        foreach (bool tokens in new[] { false, true })
        foreach (bool named in new[] { false, true })
            Case(TaskSource(named, tokens, statics), $"gameplay-{statics}-{tokens}-{named}",
                new(Static: statics, Tokens: tokens, Catch: named),
                tokens ? statics ? named ? 54 : 57 : 53 : statics ? named ? 57 : 51 : named ? 52 : 50,
                Check, fullOptions: true);
        return count;
    }
    public static int Run()
    {
        int count = 0;
        void Check(bool result, string message) { if (!result) throw new InvalidOperationException("Capability CLI: " + message); count++; }
        string? fixtureRoot = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_VOID_OWNER_FIXTURE_DIR");
        var fixtures = new List<object>();
        foreach (var profile in new[] { new Profile(true, false, true), new Profile(false, true, true), new Profile(true, true, true) })
        foreach (var scenario in CSharpGuestAsyncVoidCompositionTests.Cases(profile.Tokens, profile.Static))
        {
            string name = (profile.Static ? profile.Tokens ? "combined" : "static" : "token") + "-" + scenario.Name;
            string source = CSharpGuestAsyncVoidCompositionTests.Source(scenario.Body, profile.Static, profile.Tokens);
            Check(CSharpGuestAsyncVoidCompositionTests.Reference(source) == (scenario.Error, scenario.Trace), name + " same-source .NET result and trace");
            Case(source, name, profile, 56, Check, fixtureRoot, fixtures, scenario);
        }
        Case(CSharpGuestAsyncVoidCompositionTests.Source("await AvidContinuations.NextTickAsync(); Sync(-1);", false, false),
            "owner", new(Void: true), 55, Check);
        foreach (bool named in new[] { false, true })
            Case(TaskSource(named, false, false), named ? "catch-values" : "synchronous-async",
                new(Catch: named), named ? 52 : 50, Check);
        Case(TaskSource(true, true, false), "token", new(Tokens: true, Catch: true), 53, Check);
        Case(TaskSource(true, true, true), "static-token", new(true, true, false, true), 54, Check);
        Case("using System.Runtime.InteropServices; public static class Script { public static int Value = 7; public static int Read() { return Value; } "
            + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static void BeginPlay() { Value = Read(); } }",
            "static", new(Static: true), 49, Check);
        Check(fixtures.Count == 0 || fixtures.Count == 24, "complete public CLI fixture matrix");
        if (!string.IsNullOrWhiteSpace(fixtureRoot))
            File.WriteAllText(Path.Combine(fixtureRoot, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;
    }

    private static string TaskSource(bool named, bool token, bool statics) =>
        "using AvidScript; using System; using System.Threading; using System.Threading.Tasks; using System.Runtime.InteropServices; public static class Script { "
        + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static void BeginPlay() { Run(); } "
        + "public static int Sync(int value) { if (value < 0) throw new ArgumentException(); return value; } "
        + "public static async Task<int> Run() { "
        + (token ? "var source = AvidCancellationSource.Create(); CancellationToken token = source.Token; " : "")
        + "try { await AvidContinuations.NextTickAsync()" + (token ? ".WithCancellation(token)" : "")
        + "; return " + (statics ? "Cache.Value" : "Sync(-1)") + "; } catch (ArgumentException"
        + (named ? " error" : "") + ") { return " + (named ? "error == null ? 9 : 3" : "3") + "; } "
        + (token ? "finally { source.Release(); } " : "") + "} } "
        + (statics ? "public static class Cache { public static int Value; static Cache() { Value = Script.Sync(7); } }" : "");

    private static void Case(string source, string name, Profile profile, int schema, Action<bool, string> check,
        string? fixtureRoot = null, List<object>? fixtures = null, CSharpGuestAsyncVoidCompositionTests.Scenario? scenario = null,
        bool fullOptions = false)
    {
        string directory = Directory.CreateTempSubdirectory("AvidScript.CapabilityCli.").FullName;
        string[] files = { "source.cs", "facade.cs", "frontend.json", "semantic.json", "guest.json", "state.json", "debug.json" };
        try
        {
            string PathOf(string file) => Path.Combine(directory, file);
            string sourceId = "Scripts/AsyncVoidOwner_" + name + ".cs";
            var frontend = FrontendAnalyzer.Analyze(source, sourceId);
            File.WriteAllText(PathOf("source.cs"), source);
            File.WriteAllText(PathOf("facade.cs"), CSharpGuestCancellationTokenTests.AsyncFacade);
            byte[] frontendBytes = FrontendSerializer.Serialize(frontend);
            File.WriteAllBytes(PathOf("frontend.json"), frontendBytes);
            var semanticArgs = new List<string> { "--source", PathOf("source.cs"), "--source-id", sourceId,
                "--frontend", PathOf("frontend.json"), "--output", PathOf("semantic.json"), "--executable-reference-source", PathOf("facade.cs") };
            var analysis = fullOptions ? new Profile(true, true, true, true) : profile;
            foreach (string option in Options(analysis)) semanticArgs.AddRange(new[] { option, "enabled" });
            var workspace = new SemanticCompilerWorkspace();
            int semanticExit = SemanticCommandLine.Run(semanticArgs.ToArray(), workspace);
            // Supported exception-flow artifacts can carry source diagnostics;
            // the dedicated compiler must validate them before publication.
            check(semanticExit is 0 or 1 && File.Exists(PathOf("semantic.json")), name + " Semantic CLI writes the source artifact");
            byte[] semanticBytes = File.ReadAllBytes(PathOf("semantic.json"));
            var semantic = SemanticSerializer.Deserialize(semanticBytes);
            var api = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestCancellationTokenTests.AsyncFacade, "reference:0:facade.cs", true) },
                workspace, enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableAsyncSynchronousExceptions: true, enableStaticInitialization: analysis.Static,
                enableAsyncCatchVariables: analysis.Catch, enableCancellationTokens: analysis.Tokens, enableAsyncVoidErrorOwner: analysis.Void);
            check(semantic.SchemaVersion == schema && semanticBytes.SequenceEqual(SemanticSerializer.Serialize(api)), name + " exact API/CLI source bytes");
            check(CSharpLanguageCapabilityCompiler.TryLower(api, Hash(semanticBytes), out var apiModule, out var error)
                && apiModule is not null, name + " API compiler: " + error);
            string[] guestArgs = { "--semantic", PathOf("semantic.json"), "--output", PathOf("guest.json"),
                "--state-schema", PathOf("state.json"), "--debug-map", PathOf("debug.json"),
                "--frontend-artifact-sha256", Hash(frontendBytes), "--language-errors", "bounded" };
            check(GuestCommandLine.Run(guestArgs) == 0, name + " public Guest CLI compiles source capabilities");
            byte[] guestBytes = File.ReadAllBytes(PathOf("guest.json"));
            var guest = GuestIrSerializer.Deserialize(guestBytes);
            check(guestBytes.SequenceEqual(GuestIrSerializer.Serialize(apiModule!)) && GuestModuleValidator.Validate(guest).Succeeded,
                name + " API/CLI execution bytes and independent validation");
            check((guest.StaticStorage is not null) == profile.Static && (guest.CancellationTokens is not null) == profile.Tokens
                && (guest.Types.Any(type => type.Id == SemanticCancellationTokens.TypeId)) == profile.Tokens,
                name + " only source-used capabilities and token layouts exist");
            check(File.Exists(PathOf("state.json")) && File.Exists(PathOf("debug.json")), name + " state/debug sidecars published");
            byte[] debugBytes = File.ReadAllBytes(PathOf("debug.json"));
            var debug = CSharpGuestDebugMapSerializer.Deserialize(debugBytes);
            if (name == "static")
                check(debug.Functions.Any(function => function.DisplayName == "Script.Value initializer"
                    && function.Span == semantic.StaticInitialization!.Types.Single().Fields.Single().Span),
                    "field initializer preserves its real source span");
            if (name == "static-token")
                check(debug.Functions.Any(function => function.DisplayName == "Cache static initialization"
                    && function.Span == semantic.Symbols.Single(symbol => symbol.Name == ".cctor").Span),
                    "explicit static constructor preserves its real source span");
            var wasm = WasmModuleCompiler.Compile(guest);
            check(wasm.Succeeded && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(apiModule!).Bytes), name + " canonical API/CLI WASM");
            check(GuestCommandLine.Run(guestArgs) == 0 && guestBytes.SequenceEqual(File.ReadAllBytes(PathOf("guest.json"))), name + " repeated CLI output stable");
            check(debugBytes.SequenceEqual(File.ReadAllBytes(PathOf("debug.json"))), name + " repeated debug mapping stable");
            if (scenario is not null && !string.IsNullOrWhiteSpace(fixtureRoot))
            {
                Directory.CreateDirectory(fixtureRoot);
                File.WriteAllText(Path.Combine(fixtureRoot, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(fixtureRoot, name + ".semantic.json"), semanticBytes);
                File.WriteAllBytes(Path.Combine(fixtureRoot, name + ".guest-ir.json"), guestBytes);
                File.WriteAllBytes(Path.Combine(fixtureRoot, name + ".wasm"), wasm.Bytes);
                string traceGlobal;
                if (profile.Static)
                {
                    var trace = semantic.Symbols.Single(symbol => symbol.Name == "Trace" && symbol.ContainingSymbolId == "symbol:type:global::Script");
                    traceGlobal = "global:symbol:field:$static:" + Hash(Encoding.UTF8.GetBytes("type:global::Script\n" + trace.Id));
                }
                else traceGlobal = guest.Globals.Single(global => global.Id.Contains(".Trace:", StringComparison.Ordinal)).Id;
                fixtures!.Add(new { name, moduleId = guest.ModuleId, cancel = false, errorType = scenario.Error, trace = scenario.Trace,
                    traceOffset = guest.MemoryLayout.StateSlots.Single(slot => slot.GlobalId == traceGlobal).Offset,
                    staticRoots = guest.StaticStorage?.Slots.Count ?? 0,
                    staticSuccessDeferred = profile.Static && scenario.Name == "success",
                    staticCacheFault = scenario.CacheFault, cacheDeferred = scenario.CacheDeferred });
            }
            if (name == "static")
            {
                foreach (var invalid in new[] { semantic with { SemanticVersion = "1.59" }, semantic with { StaticInitialization = null } })
                {
                    File.WriteAllBytes(PathOf("semantic.json"), SemanticSerializer.Serialize(invalid));
                    check(GuestCommandLine.Run(guestArgs) != 0 && !File.Exists(PathOf("guest.json"))
                        && !File.Exists(PathOf("state.json")) && !File.Exists(PathOf("debug.json")),
                        "static source requires a complete paired contract before publishing sidecars");
                }
                foreach (string forgedId in new[] { "function:$static:body:forged", "function:$static:guard:forged", "function:$token:exception_receiver:forged" })
                {
                    bool rejected = false;
                    try
                    {
                        var forged = guest with { Functions = guest.Functions.Append(guest.Functions[0] with { Id = forgedId }).ToArray() };
                        CSharpGuestDebugMapProjector.Project(semantic, forged, Hash(GuestIrSerializer.Serialize(forged)), Hash(frontendBytes));
                    }
                    catch (InvalidDataException exception) { rejected = exception.Message.StartsWith("ASDEBUG1003", StringComparison.Ordinal); }
                    check(rejected, "debug mapping does not trust an arbitrary generated helper prefix: " + forgedId);
                }
            }
            if (name != "owner") return;
            foreach (string[] invalid in new[] {
                guestArgs.Select(value => value == "bounded" ? "disabled" : value).ToArray(),
                guestArgs.Concat(new[] { "--debug-instrumentation", "enabled" }).ToArray(),
                guestArgs.Concat(new[] { "--data-lane-fusion", "disabled" }).ToArray() })
            {
                foreach (string file in new[] { "guest.json", "state.json", "debug.json" }) File.WriteAllText(PathOf(file), "stale");
                check(GuestCommandLine.Run(invalid) != 0 && !File.Exists(PathOf("guest.json"))
                    && !File.Exists(PathOf("state.json")) && !File.Exists(PathOf("debug.json")), "incompatible options remove all stale publications");
            }
            foreach (string option in new[] { "--async-synchronous-exceptions", "--static-initialization", "--async-catch-variables", "--cancellation-tokens", "--async-void-error-owner" })
            {
                File.Delete(PathOf("semantic.json"));
                var basic = semanticArgs.Take(12).ToArray();
                check(SemanticCommandLine.Run(basic.Concat(new[] { option, "unknown" }).ToArray()) == 2
                    && !File.Exists(PathOf("semantic.json")), option + " rejects unknown mode");
                check(SemanticCommandLine.Run(basic.Concat(new[] { option, "enabled", option, "enabled" }).ToArray()) == 2,
                    option + " rejects duplicate option");
            }
            foreach (string option in new[] { "--async-synchronous-exceptions", "--async-catch-variables", "--cancellation-tokens", "--async-void-error-owner" })
                check(SemanticCommandLine.Run(semanticArgs.Take(12).Concat(new[] { option, "enabled" }).ToArray()) == 2,
                    option + " rejects missing dependencies");
            File.WriteAllBytes(PathOf("semantic.json"), SemanticSerializer.Serialize(semantic with { SemanticVersion = "1.40" }));
            check(GuestCommandLine.Run(guestArgs) != 0 && !File.Exists(PathOf("guest.json")), "half version pair cannot enter the selector");
            File.WriteAllBytes(PathOf("semantic.json"), SemanticSerializer.Serialize(semantic with { AsyncMethods = semantic.AsyncMethods.Select(method => method with { VoidErrorOwner = null }).ToArray() }));
            check(GuestCommandLine.Run(guestArgs) != 0 && !File.Exists(PathOf("guest.json")), "missing owner cannot enter the selector");
        }
        finally
        {
            // Only this fresh test workspace's named files are owned here.
            foreach (string file in files) File.Delete(Path.Combine(directory, file));
            if (!Directory.EnumerateFileSystemEntries(directory).Any()) Directory.Delete(directory);
        }
    }

    private static IEnumerable<string> Options(Profile profile)
    {
        yield return "--async-exception-flow"; yield return "--direct-await-cleanup"; yield return "--async-cancellation-flow";
        yield return "--async-synchronous-exceptions";
        if (profile.Static) yield return "--static-initialization";
        if (profile.Catch) yield return "--async-catch-variables";
        if (profile.Tokens) yield return "--cancellation-tokens";
        if (profile.Void) yield return "--async-void-error-owner";
    }
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
