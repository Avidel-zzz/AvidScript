using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using AvidScript.CSharpCompilerWorker;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestLanguageProfileTests
{
    private static readonly string Fingerprint = new('a', 64);
    private const string SourceId = "Scripts/GameplayProfile.cs";
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException("Gameplay profile: " + message); count++; }
        var profile = CSharpLanguageProfile.Gameplay;
        CheckUnusedTokenFacade(Check);
        Check(profile.Identity.ContractSha256 == Hash(Encoding.UTF8.GetBytes(profile.CanonicalJson)), "identity covers the canonical policy");
        using (var description = JsonDocument.Parse(profile.DescribeJson()))
            Check(description.RootElement.GetProperty("definition").GetProperty("analysis").EnumerateObject().Count() == 8
                && description.RootElement.GetProperty("contract_sha256").GetString() == profile.Identity.ContractSha256,
                "description exposes the owner definition and exact hash");
        TextWriter previousOutput = Console.Out;
        using (var output = new StringWriter())
        {
            int exit;
            try { Console.SetOut(output); exit = SemanticCommandLine.Run(new[] { "--describe-language-profile", profile.Name }); }
            finally { Console.SetOut(previousOutput); }
            Check(exit == 0 && output.ToString().Trim() == profile.DescribeJson(), "public description is the canonical owner output");
        }
        Check(SemanticCommandLine.Run(new[] { "--describe-language-profile", "gameplay-v2" }) == 2, "description rejects unknown versions");
        string directory = Directory.CreateTempSubdirectory("AvidScript.GameplayProfile.").FullName;
        string PathOf(string name) => Path.Combine(directory, name);
        string[] names = { "source.cs", "facade.cs", "frontend.json", "semantic-cli.json", "semantic-worker.json",
            "cli.guest.json", "cli.state.json", "cli.debug.json", "cli.wasm", "cli.offsets.json", "cli.inspect.json",
            "worker.guest.json", "worker.state.json", "worker.debug.json", "worker.wasm", "worker.inspect.json", "worker.debug.json.offsets.json",
            "wire-semantic.json", "wire-guest.json", "full.guest.json", "full.state.json", "full.debug.json" };
        try
        {
            File.WriteAllText(PathOf("facade.cs"), CSharpGuestCancellationTokenTests.AsyncFacade);
            var executor = new CompilerStageExecutor();
            CompilerWorkerRequest Request(string stage) => new() { ProtocolVersion = 2, RequestId = "profile-" + stage,
                ToolchainFingerprint = Fingerprint, Stage = stage, LanguageProfile = profile.Identity };
            CompilerWorkerRequest SemanticRequest() => Request("semantic") with { SourcePath = PathOf("source.cs"), SourceId = SourceId,
                FrontendPath = PathOf("frontend.json"), OutputPath = PathOf("semantic-worker.json"), ExecutableReferenceSourcePath = PathOf("facade.cs") };
            CompilerWorkerRequest GuestRequest(string frontendHash) => Request("guest") with { SemanticPath = PathOf("semantic-worker.json"),
                GuestIrPath = PathOf("worker.guest.json"), StateSchemaPath = PathOf("worker.state.json"), DebugMapPath = PathOf("worker.debug.json"),
                WasmPath = PathOf("worker.wasm"), InspectionPath = PathOf("worker.inspect.json"), FrontendArtifactSha256 = frontendHash,
                ModuleId = "gameplay_profile_test" };
            byte[] RunSource(string source, string label)
            {
                File.WriteAllText(PathOf("source.cs"), source);
                var frontend = FrontendAnalyzer.Analyze(source, SourceId);
                byte[] frontendBytes = FrontendSerializer.Serialize(frontend);
                File.WriteAllBytes(PathOf("frontend.json"), frontendBytes);
                string[] semanticArgs = { "--source", PathOf("source.cs"), "--source-id", SourceId, "--frontend", PathOf("frontend.json"),
                    "--output", PathOf("semantic-cli.json"), "--executable-reference-source", PathOf("facade.cs"), "--language-profile", profile.Name };
                int cliExit = SemanticCommandLine.Run(semanticArgs);
                var response = executor.Execute(SemanticRequest(), "profile-tests", Fingerprint);
                Check(cliExit is 0 or 1 && response.ExitCode == cliExit && response.ProtocolVersion == 2
                    && response.LanguageProfile == profile.Identity, label + " semantic response identity and honest source diagnostics");
                byte[] semanticBytes = File.ReadAllBytes(PathOf("semantic-cli.json"));
                Check(semanticBytes.SequenceEqual(File.ReadAllBytes(PathOf("semantic-worker.json"))), label + " CLI/worker semantic bytes");
                var api = SemanticAnalyzer.Analyze(source, SourceId, frontend.Source.Sha256,
                    new[] { new SemanticReferenceSource(CSharpGuestCancellationTokenTests.AsyncFacade, "reference:0:facade.cs", true) },
                    new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true,
                    enableAsyncCancellationFlow: true, enableAsyncSynchronousExceptions: true, enableStaticInitialization: true,
                    enableAsyncCatchVariables: true, enableCancellationTokens: true, enableAsyncVoidErrorOwner: true);
                Check(semanticBytes.SequenceEqual(SemanticSerializer.Serialize(api)), label + " profile is the exact API policy");
                var replay = executor.Execute(SemanticRequest(), "profile-tests", Fingerprint);
                Check(replay.Workspace.SyntaxTreeCacheHits > response.Workspace.SyntaxTreeCacheHits
                    && semanticBytes.SequenceEqual(File.ReadAllBytes(PathOf("semantic-worker.json"))), label + " workspace reuse preserves source bytes");
                Check(GuestCommandLine.Run(new[] { "--semantic", PathOf("semantic-cli.json"), "--output", PathOf("cli.guest.json"),
                    "--state-schema", PathOf("cli.state.json"), "--debug-map", PathOf("cli.debug.json"), "--frontend-artifact-sha256", Hash(frontendBytes),
                    "--module-id", "gameplay_profile_test", "--language-profile", profile.Name }) == 0, label + " single-option Guest CLI");
                using (var admission = JsonDocument.Parse(CSharpLanguageProfileAdmission.Describe(profile.Name, semanticBytes, "gameplay_profile_test")))
                {
                    var record = admission.RootElement;
                    Check(record.GetProperty("guest_ir_sha256").GetString() == Hash(File.ReadAllBytes(PathOf("cli.guest.json")))
                        && record.GetProperty("semantic_sha256").GetString() == Hash(semanticBytes), label + " owner admission covers exact source and Guest bytes");
                    Check(record.GetProperty("semantic_succeeded").GetBoolean() == api.Succeeded
                        && record.GetProperty("source_id").GetString() == SourceId, label + " admission preserves source diagnostics and identity");
                    using var admissionOutput = new StringWriter();
                    int admissionExit;
                    try { Console.SetOut(admissionOutput); admissionExit = GuestCommandLine.Run(new[] { "--validate-language-profile", profile.Name,
                        "--semantic", PathOf("semantic-cli.json"), "--module-id", "gameplay_profile_test" }); }
                    finally { Console.SetOut(previousOutput); }
                    Check(admissionExit == 0 && admissionOutput.ToString().Trim() == admission.RootElement.GetRawText(), label + " public read-only admission matches the API");
                }
                Check(WasmBackendCommandLine.Run(new[] { PathOf("cli.guest.json"), PathOf("cli.wasm"), "--debug-offsets", PathOf("cli.offsets.json") }) == 0
                    && GuestCommandLine.Run(new[] { "--finalize-debug-map", PathOf("cli.debug.json"), "--offset-map", PathOf("cli.offsets.json") }) == 0
                    && WasmBackendCommandLine.Run(new[] { "--inspect", PathOf("cli.wasm"), PathOf("cli.inspect.json") }) == 0, label + " finalized CLI artifacts");
                var guest = executor.Execute(GuestRequest(Hash(frontendBytes)), "profile-tests", Fingerprint);
                Check(guest.Succeeded && guest.ProtocolVersion == 2 && guest.LanguageProfile == profile.Identity, label + " worker Guest policy identity");
                foreach (string extension in new[] { "guest.json", "state.json", "debug.json", "wasm" })
                    Check(File.ReadAllBytes(PathOf("cli." + extension)).SequenceEqual(File.ReadAllBytes(PathOf("worker." + extension))),
                        label + " CLI/worker exact " + extension);
                using (var cliInspection = JsonDocument.Parse(File.ReadAllBytes(PathOf("cli.inspect.json"))))
                using (var workerInspection = JsonDocument.Parse(File.ReadAllBytes(PathOf("worker.inspect.json"))))
                {
                    Check(cliInspection.RootElement.GetProperty("wasm_file").GetString() == PathOf("cli.wasm")
                        && workerInspection.RootElement.GetProperty("wasm_file").GetString() == PathOf("worker.wasm"), label + " inspection names its actual artifact");
                    Check(cliInspection.RootElement.EnumerateObject().Count() == workerInspection.RootElement.EnumerateObject().Count()
                        && cliInspection.RootElement.EnumerateObject().Where(property => property.Name != "wasm_file").All(property =>
                            workerInspection.RootElement.GetProperty(property.Name).GetRawText() == property.Value.GetRawText()),
                        label + " inspection contracts match except their actual file paths");
                }
                Check(GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(File.ReadAllBytes(PathOf("worker.guest.json")))).Succeeded,
                    label + " independent execution validation");
                return File.ReadAllBytes(PathOf("worker.wasm"));
            }
            foreach (bool statics in new[] { false, true })
            foreach (bool tokens in new[] { false, true })
            foreach (bool named in new[] { false, true })
                RunSource(CSharpGuestCapabilityCliTests.TaskSource(named, tokens, statics), $"Task {statics}/{tokens}/{named}");
            foreach (bool statics in new[] { false, true })
                RunSource(CSharpGuestAsyncVoidCompositionTests.Source("await AvidContinuations.NextTickAsync(); Sync(-1);", statics, false), "async void " + statics);
            string original = CSharpGuestCapabilityCliTests.TaskSource(true, true, true);
            byte[] before = RunSource(original, "before edit");
            byte[] after = RunSource(original.Replace("Sync(7)", "Sync(8)", StringComparison.Ordinal), "after edit");
            Check(!before.SequenceEqual(after), "changed source cannot reuse the prior executable");
            RejectRequests(SemanticRequest(), GuestRequest(Hash(File.ReadAllBytes(PathOf("frontend.json")))), Check);
            PipeRoundTrip(SemanticRequest(), GuestRequest(Hash(File.ReadAllBytes(PathOf("frontend.json")))), Check);
            string legacySource = "using System.Runtime.InteropServices; public static class Script { public static int Next() { return 7; } "
                + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static void BeginPlay() { int value = Next(); } }";
            File.WriteAllText(PathOf("source.cs"), legacySource);
            byte[] legacyFrontend = FrontendSerializer.Serialize(FrontendAnalyzer.Analyze(legacySource, SourceId));
            File.WriteAllBytes(PathOf("frontend.json"), legacyFrontend);
            var legacySemantic = SemanticRequest() with { ProtocolVersion = 1, LanguageProfile = null, ExecutableReferenceSourcePath = "" };
            var legacyResponse = executor.Execute(legacySemantic, "profile-tests", Fingerprint);
            Check(legacyResponse.Succeeded && legacyResponse.ProtocolVersion == 1 && legacyResponse.LanguageProfile is null
                && SemanticCommandLine.Run(new[] { "--source", PathOf("source.cs"), "--source-id", SourceId, "--frontend", PathOf("frontend.json"),
                    "--output", PathOf("semantic-cli.json") }) == 0
                && File.ReadAllBytes(PathOf("semantic-cli.json")).SequenceEqual(File.ReadAllBytes(PathOf("semantic-worker.json"))), "legacy Semantic pipeline remains byte-identical");
            var legacyGuest = executor.Execute(GuestRequest(Hash(legacyFrontend)) with { ProtocolVersion = 1, LanguageProfile = null, ModuleId = null },
                "profile-tests", Fingerprint);
            Check(legacyGuest.Succeeded && legacyGuest.ProtocolVersion == 1 && legacyGuest.LanguageProfile is null
                && GuestCommandLine.Run(new[] { "--semantic", PathOf("semantic-cli.json"), "--output", PathOf("cli.guest.json"), "--state-schema", PathOf("cli.state.json"),
                    "--debug-map", PathOf("cli.debug.json"), "--frontend-artifact-sha256", Hash(legacyFrontend) }) == 0
                && File.ReadAllBytes(PathOf("cli.guest.json")).SequenceEqual(File.ReadAllBytes(PathOf("worker.guest.json"))), "legacy Guest pipeline remains byte-identical");
            byte[] validSource = File.ReadAllBytes(PathOf("semantic-worker.json"));
            var unsupported = SemanticSerializer.Deserialize(validSource) with { SemanticVersion = "unsupported" };
            bool sourceRejected = false;
            try { CSharpLanguageProfileAdmission.Describe(profile.Name, SemanticSerializer.Serialize(unsupported)); }
            catch (InvalidDataException) { sourceRejected = true; }
            Check(sourceRejected, "owner admission rejects an unsupported source contract");
            byte[] priorGuest = File.ReadAllBytes(PathOf("cli.guest.json"));
            Check(GuestCommandLine.Run(new[] { "--validate-language-profile", "gameplay-v2", "--semantic", PathOf("semantic-worker.json") }) == 2
                && priorGuest.SequenceEqual(File.ReadAllBytes(PathOf("cli.guest.json"))), "invalid read-only admission preserves existing publications");
            Check(GuestCommandLine.Run(new[] { "--semantic", PathOf("semantic-worker.json"), "--output", PathOf("full.guest.json"),
                "--state-schema", PathOf("full.state.json"), "--debug-map", PathOf("full.debug.json"), "--frontend-artifact-sha256", Hash(legacyFrontend),
                "--data-lane-fusion", "enabled", "--debug-instrumentation", "disabled", "--implicit-function-import-count", "1",
                "--language-errors", "bounded", "--module-id", "full-options", "--language-profile", profile.Name }) == 0,
                "profile accepts all compatible options including the implicit safepoint import");
            var fullModule = GuestIrSerializer.Deserialize(File.ReadAllBytes(PathOf("full.guest.json")));
            Check(GuestModuleValidator.Validate(fullModule).Succeeded
                && CSharpGuestDebugMapSerializer.Deserialize(File.ReadAllBytes(PathOf("full.debug.json"))).ImportedFunctionCount == fullModule.Imports.Count + 1,
                "full option set preserves a valid IR and accounts for the implicit import in debug maps");
            foreach (string option in new[] { "--static-initialization", "--async-catch-variables", "--async-void-error-owner" })
                Check(SemanticCommandLine.Run(new[] { "--source", PathOf("source.cs"), "--source-id", SourceId, "--frontend", PathOf("frontend.json"),
                    "--output", PathOf("semantic-cli.json"), "--language-profile", profile.Name, option, "disabled" }) == 2,
                    "profile rejects explicit analysis override " + option);
            foreach (var conflict in new[] { ("--language-profile", "gameplay-v2"), ("--debug-instrumentation", "enabled"),
                ("--data-lane-fusion", "disabled"), ("--language-errors", "disabled") })
            {
                var args = new List<string> { "--semantic", PathOf("semantic-worker.json"), "--output", PathOf("cli.guest.json"),
                    "--language-profile", conflict.Item1 == "--language-profile" ? conflict.Item2 : profile.Name };
                if (conflict.Item1 != "--language-profile") args.AddRange(new[] { conflict.Item1, conflict.Item2 });
                Check(GuestCommandLine.Run(args.ToArray()) == 2 && !File.Exists(PathOf("cli.guest.json")), "conflict cannot publish Guest output");
            }
        }
        finally
        {
            foreach (string name in names) File.Delete(PathOf(name));
            if (!Directory.EnumerateFileSystemEntries(directory).Any()) Directory.Delete(directory);
        }
        return count;
    }

    private static void CheckUnusedTokenFacade(Action<bool, string> check)
    {
        const string source = "using System.Runtime.InteropServices; public static class Script { public static int Main() => 0; "
            + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static void BeginPlay() { } "
            + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_tick\")] public static void Tick(float deltaSeconds) { } "
            + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_end_play\")] public static void EndPlay() { } }";
        const string facade = """
            using System.Runtime.CompilerServices;
            namespace AvidScript;
            public readonly struct AvidCancellationToken {
                internal readonly long Value;
                public bool IsValid => Value != 0;
                [MethodImpl(MethodImplOptions.InternalCall)]
                public static extern implicit operator System.Threading.CancellationToken(AvidCancellationToken token);
            }
            public readonly struct AvidDelayAwaitable {
                public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => default;
                public AvidDelayAwaitable WithCancellation(System.Threading.CancellationToken token) => default;
            }
            public readonly struct AvidObjectAwaitable {
                public AvidObjectAwaitable WithCancellation(AvidCancellationToken token) => default;
                public AvidObjectAwaitable WithCancellation(System.Threading.CancellationToken token) => default;
            }
            public readonly struct AvidOutcomeAwaitable<T> {
                public AvidOutcomeAwaitable<T> WithCancellation(AvidCancellationToken token) => default;
                public AvidOutcomeAwaitable<T> WithCancellation(System.Threading.CancellationToken token) => default;
            }
            """;
        var references = new[] { new SemanticReferenceSource(facade, "reference:0:unused-token.cs", true) };
        var legacy = SemanticAnalyzer.Analyze(source, SourceId, Hash(Encoding.UTF8.GetBytes(source)), references, new SemanticCompilerWorkspace());
        var profiled = SemanticAnalyzer.Analyze(source, SourceId, Hash(Encoding.UTF8.GetBytes(source)), references,
            new SemanticCompilerWorkspace(), true, true, true, true, true, true, true, true);
        check(legacy.Succeeded && profiled.Succeeded && legacy.SchemaVersion == 31 && profiled.SchemaVersion == 31,
            "unused token facade keeps ordinary source under its original contract");
        check(!profiled.Types.Any(type => type.Id == "type:global::System.NullReferenceException"),
            "unused token signatures do not register implicit receiver exception types");
        check(JsonSerializer.Serialize(legacy) == JsonSerializer.Serialize(profiled),
            "unused generated token overloads preserve exact ordinary Semantic bytes");
    }

    private static void RejectRequests(CompilerWorkerRequest semantic, CompilerWorkerRequest guest, Action<bool, string> check)
    {
        foreach (var request in new[] { semantic with { ProtocolVersion = 1 }, semantic with { LanguageProfile = null },
            semantic with { LanguageProfile = new("gameplay-v2", semantic.LanguageProfile!.ContractSha256) },
            semantic with { LanguageProfile = semantic.LanguageProfile! with { ContractSha256 = new string('b', 64) } },
            semantic with { ProtocolVersion = 3 }, semantic with { Stage = "frontend" }, semantic with { ModuleId = "wrong-stage" },
            guest with { ModuleId = null }, guest with { DebugInstrumentation = "enabled" }, guest with { DataLaneFusion = "disabled" } })
        {
            bool rejected = false;
            try { CompilerWorkerRequestValidator.Validate(request, Fingerprint); } catch (ArgumentException) { rejected = true; }
            check(rejected, "worker rejects mixed versions and forged policy before execution");
        }
        var legacy = new CompilerWorkerRequest { ProtocolVersion = 1, RequestId = "legacy", ToolchainFingerprint = Fingerprint, Stage = "ping" };
        string json = CompilerWorkerJson.SerializeRequest(legacy);
        check(!json.Contains("language_profile", StringComparison.Ordinal) && !json.Contains("module_id", StringComparison.Ordinal), "legacy wire fields remain absent");
        check(CompilerWorkerJson.DeserializeRequest(CompilerWorkerJson.SerializeRequest(semantic)) == semantic, "versioned identity round trip");
        check(!CompilerWorkerJson.SerializeResponse(new CompilerStageExecutor().Execute(legacy, "profile-tests", Fingerprint))
            .Contains("language_profile", StringComparison.Ordinal), "legacy response remains unchanged");
    }

    private static void PipeRoundTrip(CompilerWorkerRequest semantic, CompilerWorkerRequest guest, Action<bool, string> check)
    {
        string pipeName = "AvidScript.GameplayProfile." + Guid.NewGuid().ToString("N");
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var server = new CompilerWorkerServer(new(pipeName, Fingerprint, TimeSpan.FromSeconds(10)));
        Task<int> running = server.RunAsync(cancellation.Token);
        CompilerWorkerResponse Send(CompilerWorkerRequest request)
        {
            using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            pipe.ConnectAsync(cancellation.Token).GetAwaiter().GetResult();
            using var writer = new StreamWriter(pipe, new UTF8Encoding(false), 4096, true) { AutoFlush = true };
            using var reader = new StreamReader(pipe, new UTF8Encoding(false, true), false, 4096, true);
            writer.WriteLine(CompilerWorkerJson.SerializeRequest(request));
            return CompilerWorkerJson.DeserializeResponse(reader.ReadLineAsync(cancellation.Token).AsTask().GetAwaiter().GetResult()!);
        }
        try
        {
            var malformed = Send(semantic with { LanguageProfile = null });
            check(!malformed.Succeeded && malformed.ProtocolVersion == 2 && malformed.ExitCode == 2, "pipe rejects incomplete profile without downgrading the response");
            var source = Send(semantic);
            check(source.ExitCode is 0 or 1 && source.LanguageProfile == semantic.LanguageProfile && source.ProtocolVersion == 2, "pipe carries source profile identity");
            var executable = Send(guest);
            check(executable.Succeeded && executable.LanguageProfile == guest.LanguageProfile && executable.ProtocolVersion == 2,
                "pipe carries execution profile identity");
            string directory = Path.GetDirectoryName(semantic.SourcePath)!;
            string semanticRequestPath = Path.Combine(directory, "wire-semantic.json"), guestRequestPath = Path.Combine(directory, "wire-guest.json");
            File.WriteAllText(semanticRequestPath, CompilerWorkerJson.SerializeRequest(semantic));
            File.WriteAllText(guestRequestPath, CompilerWorkerJson.SerializeRequest(guest));
            string pluginRoot = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../.."));
            var start = new ProcessStartInfo("pwsh") { UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (string argument in new[] { "-NoProfile", "-File", Path.Combine(pluginRoot, "Build/Contracts/TestCSharpLanguageProfileWorkerClient.ps1"),
                "-PipeName", pipeName, "-SemanticRequestPath", semanticRequestPath, "-GuestRequestPath", guestRequestPath }) start.ArgumentList.Add(argument);
            using (var client = Process.Start(start)!)
            {
                client.PriorityClass = ProcessPriorityClass.BelowNormal;
                Task<string> output = client.StandardOutput.ReadToEndAsync(), error = client.StandardError.ReadToEndAsync();
                if (!client.WaitForExit(20000)) { client.Kill(); throw new InvalidOperationException("Owned PS pipe client timed out."); }
                check(client.ExitCode == 0 && output.GetAwaiter().GetResult().Contains("AvidScript.LanguageProfileWorkerClient: 9/9 passed", StringComparison.Ordinal),
                    "actual PS client and forged response rejection: " + error.GetAwaiter().GetResult());
            }
            var shutdown = Send(new() { ProtocolVersion = 1, RequestId = "shutdown", ToolchainFingerprint = Fingerprint, Stage = "shutdown" });
            check(shutdown.Succeeded && shutdown.ProtocolVersion == 1 && shutdown.LanguageProfile is null, "legacy shutdown remains compatible");
            check(running.Wait(TimeSpan.FromSeconds(5)) && running.Result == 0, "owned server exits normally");
        }
        finally
        {
            cancellation.Cancel();
            try { running.GetAwaiter().GetResult(); } catch (OperationCanceledException) { }
        }
    }
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
