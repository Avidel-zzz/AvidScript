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

internal static class CSharpGuestAsyncVoidReloadTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Async void reload: " + message);
            count++;
        }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_VOID_RELOAD_FIXTURE_DIR");
        var expectations = new List<object>();
        var generations = new List<(GuestModule Module, CSharpGuestStateSchema State, byte[] Wasm)>();
        for (int generation = 0; generation < 2; generation++)
        {
            string name = generation == 0 ? "initial" : "candidate";
            int result = generation == 0 ? 17 : 37;
            int location = generation == 0 ? 125 : 500;
            string source = Source(result, location);
            for (int mode = 0; mode < 4; mode++)
            {
                var expected = Reference(source, mode);
                Check(expected == (mode is 1 or 2 ? "ArgumentException" : "", mode == 3 ? 21 : 1,
                    mode is 1 or 2 ? 0 : result, location + (mode == 3 ? 21 : 1)), name + " same-source .NET mode=" + mode + " outcome=" + expected);
                expectations.Add(new { generation = name, mode, errorType = expected.Error,
                    trace = expected.Trace, result = expected.Result, location = expected.Location });
            }
            const string sourceId = "Scripts/AsyncVoidReload.cs";
            var frontend = FrontendAnalyzer.Analyze(source, sourceId);
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true), new SemanticReferenceSource(HostFacade,
                    "generated://AsyncVoidReload.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableAsyncSynchronousExceptions: true, enableAsyncVoidErrorOwner: true);
            Check(SemanticContract.HasAsyncVoidErrorOwner(semantic) && SemanticAsyncInvocationValidator.IsValid(semantic),
                name + " source: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
            byte[] semanticBytes = SemanticSerializer.Serialize(semantic);
            Check(CSharpLanguageErrorCompiler.TryLower(semantic, Hash(semanticBytes), out var compiled, out var error)
                && compiled is not null, name + " lowering: " + error);
            var module = compiled!.Module;
            Check(GuestAsyncVoidErrorOwners.HasSourceContract(module) && GuestModuleValidator.Validate(module).Succeeded,
                name + " independent IR36 validation");
            var state = CSharpGuestStateSchemaProjector.Project(semantic, module);
            Check(state.OwnerTypeId == "type:global::Script" && state.Slots.Count == 5,
                name + " formal source state projection");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            var info = WasmArtifactInspector.Inspect(wasm.Bytes);
            string[] exports = { "avid_on_begin_play", "avid_on_tick", "avid_on_event", "avid_on_continuation_v2" };
            Check(exports.All(export => info.Exports.Any(item => item.Name == export && item.Kind == 0)),
                name + " actual lifecycle exports");
            byte[] irBytes = GuestIrSerializer.Serialize(module);
            Check(irBytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(irBytes)))
                && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), name + " deterministic artifacts");
            generations.Add((module, state, wasm.Bytes));
            if (string.IsNullOrWhiteSpace(directory)) continue;
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
            File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), semanticBytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".guestir.json"), irBytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            byte[] stateBytes = CSharpGuestStateSchemaSerializer.Serialize(state);
            File.WriteAllBytes(Path.Combine(directory, name + ".state.json"), stateBytes);
            using var stateJson = JsonDocument.Parse(stateBytes);
            var manifest = new {
                schema_version = 1, module_id = module.ModuleId, abi_version = 1, language = "csharp",
                wasm = new { file = name + ".wasm", sha256 = Hash(wasm.Bytes) },
                guest_ir = new { file = name + ".guestir.json", sha256 = Hash(irBytes),
                    module_id = module.ModuleId, schema_version = module.SchemaVersion, version = module.IrVersion },
                state_migration = stateJson.RootElement,
                required_exports = exports,
                required_imports = info.Imports.Where(item => item.Kind == 0)
                    .OrderBy(item => item.Module, StringComparer.Ordinal).ThenBy(item => item.Name, StringComparer.Ordinal)
                    .Select(item => new { module = item.Module, name = item.Name }).ToArray(),
            };
            File.WriteAllText(Path.Combine(directory, name + ".avidscript.json"), JsonSerializer.Serialize(manifest,
                new JsonSerializerOptions { WriteIndented = true }) + "\n");
        }
        Check(generations[0].Module.ModuleId == generations[1].Module.ModuleId
            && !generations[0].Wasm.SequenceEqual(generations[1].Wasm), "actual code update retains module identity");
        Check(CSharpGuestStateSchemaSerializer.Serialize(generations[0].State)
            .SequenceEqual(CSharpGuestStateSchemaSerializer.Serialize(generations[1].State)), "constant-only update retains canonical state schema");
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(expectations,
                new JsonSerializerOptions { WriteIndented = true }));
        return count;
    }

    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static string Source(int result, int location) => $$"""
        using AvidScript; using System; using System.Threading.Tasks; using System.Runtime.InteropServices;
        public static class Script {
            public static int BeginCount, Mode, Trace, Result, TickCount;
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
            public static void BeginPlay() { BeginCount++; Run(); }
            [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
            public static void Tick(float delta) { TickCount++; }
            [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
            public static void OnEvent(int mode, float unused) { Mode = mode; }
            public static async void Run() {
                try {
                    ReloadHost.SetLocation(ReloadHost.Slot(), ReloadHost.Generation(), {{location}}.0f, 600.0f, 700.0f);
                    Task<int> pending = Read();
                    if (Mode == 1) Fail();
                    if (Mode == 3) { try { Fail(); } catch (ArgumentException) { Trace = 2; } }
                    int value = await pending;
                    if (Mode == 2) Fail();
                    Result = value;
                } finally {
                    Trace = Trace * 10 + 1;
                    ReloadHost.SetLocation(ReloadHost.Slot(), ReloadHost.Generation(), {{location}}.0f + Trace, 600.0f, 700.0f);
                }
            }
            public static int Fail() { throw new ArgumentException(); }
            public static async Task<int> Read() {
                await AvidContinuations.NextTickAsync();
                await AvidContinuations.NextTickAsync();
                return {{result}};
            }
        }
        """;

    private const string HostFacade = """
        namespace AvidScript { public static class ReloadHost {
            [System.Runtime.InteropServices.DllImport("env", EntryPoint = "owner_get_slot")] public static extern int Slot();
            [System.Runtime.InteropServices.DllImport("env", EntryPoint = "owner_get_generation")] public static extern int Generation();
            [System.Runtime.InteropServices.DllImport("env", EntryPoint = "actor_set_location")]
            public static extern int SetLocation(int slot, int generation, float x, float y, float z);
        } }
        """;

    private static (string Error, int Trace, int Result, int Location) Reference(string source, int mode)
    {
        const string facade = """
            namespace AvidScript {
                public static class ReloadHost {
                    public static int Location;
                    public static int Slot() => 1; public static int Generation() => 1;
                    public static int SetLocation(int slot, int generation, float x, float y, float z) { Location = (int)x; return 1; }
                }
                public static class AvidContinuations {
                    private static readonly System.Collections.Generic.Queue<System.Threading.Tasks.TaskCompletionSource> Pending = new();
                    public static System.Threading.Tasks.Task NextTickAsync() {
                        var completion = new System.Threading.Tasks.TaskCompletionSource(); Pending.Enqueue(completion); return completion.Task;
                    }
                    public static bool Step() {
                        if (!Pending.TryDequeue(out var completion)) return false; completion.SetResult(); return true;
                    }
                }
            }
            """;
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("AsyncVoidReloadReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(facade) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("async-void-reload-reference", isCollectible: true);
        var previous = SynchronizationContext.Current;
        var pump = new ErrorPump();
        try
        {
            SynchronizationContext.SetSynchronizationContext(pump);
            var assembly = context.LoadFromStream(bytes);
            var script = assembly.GetType("Script")!;
            script.GetField("Mode")!.SetValue(null, mode);
            script.GetMethod("Run")!.Invoke(null, null);
            var step = assembly.GetType("AvidScript.AvidContinuations")!.GetMethod("Step")!;
            for (int tick = 0; ; tick++)
            {
                if (tick >= 256) throw new InvalidOperationException("Async void reload reference exceeded its scheduler budget.");
                if (pump.Drain() || (bool)step.Invoke(null, null)!) continue;
                if (pump.Active != 0) throw new InvalidOperationException("Async void reload reference stalled.");
                break;
            }
            if (pump.Errors.Count > 1) throw new InvalidOperationException("Async void reload error reported twice.");
            return (pump.Errors.SingleOrDefault()?.GetType().Name ?? "", (int)script.GetField("Trace")!.GetValue(null)!,
                (int)script.GetField("Result")!.GetValue(null)!, (int)assembly.GetType("AvidScript.ReloadHost")!.GetField("Location")!.GetValue(null)!);
        }
        finally { SynchronizationContext.SetSynchronizationContext(previous); context.Unload(); }
    }

    private sealed class ErrorPump : SynchronizationContext
    {
        private readonly Queue<(SendOrPostCallback Callback, object? State)> posts = new();
        public readonly List<Exception> Errors = new();
        public int Active { get; private set; }
        public override void OperationStarted() => Active++;
        public override void OperationCompleted() => Active--;
        public override void Post(SendOrPostCallback callback, object? state) => posts.Enqueue((callback, state));
        public bool Drain()
        {
            if (!posts.TryDequeue(out var post)) return false;
            try { post.Callback(post.State); } catch (Exception error) { Errors.Add(error); }
            return true;
        }
    }
}
