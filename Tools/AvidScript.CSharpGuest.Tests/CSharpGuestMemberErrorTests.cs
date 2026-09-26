using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestMemberErrorTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MEMBER_ERROR_FIXTURE_DIR");
        List<object> fixtures = new();
        foreach (var scenario in new[]
        {
            ("setter", "public int Value { set { Calls++; Script.Record(4); if (Fail) throw new InvalidOperationException(); Stored = value; } }", "",
                "target.Value = Right(); return target.Stored * 10 + target.Calls;", 71, 24, 111, 245),
            ("getter", "public int Value { get { Calls++; Script.Record(3); if (Fail) throw new InvalidOperationException(); return Stored; } }", "",
                "return target.Value * 10 + target.Calls;", 111, 3, 111, 35),
            ("method", "public int Read(int amount) { Calls++; Script.Record(3); if (Fail) throw new ArgumentException(); return Stored + amount; }", "",
                "return target.Read(Right()) * 10 + target.Calls;", 181, 23, 111, 236),
            ("receiver", "", "static Target Choose(Target target) { Record(1); if (target.Fail) throw new ArgumentException(); return target; }",
                "Choose(target).Stored = Right(); return target.Stored * 10 + target.Calls;", 70, 12, 110, 16),
            ("instance-receiver", "public Target Choose() { Script.Record(1); if (Fail) throw new ArgumentException(); return this; }", "",
                "target.Choose().Stored = Right(); return target.Stored * 10 + target.Calls;", 70, 12, 110, 16),
            ("always-throw", "public void Raise() { Calls++; Script.Record(3); throw new ArgumentException(); }", "",
                "target.Raise(); return 0;", 111, 36, 111, 36),
            ("prefix-failure", "void Guard() { if (Fail) throw new InvalidOperationException(); } public void Raise() { Calls++; Script.Record(3); Guard(); Script.Record(4); throw new ArgumentException(); }", "",
                "target.Raise(); return 0;", 111, 346, 111, 35),
            ("static-prefix", "", "static void Raise() { Record(3); throw new ArgumentException(); }",
                "Raise(); return 0;", 110, 36, 110, 36),
            ("fresh-receiver", "", "static Target Choose(Target target) { Record(1); if (target.Fail) throw new ArgumentException(); Target selected = new Target(); selected.Stored = 21; return selected; }",
                "Target selected = Choose(target); Target pressure = new Target(); pressure.Stored = 5; return selected.Stored * 10 + pressure.Stored;", 215, 1, 110, 16),
            ("null-receiver-result", "", "static Target Choose(Target target) { Record(1); if (target.Fail) throw new ArgumentException(); return null; }",
                "Target selected = Choose(target); return selected == null ? 21 : 99;", 21, 1, 110, 16),
        })
        {
            Compile(scenario.Item1, Source(scenario.Item2, scenario.Item3, scenario.Item4),
                scenario.Item5, scenario.Item6, scenario.Item7, scenario.Item8);
        }
        Compile("caller-finally", CleanupSource, 7, 248, -1, 248);
        foreach (string member in new[] {
            "public void Raise() { throw new ArgumentException(\"message\"); }",
            "public void Raise() { throw new ArgumentNullException(); }",
            "public Exception Error; public void Raise() { throw Error; }",
        })
        {
            var unsupported = Analyze(Source(member, "", "target.Raise(); return 0;"), "Scripts/UnsupportedMemberError.cs");
            Check(!CSharpLanguageErrorCompiler.TryLower(unsupported, new string('a', 64), out var rejected, out _)
                && rejected is null, "unsupported exception construction must not publish a module");
        }
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "manifest.json"), JsonSerializer.Serialize(fixtures));
        return count;

        void Compile(string name, string source, int normal, int normalTrace, int failed, int failedTrace)
        {
            string sourceId = "Scripts/MemberError_" + name + ".cs";
            Check(Reference(source, 0) == (normal, normalTrace), name + " .NET normal result/order");
            Check(Reference(source, 1) == (failed, failedTrace), name + " .NET error result/order");
            var semantic = Analyze(source, sourceId);
            Check(SemanticExceptionFlowContractValidator.IsValid(semantic), name + " semantic: " + Diagnostics(semantic));
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(CSharpLanguageErrorCompiler.TryLower(semantic, hash, out var lowered, out var error), name + " lowering: " + error);
            GuestModule module = lowered!.Module;
            Check(GuestModuleValidator.Validate(module).Succeeded && module.LanguageOutcomeTypes is not null,
                name + " independent outcome reader");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            byte[] json = GuestIrSerializer.Serialize(module);
            var restored = GuestIrSerializer.Deserialize(json);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(restored))
                && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(restored).Bytes), name + " deterministic round trip");
            if (name.Contains("receiver", StringComparison.Ordinal))
                Check(module.LanguageOutcomeTypes!.Any(item => item.ValueTypeId == "type:global::Target"),
                    name + " preserves object return type inside the outcome");
            if (string.IsNullOrWhiteSpace(directory)) return;
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
            File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            fixtures.Add(new { name, cases = new[] {
                new { input = 0, result = normal, trace = normalTrace },
                new { input = 1, result = failed, trace = failedTrace } } });
        }
    }

    private const string CleanupSource = """
        using System;
        using System.Runtime.InteropServices;
        public sealed class Target {
            public int Stored;
            public bool Fail;
            public int Value { set { Script.Trace = Script.Trace * 10 + 4;
                if (Fail) throw new InvalidOperationException(); Stored = value; } }
        }
        public static class Script {
            public static int Trace;
            static int Write(int fail) {
                Trace = 2;
                Target target = new Target(); target.Fail = fail != 0;
                target.Value = 7; return target.Stored;
            }
            public static int Run(int fail) {
                try { return Write(fail); }
                catch (Exception) { return -1; }
                finally { Trace = Trace * 10 + 8; }
            }
            [UnmanagedCallersOnly(EntryPoint = "run")]
            public static int Entry(int fail) => Run(fail);
            [UnmanagedCallersOnly(EntryPoint = "trace")]
            public static int ReadTrace() => Trace;
        }
        """;

    private static string Source(string member, string helper, string body) => """
        using System;
        using System.Runtime.InteropServices;
        public sealed class Target {
            public int Stored;
            public int Calls;
            public bool Fail;
        """ + member + """
        }
        public static class Script {
            public static int Trace;
            public static void Record(int digit) { Trace = Trace * 10 + digit; }
            public static int Right() { Record(2); return 7; }
        """ + helper + """
            public static int Run(int fail) {
                Trace = 0;
                Target target = new Target();
                target.Stored = 11;
                target.Fail = fail != 0;
                try {
        """ + body + """
                } catch (InvalidOperationException) {
                    Record(5); return target.Stored * 10 + target.Calls;
                } catch (ArgumentException) {
                    Record(6); return target.Stored * 10 + target.Calls;
                }
            }
            [UnmanagedCallersOnly(EntryPoint = "run")]
            public static int Entry(int fail) => Run(fail);
            [UnmanagedCallersOnly(EntryPoint = "trace")]
            public static int ReadTrace() => Trace;
        }
        """;

    private static SemanticDocument Analyze(string source, string id) =>
        SemanticAnalyzer.Analyze(source, id, FrontendAnalyzer.Analyze(source, id).Source.Sha256);

    private static string Diagnostics(SemanticDocument document) =>
        string.Join(" | ", document.Diagnostics.Select(item => item.Code + ": " + item.Message));

    private static (int Result, int Trace) Reference(string source, int input)
    {
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("MemberErrorOracle", new[] { CSharpSyntaxTree.ParseText(source) },
            references, new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Release));
        using MemoryStream bytes = new();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("member-error-oracle", isCollectible: true);
        try
        {
            Type script = context.LoadFromStream(bytes).GetType("Script")!;
            int result = (int)script.GetMethod("Run")!.Invoke(null, new object[] { input })!;
            return (result, (int)script.GetField("Trace")!.GetValue(null)!);
        }
        finally { context.Unload(); }
    }
}
