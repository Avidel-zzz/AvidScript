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

internal static class CSharpGuestStaticAsyncExecutionTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException("Static async: " + reason); count++; }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_STATIC_ASYNC_FIXTURE_DIR");
        List<object> fixtures = new();
        const string cache = "public static class Cache { public static Target Current = new Target(); public static int Scalar; static Cache() { Script.Mark(1); } }";
        const string broken = "public static class Broken { public static int Value; static Broken() { Script.Mark(4); throw new InvalidOperationException(); } }";
        foreach (bool deferred in new[] { false, true })
        foreach (var scenario in new[] {
            ("member", cache, "Target original = Cache.Current; try { Cache.Current.Value = await Read(7); return original.Value; } finally { Mark(8); }"),
            ("replacement", cache, "Target original = Cache.Current; try { Cache.Current.Value = await Replace(); return original.Value * 100 + Cache.Current.Value; } finally { Mark(8); }"),
            ("static-write", "public static class Cache { public static int Scalar; static Cache() { Script.Mark(1); } public static async Task<int> Assign() { try { Scalar = await Script.Read(7); return Scalar; } finally { Script.Mark(8); } } }", "int value = await Cache.Assign(); return value;"),
            ("before-await", broken, "try { int value = Broken.Value; int result = await Read(value); return result; } catch (TypeInitializationException) { return 41; } finally { Mark(8); }"),
            ("after-await", broken, "try { int value = await Read(7); return Broken.Value + value; } catch (TypeInitializationException) { return 42; } finally { Mark(8); }"),
            ("cached-failure", broken, "try { int value = Broken.Value; } catch (TypeInitializationException) { Mark(5); } int result = await Read(7); try { return Broken.Value; } catch (TypeInitializationException) { return result; } finally { Mark(8); }"),
            ("producer-init", "public static class Producer { static Producer() { Script.Mark(4); throw new InvalidOperationException(); } public static async Task<int> Read(int value) { Script.Mark(9); await AvidContinuations.NextTickAsync(); return value; } }", "try { int value = await Read(7); int result = await Producer.Read(Mark(1)); return result; } catch (TypeInitializationException) { return 43; } finally { Mark(8); }"),
            ("producer-reassign", "public static class Producer { static Producer() { Script.Mark(4); throw new InvalidOperationException(); } public static async Task<int> Read(int value) { Script.Mark(9); await AvidContinuations.NextTickAsync(); return value; } }", "Task<int> pending = Read(7); try { pending = Producer.Read(Mark(1)); } catch (TypeInitializationException) { Mark(5); } int value = await pending; return value;"),
            ("finally-local", cache, "try { int value = await Read(7); return value; } finally { int saved = Cache.Current.Value; Mark(saved); }"),
            ("finally-failure", broken, "try { try { int value = await Read(7); return value; } finally { int unused = Broken.Value; Mark(9); } } catch (TypeInitializationException) { return 44; }"),
            ("conditional-argument", cache, "int value = await Read(Mark(1) > 0 ? Mark(4) : Mark(9)); return value;"),
            ("conditional-nested", cache, "int value = await Read(7); return Mark(1) > 0 && (Mark(2) > 0 ? Mark(3) > 0 : Mark(9) > 0) ? Mark(4) : Mark(5);"),
            ("conditional-failure", broken, "int value = await Read(7); try { return Mark(1) > 0 ? Broken.Value : Mark(9); } catch (TypeInitializationException) { return 45; } finally { Mark(8); }"),
            ("conditional-skipped-failure", broken, "int value = await Read(7); try { return Mark(1) < 0 ? Broken.Value : Mark(9); } finally { Mark(8); }"),
            ("conditional-reference", cache, "Target first = Cache.Current; Target second = new Target(); int value = await Read(7); Target chosen = Mark(1) > 0 ? first : second; chosen.Value = await Read(9); return first.Value * 100 + second.Value;"),
            ("conditional-reference-false", cache, "Target first = Cache.Current; Target second = new Target(); int value = await Read(7); Target chosen = Mark(1) < 0 ? first : second; chosen.Value = await Read(9); return first.Value * 100 + second.Value;"),
            ("conditional-struct", "public struct Pair { public int Value; public Pair(int value) { Value = value; } }", "Pair first = new Pair(4); Pair second = new Pair(9); int value = await Read(7); Pair chosen = Mark(1) > 0 ? first : second; return chosen.Value;"),
            ("conditional-struct-false", "public struct Pair { public int Value; public Pair(int value) { Value = value; } }", "Pair first = new Pair(4); Pair second = new Pair(9); int value = await Read(7); Pair chosen = Mark(1) < 0 ? first : second; return chosen.Value;"),
        }) Compile(scenario.Item1 + (deferred ? "-deferred" : "-ready"), Source(scenario.Item2, scenario.Item3, deferred));
        Compile("member-cancel", Source(cache, "try { Cache.Current.Value = await Read(7); return 0; } catch (OperationCanceledException) { return Cache.Current.Value; } finally { Mark(8); }", true), true);
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;

        void Compile(string name, string source, bool cancel = false)
        {
            var reference = CSharpGuestAsyncThrowRoutingTests.Reference(source, cancel);
            string sourceId = "Scripts/StaticAsync_" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
                enableStaticInitialization: true, enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true,
                enableAsyncCancellationFlow: true, enableAsyncSynchronousExceptions: true);
            byte[] sourceBytes = SemanticSerializer.Serialize(semantic);
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), sourceBytes);
            }
            Check(semantic.SchemaVersion == 51 && semantic.SemanticVersion == "1.60"
                && semantic.StaticInitialization is { BaseSchemaVersion: 50, BaseSemanticVersion: "1.59" }
                && SemanticStaticInitializationValidator.IsValid(semantic), name + " source envelope: "
                    + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
            string hash = Convert.ToHexString(SHA256.HashData(sourceBytes)).ToLowerInvariant();
            Check(!CSharpGuestLowerer.Lower(semantic, hash).Succeeded, name + " ordinary entry must reject envelope");
            Check(CSharpStaticInitializationCompiler.TryLower(semantic, hash, out var module, out string? error)
                && module is not null, name + " lowering: " + error);
            bool readiness = semantic.AsyncMethods.SelectMany(method => method.Segments)
                .Any(segment => segment.AwaitSite?.CancellationToken is not null);
            Check(module!.SchemaVersion == (readiness ? 31 : 30) && module.IrVersion == (readiness ? "1.30" : "1.29")
                && (!readiness || module.DirectAwaitReadiness is { BaseSchemaVersion: 30, BaseIrVersion: "1.29" })
                && module.StaticStorage is { BaseSchemaVersion: 29, BaseIrVersion: "1.28" }
                && module.Provenance.SemanticSchemaVersion == 51 && module.Provenance.SemanticVersion == "1.60"
                && module.Provenance.SemanticSha256 == hash && module.AsyncSynchronousExceptions is not null
                && GuestModuleValidator.Validate(module).Succeeded, name + " execution envelope");
            count += CSharpGuestCancellationIdentityTests.CheckUpgrade(module, out _);
            byte[] json = GuestIrSerializer.Serialize(module);
            Check(CSharpStaticInitializationCompiler.TryLower(SemanticSerializer.Deserialize(sourceBytes), hash, out var again, out error)
                && json.SequenceEqual(GuestIrSerializer.Serialize(again!)), name + " deterministic source compilation: " + error);
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            var roundTrip = GuestIrSerializer.Deserialize(json);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(roundTrip))
                && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(roundTrip).Bytes), name + " canonical execution artifact");
            Check(Encoding.UTF8.GetString(wasm.Bytes).Contains(readiness ? "guest_ir_base=30/1.29" : "guest_ir_base=29/1.28",
                StringComparison.Ordinal), name + " WASM provenance");
            if (name == "member-deferred") Mutations(semantic, module);
            if (name == "finally-local-deferred")
            {
                var method = semantic.AsyncMethods.Single(item => item.CompilerLocals.Any(local =>
                    local.SymbolId.StartsWith(SemanticAsyncCleanupLocals.Prefix(item.MethodSymbolId), StringComparison.Ordinal)));
                var local = method.CompilerLocals.First(item => item.SymbolId.StartsWith(SemanticAsyncCleanupLocals.Prefix(method.MethodSymbolId), StringComparison.Ordinal));
                foreach (var changed in new[] { local with { Name = "forged" }, local with { TypeId = "type:bool" },
                    local with { Span = method.Span } })
                {
                    var invalid = semantic with { AsyncMethods = semantic.AsyncMethods.Select(item => item != method ? item : item with {
                        CompilerLocals = item.CompilerLocals.Select(value => value == local ? changed : value).ToArray() }).ToArray() };
                    Check(!CSharpStaticInitializationCompiler.TryLower(invalid, hash, out var rejected, out _) && rejected is null,
                        "Copied cleanup local must retain source identity, type and span");
                }
            }
            if (string.IsNullOrWhiteSpace(directory)) return;
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            int Offset(string fieldName)
            {
                var owner = semantic.StaticInitialization!.Types.Single(type => type.Fields.Any(field =>
                    semantic.Symbols.Any(symbol => symbol.Id == field.FieldSymbolId && symbol.Name == fieldName)));
                var field = owner.Fields.Single(field => semantic.Symbols.Any(symbol => symbol.Id == field.FieldSymbolId && symbol.Name == fieldName));
                string suffix = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(owner.TypeId + "\n" + field.FieldSymbolId))).ToLowerInvariant();
                return module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId == "global:symbol:field:$static:" + suffix).Offset;
            }
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel, expected = reference.Result, trace = reference.Trace,
                resultOffset = Offset("Result"), traceOffset = Offset("Trace"), staticSlots = module.StaticStorage!.Slots.Count });
        }
        void Mutations(SemanticDocument semantic, GuestModule module)
        {
            foreach (var invalid in new[] {
                semantic with { SchemaVersion = 49, SemanticVersion = "1.58" },
                semantic with { StaticInitialization = null },
                semantic with { StaticInitialization = semantic.StaticInitialization! with { BaseSchemaVersion = 47, BaseSemanticVersion = "1.56" } },
            }) Check(!CSharpStaticInitializationCompiler.TryLower(invalid, new string('a', 64), out var rejected, out _) && rejected is null,
                "malformed source composition must fail closed");
            foreach (var invalid in new[] {
                module with { SchemaVersion = 29, IrVersion = "1.28" },
                module with { SchemaVersion = 27, IrVersion = "1.26" },
                module with { StaticStorage = null },
                module with { AsyncSynchronousExceptions = null },
                module with { StaticStorage = module.StaticStorage! with { BaseSchemaVersion = 26, BaseIrVersion = "1.25" } },
                module with { Provenance = module.Provenance with { SemanticSchemaVersion = 50, SemanticVersion = "1.59" } },
            }) Check(!GuestModuleValidator.Validate(invalid).Succeeded && !WasmModuleCompiler.Compile(invalid).Succeeded,
                "malformed execution composition must fail closed");
        }
    }

    private static string Source(string declarations, string body, bool deferred) => """
        using AvidScript; using System; using System.Runtime.InteropServices; using System.Threading.Tasks;
        public sealed class Target {
            private int stored = 11;
            public int Value { get { return stored; } set { Script.Mark(6); stored = value; } }
        }
        """ + declarations + """
        public static class Script {
            public static int Result; public static int Trace;
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
            public static async void BeginPlay() { Result = await Run(); }
            public static int Mark(int value) { Trace = Trace * 10 + value; return value; }
            public static async Task<int> Read(int value) { Mark(2);
        """ + (deferred ? "await AvidContinuations.NextTickAsync(); Mark(3); " : "")
            + "return value; } "
            + (body.Contains("Replace()", StringComparison.Ordinal)
                ? "public static async Task<int> Replace() { Mark(2); Cache.Current = new Target(); "
                    + (deferred ? "await AvidContinuations.NextTickAsync(); Mark(3); " : "") + "return 7; } " : "")
            + "public static async Task<int> Run() { " + body + " } }";
}
