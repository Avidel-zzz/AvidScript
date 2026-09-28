using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestCancellationTokenTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_CSHARP_TOKEN_FIXTURE_DIR");
        var fixtures = new System.Collections.Generic.List<object>();
        foreach (var (name, members, expectedBase) in new[] {
            ("none", "public static int Main() { CancellationToken a = CancellationToken.None; CancellationToken b = new CancellationToken(); return a == b && a.Equals(default(CancellationToken)) && !(a != b) ? 1 : 0; }", 14),
            ("copy", "static CancellationToken Copy(CancellationToken input) { return input; } public static int Main() { var a = Copy(default(CancellationToken)); return a.Equals(Copy(a)) ? 1 : 0; }", 14),
            ("convert", "public static int Main() { CancellationToken a = new AvidCancellationToken(4294967297L); CancellationToken b = new AvidCancellationToken(1L); return a != b && a == (CancellationToken)new AvidCancellationToken(4294967297L) ? 1 : 0; }", 14),
            ("value-call", "static CancellationToken Copy(CancellationToken input) { return input; } public static int Main() { CancellationToken a = new AvidCancellationToken(-9223372036854775808L); CancellationToken b = new AvidCancellationToken(9223372036854775807L); return Copy(a) == a && Copy(b) == b && Copy(a) != Copy(b) ? 1 : 0; }", 14),
            ("parameter-values", "static CancellationToken Copy(CancellationToken input) { return input; } [UnmanagedCallersOnly(EntryPoint = \"token_same\")] public static int Same(long left, long right) { CancellationToken a = new AvidCancellationToken(left); CancellationToken b = new AvidCancellationToken(right); return Copy(a) == Copy(b) ? 1 : 0; } public static int Main() { return Copy(CancellationToken.None) == CancellationToken.None ? 1 : 0; }", 14),
            ("evaluation-order", "static int Trace; static CancellationToken Make(int step, long value) { Trace = Trace * 10 + step; return new AvidCancellationToken(value); } public static int Main() { Trace = 0; bool equal = Make(1, 4294967297L) == Make(2, 4294967297L); bool different = Make(3, 4294967297L) != Make(4, 1L); bool typed = Make(5, 4294967297L).Equals(Make(6, 4294967297L)); return equal && different && typed && Trace == 123456 ? 1 : 0; }", 14),
            ("comparison-snapshot", "static CancellationToken Saved; static CancellationToken Change() { Saved = new AvidCancellationToken(1L); return Saved; } public static int Main() { Saved = new AvidCancellationToken(4294967297L); return Saved != Change() && Saved == (CancellationToken)new AvidCancellationToken(1L) ? 1 : 0; }", 14),
            ("null", "public static int Main() { OperationCanceledException error = null; try { var token = error.CancellationToken; return 0; } catch (NullReferenceException) { return 1; } }", 17),
            ("helper-null", "static CancellationToken Read(OperationCanceledException error) { return error.CancellationToken; } static CancellationToken Forward(OperationCanceledException error) { return Read(error); } public static int Main() { try { var token = Forward(null); return 0; } catch (NullReferenceException) { return 1; } }", 17),
            ("receiver-once", "static int Trace; static OperationCanceledException Receiver() { Trace++; return null; } public static int Main() { Trace = 0; try { var token = Receiver().CancellationToken; return 0; } catch (NullReferenceException) { return Trace == 1 ? 1 : 0; } }", 17),
            ("null-finally", "static int Trace; static CancellationToken Read(OperationCanceledException error) { try { return error.CancellationToken; } finally { Trace = Trace * 10 + 1; } } public static int Main() { Trace = 0; try { var token = Read(null); return 0; } catch (NullReferenceException) { Trace = Trace * 10 + 2; } return Trace == 12 ? 1 : 0; }", 17),
        }.Concat(AsyncCases())) {
            string source = "using System; using System.Threading; using System.Threading.Tasks; using System.Runtime.InteropServices; using AvidScript; public static class Script { " + members
                + " [UnmanagedCallersOnly(EntryPoint = \"token_main\")] public static int Entry() { return Main(); } }";
            Check(CSharpGuestCancellationTokenReference.Execute(source, expectedBase == 29) == 1, name + " same-source .NET result");
            string sourceId = "Scripts/Token_" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                new[] { new SemanticReferenceSource(expectedBase == 29 ? AsyncFacade : Facade, "generated://Tokens.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableAsyncSynchronousExceptions: true, enableAsyncCatchVariables: true, enableCancellationTokens: true);
            Check(SemanticContract.HasCancellationTokens(semantic) && SemanticCancellationTokenValidator.IsValid(semantic),
                name + " semantic: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
            if (!string.IsNullOrWhiteSpace(directory)) {
                Directory.CreateDirectory(directory);
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
            }
            byte[] sourceJson = SemanticSerializer.Serialize(semantic);
            string hash = Convert.ToHexString(SHA256.HashData(sourceJson)).ToLowerInvariant();
            Check(CSharpCancellationTokenCompiler.TryLower(semantic, hash, out var module, out var error)
                && module is not null, name + " lowering: " + error + " flow=" + SemanticExceptionFlowContractValidator.IsValid(semantic)
                + " scope=" + SemanticAsyncScopeValidator.IsValid(semantic));
            Check(module!.CancellationTokens?.BaseSchemaVersion == expectedBase && GuestModuleValidator.Validate(module).Succeeded,
                name + " execution base");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            Check(module.Exports.Any(export => export.Name == "token_main"), name + " callable WASM entry");
            var json = GuestIrSerializer.Serialize(module);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json)))
                && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json)).Bytes), name + " determinism");
            Check(CSharpCancellationTokenCompiler.TryLower(semantic, hash, out var repeated, out _)
                && json.SequenceEqual(GuestIrSerializer.Serialize(repeated!)), name + " deterministic repeated source lowering");
            string[] provenance = WasmArtifactInspector.Inspect(wasm.Bytes).CustomSections
                .Single(section => section.Name == "avidscript.provenance").PayloadText.Split('\n');
            Check(provenance.Contains("guest_ir=34/1.33") && provenance.Contains("semantic=53/1.62")
                && provenance.Contains($"guest_ir_base={expectedBase}/1.{expectedBase - 1}"), name + " emitted source/execution identity");
            Check(!CSharpGuestLowerer.Lower(semantic, hash).Succeeded
                && !CSharpLanguageErrorCompiler.TryLower(semantic, hash, out _, out _), name + " compiler context does not authorize caller document");
            Check(sourceJson.SequenceEqual(SemanticSerializer.Serialize(semantic)), name + " caller source remains unchanged");
            if (name is "convert" or "null" or "async-catch-token") CheckRejectedSources(semantic, hash, Check, name == "convert");
            if (!string.IsNullOrWhiteSpace(directory)) {
                Directory.CreateDirectory(directory);
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
                File.WriteAllBytes(Path.Combine(directory, name + ".guestir.json"), json);
                File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
                File.WriteAllText(Path.Combine(directory, name + ".exports.json"), JsonSerializer.Serialize(module.Exports));
                fixtures.Add(new { name, moduleId = module.ModuleId, asynchronous = expectedBase == 29,
                    expected = 1, trace = 0, semanticSha256 = hash,
                    sourceSha256 = semantic.Source.Sha256,
                    wasmSha256 = Convert.ToHexString(SHA256.HashData(wasm.Bytes)).ToLowerInvariant(),
                    resultOffset = expectedBase == 29 ? module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Result:", StringComparison.Ordinal)).Offset : (int?)null,
                    traceOffset = expectedBase == 29 ? module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Trace:", StringComparison.Ordinal)).Offset : (int?)null });
            }
        }
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;
    }

    private static void CheckRejectedSources(SemanticDocument source, string hash, Action<bool, string> check, bool conversion)
    {
        void Reject(SemanticDocument invalid, string reason) => check(
            !CSharpCancellationTokenCompiler.TryLower(invalid, hash, out var rejected, out var error)
                && rejected is null && !string.IsNullOrWhiteSpace(error), "Token source rejects " + reason);
        Reject(null!, "null document");
        Reject(source with { Source = null! }, "missing source");
        Reject(source with { Types = null! }, "missing types");
        Reject(source with { Methods = null! }, "missing methods");
        Reject(source with { Callables = null! }, "missing callables");
        Reject(source with { Symbols = null! }, "missing symbols");
        Reject(source with { Diagnostics = null! }, "missing diagnostics");
        Reject(source with { CapabilityManifest = new(50, "1.59", new[] {
            new SemanticCapability(SemanticComposableCapabilities.CancellationTokenValue, 1) }) },
            "capability manifest on Semantic 53");
        Reject(source with { ControlFlowGraphs = null! }, "missing control flow");
        Reject(source with { AsyncMethods = null! }, "missing async methods");
        Reject(source with { Methods = source.Methods.Append(null!).ToArray() }, "null method");
        Reject(source with { Callables = source.Callables.Append(null!).ToArray() }, "null callable");
        Reject(source with { Symbols = source.Symbols.Append(null!).ToArray() }, "null symbol");
        Reject(source with { Diagnostics = source.Diagnostics.Append(null!).ToArray() }, "null diagnostic");
        Reject(source with { Methods = source.Methods.Append(source.Methods[0]).ToArray() }, "duplicate method");
        Reject(source with { Callables = source.Callables.Append(source.Callables[0]).ToArray() }, "duplicate callable");
        Reject(source with { Symbols = source.Symbols.Append(source.Symbols[0]).ToArray() }, "duplicate symbol");
        if (!conversion) {
            var method = source.Methods.First(method => Nodes(method.Root).Any(node => node.Kind == SemanticCancellationTokens.Read));
            var read = Nodes(method.Root).First(node => node.Kind == SemanticCancellationTokens.Read);
            Reject(source with { Methods = source.Methods.Select(item => item != method ? item : item with {
                Root = item.Root with { Children = item.Root.Children.Append(read with {
                    Span = read.Span with { Length = read.Span.Length + 1 } }).ToArray() } }).ToArray() }, "conflicting getter guard identity");
        }
        foreach (var version in new[] { (52, "1.61"), (53, "1.61"), (52, "1.62"), (54, "1.63") })
            Reject(source with { SchemaVersion = version.Item1, SemanticVersion = version.Item2 }, "unpaired/old/future version");
        Reject(source with { Types = source.Types.Where(type => type.Id != SemanticCancellationTokens.TypeId).ToArray() }, "missing nominal token");
        Reject(source with { Types = source.Types.Select(type => type.Id == SemanticCancellationTokens.TypeId
            ? type with { Kind = "class", IsValueType = false } : type).ToArray() }, "borrowed reference layout");
        Reject(source with { TypeShapes = source.TypeShapes.Append(new(SemanticCancellationTokens.TypeId, "type:int64", null)).ToArray() }, "borrowed array layout");
        if (conversion)
            Reject(source with { Callables = source.Callables.Select(callable => callable.ReturnTypeId == SemanticCancellationTokens.TypeId
                ? callable with { HasBody = true } : callable).ToArray() }, "untrusted conversion body");
        foreach (string invalidHash in new[] { "", "not-a-hash", new string('g', 64) })
            check(!CSharpCancellationTokenCompiler.TryLower(source, invalidHash, out var invalid, out _) && invalid is null,
                "Token lowering rejects malformed semantic identity");
    }

    private static System.Collections.Generic.IEnumerable<SemanticOperation> Nodes(SemanticOperation root)
    {
        yield return root;
        foreach (var child in root.Children)
            foreach (var node in Nodes(child)) yield return node;
    }

    private static System.Collections.Generic.IEnumerable<(string, string, int)> AsyncCases()
    {
        foreach (var (name, body) in new[] {
            ("async-none", "CancellationToken token = default; await AvidContinuations.NextTickAsync(); return token == CancellationToken.None ? 1 : 0;"),
            ("async-copy", "CancellationToken token = new AvidCancellationToken(4294967297L); await AvidContinuations.NextTickAsync(); return Copy(token) == (CancellationToken)new AvidCancellationToken(4294967297L) ? 1 : 0;"),
            ("async-with-token", "CancellationToken token = CancellationToken.None; await AvidContinuations.NextTickAsync().WithCancellation(token); return token == CancellationToken.None ? 1 : 0;"),
            ("async-catch-token", "var source = AvidCancellationSource.Create(); CancellationToken token = source.Token; source.Cancel(); try { await AvidContinuations.NextTickAsync().WithCancellation(token); } catch (OperationCanceledException error) { return error.CancellationToken == token ? 1 : 0; } return 0;"),
            ("async-saved-token", "var source = AvidCancellationSource.Create(); CancellationToken token = source.Token; source.Cancel(); OperationCanceledException saved = null; try { await AvidContinuations.NextTickAsync().WithCancellation(token); } catch (OperationCanceledException error) { saved = error; } source.Release(); await AvidContinuations.NextTickAsync(); return Read(saved) == token ? 1 : 0;"),
            ("async-null", "await AvidContinuations.NextTickAsync(); try { var token = Read(null); return 0; } catch (NullReferenceException) { return 1; }"),
            ("async-task-argument", "CancellationToken token = CancellationToken.None; int result = await Use(token); return result;"),
            ("async-task-catch-token", "var source = AvidCancellationSource.Create(); CancellationToken token = source.Token; source.Cancel(); try { await AvidContinuations.NextTickAsync().WithCancellation(token); } catch (TaskCanceledException error) { return error.CancellationToken.Equals(token) ? 1 : 0; } return 0;"),
            ("async-release-convert", "var source = AvidCancellationSource.Create(); var legacy = source.Token; CancellationToken before = legacy; source.Release(); CancellationToken after = legacy; await AvidContinuations.NextTickAsync(); return before == after ? 1 : 0;"),
            ("async-pending-cancel", "var source = AvidCancellationSource.Create(); CancellationToken token = source.Token; Task<int> cancellation = CancelNextTick(source); bool observed = false; try { await AvidContinuations.NextTickAsync().WithCancellation(token); } catch (OperationCanceledException error) { observed = error.CancellationToken == token; } int completed = await cancellation; source.Release(); return observed && completed == 1 ? 1 : 0;"),
        }) yield return (name, "public static int Result; public static int Trace; public static int Main() { return 0; } "
            + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static async void BeginPlay() { Result = await Run(); } "
            + (body.Contains("Copy(", StringComparison.Ordinal) ? "static CancellationToken Copy(CancellationToken token) { return token; } " : "")
            + (body.Contains("Read(", StringComparison.Ordinal) ? "static CancellationToken Read(OperationCanceledException error) { return error.CancellationToken; } " : "")
            + (body.Contains("Use(", StringComparison.Ordinal) ? "static async Task<int> Use(CancellationToken token) { await AvidContinuations.NextTickAsync().WithCancellation(token); return token == CancellationToken.None ? 1 : 0; } " : "")
            + (body.Contains("CancelNextTick(", StringComparison.Ordinal) ? "static async Task<int> CancelNextTick(AvidCancellationSource source) { await AvidContinuations.NextTickAsync(); source.Cancel(); return 1; } " : "")
            + "public static async Task<int> Run() { " + body + " }", 29);
    }

    private static string AsyncFacade => CSharpGuestContinuationTests.ReferenceFacade
        .Replace("internal AvidCancellationToken(long value) { Value = value; }",
            "internal AvidCancellationToken(long value) { Value = value; } [MethodImpl(MethodImplOptions.InternalCall)] public static extern implicit operator System.Threading.CancellationToken(AvidCancellationToken token);", StringComparison.Ordinal)
        .Replace("public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => default;",
            "public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => default; public AvidDelayAwaitable WithCancellation(System.Threading.CancellationToken token) => default;", StringComparison.Ordinal)
        + CSharpGuestAsyncThrowRoutingTests.CancelFacade;

    private const string Facade = """
        using System.Runtime.CompilerServices; using System.Threading;
        namespace AvidScript;
        public readonly struct AvidCancellationToken {
            internal readonly long Value;
            internal AvidCancellationToken(long value) { Value = value; }
            [MethodImpl(MethodImplOptions.InternalCall)]
            public static extern implicit operator CancellationToken(AvidCancellationToken token);
        }
        """;
}
