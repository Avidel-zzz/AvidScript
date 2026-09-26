using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestInstanceInitializerTests
{
    public static int Run()
    {
        int count = 0;
        var fixtures = new List<object>();
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_INSTANCE_INITIALIZER_DIR");
        Compile("defaults", """
            public sealed class Target {
                public int Field = 11, Other = Script.Record(2);
                public readonly long Wide = 12345678901L;
                public double Fraction = 1.5;
                public bool Enabled = true;
                public int Empty;
            }
            """, "Target target = new Target(); return target.Field + target.Other + (target.Wide == 12345678901L && target.Fraction == 1.5 && target.Enabled && target.Empty == 0 ? 100 : 0);", 113, 2);
        Compile("chain", """
            public sealed class Target {
                public int Value = Script.Record(1);
                public Target() : this(Script.Record(2)) { Script.Record(4); }
                public Target(int value) { Script.Record(3); Value += value; }
            }
            """, "Target target = new Target(); return target.Value;", 3, 2134);
        Compile("conditional", """
            public sealed class Target {
                public int Left = Script.Input == 0 ? Script.Record(1) : Script.Record(2);
                public int Right = Script.Input == 0 ? Script.Record(3) : Script.Record(4);
                public Target() { Script.Record(5); }
            }
            """, "Target target = new Target(); return target.Left * 10 + target.Right;", 13, 135, 24, 245);
        Compile("nested", """
            public sealed class Leaf { public int Value = Script.Record(2); }
            public sealed class Target {
                public int Before = Script.Record(1);
                public Leaf Child = new Leaf();
                public int After = Script.Record(3);
            }
            """, "Target target = new Target(); Leaf pressure = new Leaf(); return target.Child.Value + target.Before + target.After + pressure.Value;", 8, 1232);
        Compile("generic", """
            public sealed class Leaf { public int Value = Script.Record(2); }
            public sealed class Target<T> {
                public T Value = default(T);
                public int Count = Script.Record(1);
                public Leaf Child = new Leaf();
            }
            """, "Target<int> number = new Target<int>(); Target<Leaf> reference = new Target<Leaf>(); return number.Value + number.Count + number.Child.Value + (reference.Value == null ? 10 : 0);", 13, 1212);
        Compile("partial", """
            public sealed partial class Target { public int First = Script.Record(1); }
            public sealed partial class Target { public int Second = Script.Record(2); public Target() { Script.Record(3); } }
            """, "Target target = new Target(); return target.First + target.Second;", 3, 123);
        Compile("loop", """
            public sealed class Target {
                public int Value = Script.Record(1);
                public Target() { for (int i = 0; i < 3; i++) Value += i; Script.Record(2); }
            }
            """, "Target target = new Target(); Target other = new Target(); return target.Value + other.Value;", 8, 1212);
        Compile("short_circuit", """
            public sealed class Target {
                public bool Enabled = Script.Input == 0 && Script.Record(1) == 1;
                public int Value = Script.Record(2);
            }
            """, "Target target = new Target(); return target.Enabled ? target.Value : -target.Value;", 2, 12, -2, 2);
        Compile("overloads", """
            public sealed class Target {
                public int Value = Script.Record(1);
                public Target() { Value = Script.Record(2); }
                public Target(int value) { Value = value; Script.Record(3); }
            }
            """, "Target first = new Target(); Target second = new Target(Script.Record(4)); return first.Value + second.Value;", 6, 12413);

        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "manifest.json"), JsonSerializer.Serialize(fixtures));
        return count;

        void Check(bool success, string message)
        {
            if (!success) throw new InvalidOperationException("Instance initializers: " + message);
            count++;
        }

        void Compile(string name, string declaration, string body, int result, int trace,
            int? otherResult = null, int? otherTrace = null)
        {
            string source = "using System; using System.Runtime.InteropServices;\n" + declaration + """
                public static class Script {
                    public static int Trace;
                    public static int Input;
                    public static int Record(int digit) { Trace = Trace * 10 + digit; return digit; }
                    public static int Run(int input) { Trace = 0; Input = input;
                """ + body + """
                    }
                    [UnmanagedCallersOnly(EntryPoint = "run")]
                    public static int Entry(int input) => Run(input);
                    [UnmanagedCallersOnly(EntryPoint = "trace")]
                    public static int ReadTrace() => Trace;
                }
                """;
            Check(Reference(source, 0) == (result, trace), name + " .NET order/result 0");
            Check(Reference(source, 1) == (otherResult ?? result, otherTrace ?? trace), name + " .NET order/result 1");
            string sourceId = "Scripts/InstanceInitializer_" + name + ".cs";
            SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId,
                FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256);
            Check(semantic.Succeeded, name + " semantic: " + string.Join(" | ", semantic.Diagnostics));
            byte[] semanticBytes = SemanticSerializer.Serialize(semantic);
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), semanticBytes);
            }
            var lowered = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
            Check(lowered.Succeeded, name + " lowering: " + string.Join(" | ", lowered.Diagnostics));
            GuestModule module = lowered.Module!;
            Check(GuestModuleValidator.Validate(module).Succeeded, name + " IR reader");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics));
            var repeated = SemanticAnalyzer.Analyze(source, sourceId, semantic.Source.Sha256);
            Check(semanticBytes.SequenceEqual(SemanticSerializer.Serialize(repeated)), name + " deterministic semantic");
            Check(CSharpGuestLowerer.Lower(SemanticSerializer.Deserialize(semanticBytes), new string('a', 64)).Succeeded,
                name + " semantic serialization reader");
            var restored = GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module));
            Check(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(restored).Bytes), name + " IR round trip");
            if (name == "defaults")
            {
                string helperId = SemanticInstanceInitializerContract.MethodId("type:global::Target");
                Check(!CSharpGuestLowerer.Lower(semantic with { Methods = semantic.Methods.Where(item => item.MethodSymbolId != helperId).ToArray() }, new string('a', 64)).Succeeded,
                    "missing initializer source body is rejected");
                Check(!CSharpGuestLowerer.Lower(semantic with { ControlFlowGraphs = semantic.ControlFlowGraphs.Where(item => item.MethodSymbolId != helperId).ToArray() }, new string('a', 64)).Succeeded,
                    "missing initializer CFG is rejected");
                var ctorId = semantic.Callables.Single(item => item.IsConstructor && item.ContainingTypeId == "type:global::Target").MethodSymbolId;
                var damaged = semantic with { ControlFlowGraphs = semantic.ControlFlowGraphs.Select(graph => graph.MethodSymbolId != ctorId ? graph : graph with {
                    Blocks = graph.Blocks.Select(block => block with { Operations = Array.Empty<SemanticOperation>() }).ToArray() }).ToArray() };
                Check(!CSharpGuestLowerer.Lower(damaged, new string('a', 64)).Succeeded, "missing constructor initialization call is rejected");
                var missingStore = semantic with { ControlFlowGraphs = semantic.ControlFlowGraphs.Select(graph => graph.MethodSymbolId != helperId ? graph : graph with {
                    Blocks = graph.Blocks.Select(block => block with { Operations = Array.Empty<SemanticOperation>() }).ToArray() }).ToArray() };
                Check(!CSharpGuestLowerer.Lower(missingStore, new string('a', 64)).Succeeded, "missing field stores are rejected");
            }
            if (string.IsNullOrWhiteSpace(directory)) return;
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), GuestIrSerializer.Serialize(module));
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            fixtures.Add(new { name, cases = new[] { new { input = 0, result, trace },
                new { input = 1, result = otherResult ?? result, trace = otherTrace ?? trace } } });
        }
    }

    private static (int Result, int Trace) Reference(string source, int input)
    {
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("InitializerOracle", new[] { CSharpSyntaxTree.ParseText(source) },
            references, new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Release));
        using MemoryStream bytes = new();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("initializer-oracle", isCollectible: true);
        try
        {
            Type script = context.LoadFromStream(bytes).GetType("Script")!;
            return ((int)script.GetMethod("Run")!.Invoke(null, new object[] { input })!,
                (int)script.GetField("Trace")!.GetValue(null)!);
        }
        finally { context.Unload(); }
    }
}
