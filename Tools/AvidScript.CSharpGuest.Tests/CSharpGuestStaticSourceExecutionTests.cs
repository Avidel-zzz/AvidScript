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

internal static class CSharpGuestStaticSourceExecutionTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool value, string message) { if (!value) throw new InvalidOperationException("Static source execution: " + message); count++; }
        const string trace = "public static class Trace { public static int Count; public static int Mark(int value) { Count = Count * 10 + value; return value; } }";
        Verify("objects", """
            using System.Runtime.InteropServices;
            public class Node { public int Value; public Node(int value) { Value = value; } }
            public static class Trace { public static int Count; public static int Mark(int value) { Count = Count * 10 + value; return value; } }
            public class Cache {
                public static Node First = new Node(Trace.Mark(1));
                public static Node Alias = First;
                public static int Last = Trace.Mark(2);
                static Cache() { Trace.Mark(3); }
                public static int Read() { return First.Value + Last; }
            }
            public static class Script {
                public static int Run() { Cache.Read(); return Trace.Count; }
                public static int Replace() { Cache.First = new Node(9); return Cache.Alias.Value * 10 + Cache.First.Value; }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")] public static void Begin() {}
                [UnmanagedCallersOnly(EntryPoint = "run")] public static int ExportRun() { return Run(); }
                [UnmanagedCallersOnly(EntryPoint = "replace")] public static int ExportReplace() { return Replace(); }
            }
            """, 123, 123, objects: true);
        Verify("cycle", Source("""
            public class A { public static int Value = B.Value + 1; static A() {} }
            public class B { public static int Value = A.Value + 2; static B() {} }
            """, "return A.Value * 10 + B.Value;"), 32, 32);
        Verify("reentry", Source("""
            public class Cache {
                public static int First = ReadLater(); public static int Later = 7;
                static Cache() {} public static int ReadLater() { return Later; }
            }
            """, "return Cache.First * 10 + Cache.Later;"), 7, 7);
        Verify("generic", Source("""
            public static class Trace { public static int Count; public static int Next() { Count += 1; return Count; } }
            public class Cache<T> { public static int Value = Trace.Next(); static Cache() {} }
            """, "return Cache<int>.Value * 10 + Cache<long>.Value;"), 12, 12);
        Verify("construct", Source(trace + """
            public class Cache {
                public static int Shared = Trace.Mark(1); public int Value = Trace.Mark(2);
                static Cache() { Trace.Mark(3); } public Cache() { Trace.Mark(4); }
            }
            """, "Cache value = new Cache(); return Trace.Count;"), 1324, 132424);
        Verify("store-order", Source(trace + """
            public class Cache { public static int Value; static Cache() { Trace.Mark(2); } }
            """, "Cache.Value = Trace.Mark(1); return Trace.Count;"), 12, 121);
        Verify("call-order", Source(trace + """
            public class Cache { static Cache() { Trace.Mark(2); } public static void Touch(int argument) { Trace.Mark(3); } }
            """, "Cache.Touch(Trace.Mark(1)); return Trace.Count;"), 123, 12313);
        Verify("defaults", Source("public class Cache<T> { public static int Value; }",
            "Cache<int>.Value += 1; Cache<long>.Value += 2; return Cache<int>.Value * 10 + Cache<long>.Value;"), 12, 24);
        Verify("ref-order", Source(trace + """
            public class Box { public int Value; }
            public class Cache {
                public static int Value = 5; public static Box Root = new Box();
                static Cache() { Trace.Mark(2); }
            }
            public static class Helper { public static void Touch(ref int value, int amount) { value += amount; Trace.Mark(3); } }
            """, "Helper.Touch(ref Cache.Value, Trace.Mark(1)); return Trace.Count * 100 + Cache.Value;"), 21306, 2131307);
        Verify("struct-call", Source(trace + """
            public struct Cache { static Cache() { Trace.Mark(2); } public int Read() { return 3; } }
            """, "Cache value = default(Cache); int read = value.Read(); return read + Trace.Count * 10;"), 23, 23);
        return count;

        void Verify(string name, string source, int first, int second, bool objects = false)
        {
            string id = "Scripts/StaticSource-" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, id, FrontendAnalyzer.Analyze(source, id).Source.Sha256,
                Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(), enableStaticInitialization: true);
            Check(SemanticStaticInitializationValidator.IsValid(semantic), name + " source plan");
            byte[] bytes = SemanticSerializer.Serialize(semantic);
            string hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
            Check(CSharpStaticInitializationCompiler.TryLower(semantic, hash, out var module, out var error), name + ": " + error);
            Check(GuestModuleValidator.Validate(module!).Succeeded, name + " published module");
            Check(module!.Provenance.SemanticSchemaVersion == 49 && module.Provenance.SemanticVersion == "1.58"
                && module.Provenance.SemanticSha256 == hash, name + " original provenance");
            Check(module.SchemaVersion == 27 && module.StaticStorage is { BaseSchemaVersion: 17 }, name + " storage with error outcomes");
            Check(module.Globals.All(global => module.Types.Single(type => type.Id == global.TypeId).Kind != "managed_ref"), name + " no reference globals");
            if (objects) Check(module.StaticStorage!.Slots.Count == 4, "two object slots and two type states");
            Check(CSharpStaticInitializationCompiler.TryLower(SemanticSerializer.Deserialize(bytes), hash, out var again, out error)
                && GuestIrSerializer.Serialize(module).SequenceEqual(GuestIrSerializer.Serialize(again!)), name + " deterministic compile: " + error);
            foreach (var invalid in new[] {
                semantic with { StaticInitialization = null }, semantic with { SchemaVersion = 48, SemanticVersion = "1.57" }, semantic with { Succeeded = false },
                semantic with { Callables = semantic.Callables.Append(semantic.Callables[0]).ToArray() },
                semantic with { Methods = semantic.Methods.Append(semantic.Methods[0]).ToArray() },
                semantic with { ControlFlowGraphs = semantic.ControlFlowGraphs.Append(semantic.ControlFlowGraphs[0]).ToArray() },
                semantic with { UeTypeDeclarations = null! },
            })
                Check(!CSharpStaticInitializationCompiler.TryLower(invalid, hash, out var rejected, out _) && rejected is null, name + " rejects invalid source contract");

            var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
                .Select(path => MetadataReference.CreateFromFile(path));
            var compilation = CSharpCompilation.Create("StaticSource_" + name.Replace('-', '_'),
                new[] { CSharpSyntaxTree.ParseText(source) }, references, new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
            using var assemblyBytes = new MemoryStream();
            var emitted = compilation.Emit(assemblyBytes);
            Check(emitted.Success, name + " .NET oracle compilation: " + string.Join(" | ", emitted.Diagnostics));
            assemblyBytes.Position = 0;
            var context = new AssemblyLoadContext("static-source-" + name, isCollectible: true);
            try
            {
                var script = context.LoadFromStream(assemblyBytes).GetType("Script")!;
                int Call(string method) => (int)script.GetMethod(method)!.Invoke(null, null)!;
                Check(Call("Run") == first, name + " .NET first call");
                Check(Call("Run") == second, name + " .NET second call");
                if (objects)
                {
                    Check(Call("Replace") == 19, name + " .NET replacement retains alias");
                    Check(Call("Run") == 123, name + " .NET replacement does not initialize again");
                }
            }
            finally { context.Unload(); }

            // Force collection while references are live across compiler-inserted
            // initializer calls, and immediately after changing a static root.
            var stressed = module with { Functions = module.Functions.Select(function => function with
            {
                Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction =>
                    instruction.Op is "managed_new" or "managed_static_set"
                        ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) }
                        : new[] { instruction }).ToArray() }).ToArray(),
            }).ToArray() };
            Check(GuestModuleValidator.Validate(stressed).Succeeded, name + " GC stress module");
            foreach (bool cooperative in new[] { false, true })
            {
                var wasm = WasmModuleCompiler.Compile(stressed, new(cooperative, 4));
                Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
                string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_STATIC_SOURCE_WASM_DIR");
                if (!string.IsNullOrEmpty(directory))
                {
                    Directory.CreateDirectory(directory);
                    string filename = "static-source-" + name + (cooperative ? "-cooperative" : "");
                    File.WriteAllBytes(Path.Combine(directory, filename + ".wasm"), wasm.Bytes);
                    File.WriteAllBytes(Path.Combine(directory, filename + ".guest-ir.json"), GuestIrSerializer.Serialize(stressed));
                    File.WriteAllText(Path.Combine(directory, "static-source-" + name + ".cs"), source);
                }
            }
        }
    }

    private static string Source(string declarations, string run) => "using System.Runtime.InteropServices;\n" + declarations + """
        public static class Script {
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")] public static void Begin() {}
            [UnmanagedCallersOnly(EntryPoint = "run")] public static int ExportRun() { return Run(); }
            public static int Run() {
        """ + run + "\n} }";
}
