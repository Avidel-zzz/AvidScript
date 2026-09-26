using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestAsyncMemberAssignmentTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string reason) { if (!condition) throw new InvalidOperationException(reason); count++; }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR");
        List<object> fixtures = new();
        foreach (bool property in new[] { false, true })
        foreach (bool deferred in new[] { false, true })
        foreach (int mode in new[] { 0, 2, 3, 6, 7, 8 })
        {
            string name = (property ? "property" : "field") + (deferred ? "-deferred-" : "-ready-") + mode;
            string source = Source(property, deferred, mode);
            Compile(name, source);
        }
        foreach (bool deferred in new[] { false, true })
        foreach (var scenario in new[] {
            ("plain-field", "Target target = new Target(); try { target.Field = await Read(); return target.Field; } finally { Trace = Trace * 10 + 8; }"),
            ("plain-property", "Target target = new Target(); try { target.Value = await Read(); return target.Value; } finally { Trace = Trace * 10 + 8; }"),
            ("plain-null-field", "Target target = null; try { target.Field = await Read(); return 0; } catch (NullReferenceException) { return 6; } finally { Trace = Trace * 10 + 8; }"),
            ("plain-null-property", "Target target = null; try { target.Value = await Read(); return 0; } catch (NullReferenceException) { return 6; } finally { Trace = Trace * 10 + 8; }"),
            ("temporary", "try { new Target().Value = await Read(); return LastAssigned; } finally { Trace = Trace * 10 + 8; }"),
            ("task-alias", "Target target = new Target(); Task<int> pending = Read(); Task<int> alias = pending; try { target.Value = await alias; return target.Value; } finally { Trace = Trace * 10 + 8; }"),
            ("loop", "Target target = new Target(); try { for (int i = 0; i < 2; i++) { target.Value = await Read(); } return target.Value; } finally { Trace = Trace * 10 + 8; }"),
            ("implicit", "Target target = new Target(); int value = await target.Assign(); return value;"),
            ("concurrent", "Target first = new Target(); Target second = new Target(); first.Offset = 1; second.Offset = 2; Task<int> a = first.Assign(); Task<int> b = second.Assign(); int x = await a; int y = await b; return first.Value * 100 + second.Value;"),
            ("null-producer", "Target target = null; try { int value = await target.WithArgument(Record()); return value; } catch (SystemException) { return 6; } finally { Trace = Trace * 10 + 8; }"),
            ("null-producer-reassign", "Target target = null; Task<int> pending = Read(); try { pending = target.WithArgument(Record()); } catch (SystemException) { Trace = Trace * 10 + 6; } Target output = new Target(); output.Value = await pending; return output.Value;"),
        }) Compile(scenario.Item1 + (deferred ? "-deferred" : "-ready"), PureSource(scenario.Item2, deferred));
        Compile("temporary-cancel", PureSource("try { new Target().Value = await Read(); return 0; } catch (OperationCanceledException) { return LastAssigned; } finally { Trace = Trace * 10 + 8; }", true), cancel: true);
        CheckOriginalFixture();
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;

        void Compile(string name, string source, bool cancel = false)
        {
            var reference = CSharpGuestAsyncThrowRoutingTests.Reference(source, cancel);
            var semantic = Analyze(source, "Scripts/AwaitMember_" + name + ".cs");
            Check(semantic.SchemaVersion == 50 && SemanticAsyncInvocationValidator.IsValid(semantic),
                name + " semantic: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
            Check(semantic.AsyncMethods.SelectMany(method => method.Segments).Any(segment => segment.AwaitSite?.MemberAssignment is not null),
                name + " must retain the member assignment plan");
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
            }
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(CSharpLanguageErrorCompiler.TryLower(semantic, hash, out var compiled, out string? error) && compiled is not null,
                name + " lowering: " + error);
            var module = compiled!.Module;
            Check(module.SchemaVersion == 29 && module.Provenance.SemanticSchemaVersion == 50 && GuestModuleValidator.Validate(module).Succeeded,
                name + " exact validated execution contract");
            Check(module.Functions.Any(function => function.Id.StartsWith("function:$async:member_receiver:", StringComparison.Ordinal)),
                name + " implicit null outcome guard");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            byte[] json = GuestIrSerializer.Serialize(module);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), name + " canonical IR");
            if (name == "property-deferred-0") CheckMutations(semantic);
            if (string.IsNullOrWhiteSpace(directory)) return;
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel, expected = reference.Result, trace = reference.Trace,
                resultOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Result:", StringComparison.Ordinal)).Offset,
                traceOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Trace:", StringComparison.Ordinal)).Offset });
        }
        void CheckMutations(SemanticDocument valid)
        {
            var method = valid.AsyncMethods.Single(method => method.Segments.Any(segment => segment.AwaitSite?.MemberAssignment is not null));
            var capture = method.Segments.Single(segment => segment.AwaitSite?.MemberAssignment is not null);
            var site = capture.AwaitSite!;
            var plan = site.MemberAssignment!;
            void Reject(SemanticAsyncMethod changed, string reason)
            {
                var invalid = valid with { AsyncMethods = valid.AsyncMethods.Select(item => item == method ? changed : item).ToArray() };
                Check(!CSharpLanguageErrorCompiler.TryLower(invalid, new string('a', 64), out var result, out _) && result is null, reason);
            }
            void Change(SemanticAsyncSegment changed, string reason) => Reject(method with {
                Segments = method.Segments.Select(segment => segment.Ordinal == changed.Ordinal ? changed : segment).ToArray() }, reason);
            Change(capture with { Statements = Array.Empty<SemanticAsyncStatement>() }, "Missing receiver evaluation must not publish IR");
            Change(capture with { Statements = capture.Statements.Concat(capture.Statements).ToArray() }, "Repeated receiver evaluation must not publish IR");
            Change(capture with { AwaitSite = site with { ResultSymbolId = plan.ReceiverSymbolId } }, "Aliased receiver/result must not publish IR");
            Change(capture with { Transfer = capture.Transfer! with { SecondaryTarget = plan.WriteSegmentOrdinal } }, "Failure writeback must not publish IR");
            Change(capture with { AwaitSite = site with { StateFrame = site.StateFrame! with {
                Slots = site.StateFrame.Slots.Where(slot => slot.SymbolId != plan.ReceiverSymbolId).ToArray() } } }, "Missing suspended receiver root must not publish IR");
            Reject(method with { CompilerLocals = method.CompilerLocals.Where(local => local.SymbolId != plan.ReceiverSymbolId).ToArray() },
                "Missing captured receiver storage must not publish IR");
            var write = method.Segments[plan.WriteSegmentOrdinal];
            Change(write with { Statements = write.Statements.Concat(write.Statements).ToArray() }, "Repeated setter must not publish IR");
        }
        void CheckOriginalFixture()
        {
            const string fixture = "Fixtures/Phase66/AwaitMemberAssignment.cs";
            DirectoryInfo? root = new(AppContext.BaseDirectory);
            while (root is not null && !File.Exists(Path.Combine(root.FullName, fixture))) root = root.Parent;
            Check(root is not null, "Original C10 fixture must remain available");
            byte[] bytes = File.ReadAllBytes(Path.Combine(root!.FullName, fixture));
            Check(Convert.ToHexString(SHA256.HashData(bytes)).Equals("265a7e71e2c744681d099ee6b11a93fcf94b151980205cddf155c46f602249d9", StringComparison.OrdinalIgnoreCase),
                "Original C10 source must remain byte-for-byte unchanged");
            // This is deliberately the original source, including static object initialization.
            var source = Analyze(System.Text.Encoding.UTF8.GetString(bytes), fixture);
            Check(source.AsyncMethods.SelectMany(method => method.Segments).Count(segment => segment.AwaitSite?.MemberAssignment is not null) == 9,
                "All nine original writeback sites must remain in the plan");
            bool accepted = CSharpLanguageErrorCompiler.TryLower(source, new string('b', 64), out var result, out string? error);
            Check(!accepted && result is null && !string.IsNullOrWhiteSpace(error), "Original C10 must not bypass the missing static initialization composition");
            if (!string.IsNullOrWhiteSpace(directory)) File.WriteAllText(Path.Combine(directory, "original-c10-pending.txt"), error);
        }
    }

    private static SemanticDocument Analyze(string source, string sourceId) => SemanticAnalyzer.Analyze(source, sourceId,
        FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
        new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
            "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
        enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
        enableAsyncSynchronousExceptions: true);

    private static string PureSource(string body, bool deferred) => """
        using AvidScript; using System; using System.Runtime.InteropServices; using System.Threading.Tasks;
        public sealed class Target {
            public int Field = 11; private int stored = 11; public int Offset;
            public int Value { get { return stored; } set {
                Script.Trace = Script.Trace * 10 + 4; stored = value; Script.LastAssigned = value;
            } }
            public async Task<int> Assign() {
                try { Value = await Script.ReadFor(Offset); return Value; } finally { Script.Trace = Script.Trace * 10 + 8; }
            }
            public async Task<int> WithArgument(int value) {
                Script.Trace = Script.Trace * 10 + 5; Field = await Script.Read(); return value + Field;
            }
        }
        public static class Script {
            public static int Result; public static int Trace; public static int LastAssigned;
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
            public static async void BeginPlay() { Result = await Run(); }
            public static int Record() { Trace = Trace * 10 + 1; return 9; }
            public static async Task<int> Read() { Trace = Trace * 10 + 2;
        """ + (deferred ? "await AvidContinuations.NextTickAsync(); Trace = Trace * 10 + 3; " : "")
            + "return 7; } public static async Task<int> ReadFor(int offset) { Trace = Trace * 10 + 2; "
            + (deferred ? "await AvidContinuations.NextTickAsync(); Trace = Trace * 10 + 3; " : "")
            + "return 7 + offset; } public static async Task<int> Run() { " + body + " } }";

    private static string Source(bool property, bool deferred, int mode) => """
        using AvidScript; using System; using System.Runtime.InteropServices; using System.Threading.Tasks;
        public sealed class Target {
            public int Field = 11; private int stored = 11; public bool ThrowOnSet; public int SetterCalls;
            public int Value { get { return stored; } set {
                SetterCalls++; Script.Trace = Script.Trace * 10 + 4;
                if (ThrowOnSet) throw new InvalidOperationException(); stored = value;
            } }
        }
        public sealed class Box { public Target Original; public Target Replacement; public Target Current; }
        public static class Script {
            public static int Result; public static int Trace;
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
            public static async void BeginPlay() { Result = await Run(); }
            public static Target Select(Box box, int mode) {
                Trace = Trace * 10 + 1; if (mode == 7) throw new ArgumentException();
                if (mode == 6) return null; return box.Current;
            }
            public static async Task<int> Produce(Box box, int mode) {
                Trace = Trace * 10 + 2; box.Current = box.Replacement;
                if (mode == 3) throw new ArgumentException();
        """ + (deferred ? "await AvidContinuations.NextTickAsync(); Trace = Trace * 10 + 3; " : "") + """
                if (mode == 2) throw new ArgumentException(); return 7;
            }
            public static async Task<int> Assign(Box box, int mode) {
                try {
        """ + "Select(box, mode)." + (property ? "Value" : "Field") + " = await Produce(box, mode); return 7; " + """
                } finally { Trace = Trace * 10 + 8; }
            }
            public static async Task<int> Run() {
                Box box = new Box(); box.Original = new Target(); box.Replacement = new Target(); box.Current = box.Original;
        """ + (mode == 8 ? "box.Original.ThrowOnSet = true; " : "") + """
                int status = 0;
                try {
        """ + "status = await Assign(box, " + mode + "); " + """
                } catch (NullReferenceException) { status = 6; }
                  catch (ArgumentException) { status = 2; }
                  catch (InvalidOperationException) { status = 8; }
                return status * 100000 + box.Original.Field * 1000 + box.Original.Value * 100
                    + box.Replacement.Field * 10 + box.Original.SetterCalls;
            }
        }
        """;
}
