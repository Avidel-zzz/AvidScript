using System;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestStaticSourceFailureTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool value, string message) { if (!value) throw new InvalidOperationException("Static source failure: " + message); count++; }
        const string attempts = "public static class Attempts { public static int Count; }";
        const string broken = """
            public class Broken {
                public static int Value = Create(); static Broken() {}
                private static int Create() { Attempts.Count++; throw new InvalidOperationException(); }
            }
            """;
        Verify("field", Source(attempts + broken,
            "try { return Broken.Value; } catch (InvalidOperationException) { return -1; } catch (TypeInitializationException) { return Attempts.Count * 10 + 1; }"), 11, 11);
        Verify("cctor", Source(attempts + """
            public class Broken { public static int Value = 7; static Broken() { Attempts.Count++; throw new ArgumentException(); } }
            """, "try { return Broken.Value; } catch (SystemException) { return Attempts.Count * 10 + 2; }"), 12, 12);
        Verify("nested", Source(attempts + broken + """
            public class Outer { public static int Value = Broken.Value; static Outer() {} }
            """, "try { return Outer.Value; } catch (Exception) { return Attempts.Count * 10 + 3; }"), 13, 13);
        Verify("identity", Source(attempts + broken + "public static class Observed { public static Exception First; }",
            "try { return Broken.Value; } catch (Exception error) { if (Observed.First == null) Observed.First = error; return error == Observed.First ? Attempts.Count * 10 + 4 : -1; }"), 14, 14);
        Verify("generic", Source(attempts + """
            public class Cache<T> {
                public static int Value = Create(); static Cache() {}
                private static int Create() { Attempts.Count++; throw new InvalidOperationException(); }
            }
            public static class Helper {
                public static void ReadInt() { try { int value = Cache<int>.Value; } catch (Exception) {} }
                public static void ReadLong() { try { int value = Cache<long>.Value; } catch (Exception) {} }
            }
            """, "Helper.ReadInt(); Helper.ReadLong(); return Attempts.Count * 10;"), 20, 20);
        Verify("finally", Source(attempts + broken + """
            public static class Cleanup { public static int Count; }
            public static class Helper { public static int Read() { try { return Broken.Value; } finally { Cleanup.Count++; } } }
            """, "try { return Helper.Read(); } catch (Exception) { return Cleanup.Count * 10 + Attempts.Count; }"), 11, 21);
        const string cleanupFailure = "public static class Cleanup { public static void Fail() { Attempts.Count += 10; throw new ArgumentException(); } }";
        Verify("finally-call", Source(attempts + broken + cleanupFailure + """
            public static class Helper { public static int Read() {
                try { return Broken.Value; } finally { Cleanup.Fail(); Attempts.Count += 100; }
            } }
            """, "try { return Helper.Read(); } catch (TypeInitializationException) { return -1; } catch (ArgumentException) { return Attempts.Count; }"), 11, 21);
        Verify("finally-normal", Source(attempts + cleanupFailure + """
            public static class Helper { public static int Read() {
                try { return 7; } finally { Cleanup.Fail(); Attempts.Count += 100; }
            } }
            """, "try { return Helper.Read(); } catch (ArgumentException) { return Attempts.Count; }"), 10, 20);
        Verify("nested-finally-call", Source(attempts + broken + cleanupFailure + """
            public static class Tracking { public static int Count; }
            public static class Helper { public static int Read() {
                try { try { return Broken.Value; } finally { Cleanup.Fail(); Tracking.Count += 1000; } }
                finally { Tracking.Count++; }
            } }
            """, "try { return Helper.Read(); } catch (ArgumentException) { return Attempts.Count * 100 + Tracking.Count; }"), 1101, 2102);
        Verify("finally-static", Source(attempts + broken + """
            public class CleanupState {
                public static int Value = Create(); static CleanupState() {}
                private static int Create() { Attempts.Count += 10; throw new ArgumentException(); }
            }
            public static class Helper { public static int Read() {
                try { return Broken.Value; } finally { int value = CleanupState.Value; Attempts.Count += 100; }
            } }
            """, """
            try { return Helper.Read(); } catch (Exception error) {
                try { int value = CleanupState.Value; }
                catch (Exception cleanup) { return error == cleanup ? Attempts.Count * 10 + 1 : -1; }
                return -2;
            }
            """), 111, 111);
        Verify("rethrow", Source(attempts + broken + """
            public static class Helper { public static int Read() { try { return Broken.Value; } catch (Exception) { throw; } } }
            """, "try { return Helper.Read(); } catch (TypeInitializationException) { return Attempts.Count * 10 + 5; }"), 15, 15);
        Verify("caught-inside", Source(attempts + """
            public class Cache {
                public static int Value = Create(); static Cache() {}
                private static int Create() { Attempts.Count++; try { throw new InvalidOperationException(); } catch (Exception) { return 7; } }
            }
            """, "return Cache.Value * 10 + Attempts.Count;"), 71, 71);
        Verify("published-alias", Source(attempts + """
            public class Node { public int Value = 7; }
            public static class Escaped { public static Node Value; }
            public class Broken {
                public static Node Original = new Node();
                static Broken() { Escaped.Value = Original; Attempts.Count++; throw new InvalidOperationException(); }
            }
            """, "try { Node value = Broken.Original; return -1; } catch (Exception) { return Escaped.Value.Value * 10 + Attempts.Count; }"), 71, 71);
        return count;

        void Verify(string name, string source, int first, int second)
        {
            string id = "Scripts/StaticFailure-" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, id, FrontendAnalyzer.Analyze(source, id).Source.Sha256,
                Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(), enableStaticInitialization: true);
            Check(SemanticStaticInitializationValidator.IsValid(semantic), name + " static source plan");
            byte[] sourceBytes = SemanticSerializer.Serialize(semantic);
            string hash = Convert.ToHexString(SHA256.HashData(sourceBytes)).ToLowerInvariant();
            Check(CSharpStaticInitializationCompiler.TryLower(semantic, hash, out var module, out var error), name + ": " + error);
            if (name == "field")
            {
                var malformed = semantic with { ExceptionFlows = semantic.ExceptionFlows!.Select((flow, index) =>
                    index == 0 ? flow with { Blocks = null } : flow).ToArray() };
                Check(!CSharpStaticInitializationCompiler.TryLower(malformed, hash, out _, out var invalidError)
                    && invalidError is not null, "missing exception blocks fail closed without throwing");
            }
            Check(GuestModuleValidator.Validate(module!).Succeeded && module!.Provenance.SemanticSchemaVersion == 49
                && module.Provenance.SemanticSha256 == hash, name + " original source and validated output");
            Check(module!.LanguageErrorCatalog!.Types.Any(type => type.TypeId == CSharpStaticInitializationGuards.ExceptionType), name + " generated initialization error token");
            Check(CSharpStaticInitializationCompiler.TryLower(SemanticSerializer.Deserialize(sourceBytes), hash, out var again, out error)
                && GuestIrSerializer.Serialize(module).SequenceEqual(GuestIrSerializer.Serialize(again!)), name + " deterministic source round trip: " + error);
            var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
                .Select(path => MetadataReference.CreateFromFile(path));
            var compilation = CSharpCompilation.Create("StaticFailure_" + name, new[] { CSharpSyntaxTree.ParseText(source) }, references,
                new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Release));
            using var assemblyBytes = new MemoryStream();
            var emitted = compilation.Emit(assemblyBytes);
            Check(emitted.Success, name + " .NET compile: " + string.Join(" | ", emitted.Diagnostics));
            assemblyBytes.Position = 0;
            var context = new AssemblyLoadContext("static-failure-" + name, isCollectible: true);
            try
            {
                var run = context.LoadFromStream(assemblyBytes).GetType("Script")!.GetMethod("Run")!;
                Check((int)run.Invoke(null, null)! == first, name + " .NET first access");
                Check((int)run.Invoke(null, null)! == second, name + " .NET cached failure");
            }
            finally { context.Unload(); }
            var stressed = module with { Functions = module.Functions.Select(function => function with
            {
                Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction =>
                    instruction.Op is "managed_new" or "managed_static_set"
                        ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) }
                        : new[] { instruction }).ToArray() }).ToArray(),
            }).ToArray() };
            Check(GuestModuleValidator.Validate(stressed).Succeeded, name + " GC pressure");
            foreach (bool cooperative in new[] { false, true })
            {
                var wasm = WasmModuleCompiler.Compile(stressed, new(cooperative, 4));
                Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
                string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_STATIC_FAILURE_WASM_DIR");
                if (string.IsNullOrWhiteSpace(directory)) continue;
                Directory.CreateDirectory(directory);
                string filename = "static-failure-" + name + (cooperative ? "-cooperative" : "");
                File.WriteAllBytes(Path.Combine(directory, filename + ".wasm"), wasm.Bytes);
                File.WriteAllBytes(Path.Combine(directory, filename + ".guest-ir.json"), GuestIrSerializer.Serialize(stressed));
                File.WriteAllText(Path.Combine(directory, "static-failure-" + name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, "static-failure-" + name + ".semantic.json"), sourceBytes);
            }
        }
    }

    private static string Source(string declarations, string run) => "using System; using System.Runtime.InteropServices;\n" + declarations + """
        public static class Script {
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")] public static void Begin() {}
            [UnmanagedCallersOnly(EntryPoint = "run")] public static int ExportRun() { return Run(); }
            public static int Run() {
        """ + run + "\n} }";
}
