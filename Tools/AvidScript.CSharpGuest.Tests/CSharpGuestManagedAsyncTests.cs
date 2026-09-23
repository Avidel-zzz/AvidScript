using System;
using System.IO;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestManagedAsyncTests
{
    public static int Run()
    {
        int count = 0;
        foreach (var (name, body) in new[]
        {
            ("aliases", """
                Counter counter = new Counter(5);
                Counter alias = counter;
                alias.Next = alias;
                await AvidContinuations.DelayAsync(0.01f);
                Replace(ref counter);
                alias.Value += 2;
                await AvidContinuations.NextTickAsync();
                Result = counter.Value * 100 + alias.Value;
                if (alias.Next == alias) Result += 10000;
                """),
            ("loop", """
                Counter counter = new Counter(0);
                for (int index = 0; index < 3; ++index)
                {
                    await AvidContinuations.DelayAsync(0.01f);
                    counter.Value += index + 1;
                }
                Result = counter.Value;
                """),
            ("delegate", """
                Counter counter = new Counter(5);
                Func<int> increment = Make(counter);
                await AvidContinuations.DelayAsync(0.01f);
                int observed = increment();
                counter.Value += 2;
                await AvidContinuations.NextTickAsync();
                Result = increment() + observed;
                """),
            ("aggregate", """
                Counter counter = new Counter(5);
                Snapshot snapshot = default;
                snapshot.Reference = counter;
                snapshot.Marker = 7;
                await AvidContinuations.DelayAsync(0.01f);
                counter.Value += 2;
                snapshot.Marker += 3;
                await AvidContinuations.NextTickAsync();
                Result = snapshot.Reference.Value + snapshot.Marker;
                """),
            ("null", """
                Counter counter = null;
                await AvidContinuations.DelayAsync(0.01f);
                if (counter == null) Result = 9;
                else Result = 0;
                """),
            ("owned-shared", """
                int value = 5;
                Func<int> increment = () => ++value;
                Func<int> read = () => value;
                await AvidContinuations.DelayAsync(0.01f);
                value += 10;
                increment();
                await AvidContinuations.NextTickAsync();
                Result = read() * 100 + value;
                """),
            ("owned-direct", """
                int value = 1;
                void Add() { value += 3; }
                Func<int> read = () => value;
                await AvidContinuations.DelayAsync(0.01f);
                Add();
                await AvidContinuations.NextTickAsync();
                value += 2;
                Result = read();
                """),
            ("owned-loop", """
                Func<int> first = null;
                Func<int> second = null;
                for (int i = 0; i < 3; ++i) {
                    int copy = i;
                    Func<int> read = () => i * 10 + copy;
                    if (i == 0) first = read;
                    if (i == 1) second = read;
                    await AvidContinuations.DelayAsync(0.01f);
                    copy += 100;
                }
                await AvidContinuations.NextTickAsync();
                Result = first() * 1000 + second();
                """),
            ("owned-ref", """
                Counter counter = new Counter(2);
                Func<int> read = () => counter.Value;
                await AvidContinuations.DelayAsync(0.01f);
                Replace(ref counter);
                await AvidContinuations.NextTickAsync();
                Result = read();
                """),
            ("owned-scope", """
                Func<int> read = null;
                await AvidContinuations.DelayAsync(0.01f);
                {
                    int value = 7;
                    read = () => ++value;
                    await AvidContinuations.NextTickAsync();
                    value += 3;
                }
                await AvidContinuations.NextTickAsync();
                Result = read();
                """),
            ("owned-no-live", """
                int value = 1;
                Func<int> read = () => value;
                await AvidContinuations.DelayAsync(0.01f);
                value = 7;
                Result = value;
                """),
            ("owned-cycle", """
                int value = 1;
                Func<int> recurse = null;
                recurse = () => { if (value < 3) { value++; return recurse(); } return value; };
                await AvidContinuations.DelayAsync(0.01f);
                Result = recurse();
                """),
            ("owned-before-declaration", """
                await AvidContinuations.DelayAsync(0.01f);
                int value = 7;
                Func<int> increment = () => ++value;
                await AvidContinuations.NextTickAsync();
                Result = increment();
                """),
            ("owned-result", """
                AvidLoadedObject loaded = await AvidAssets.LoadObjectAsync("/Engine/EngineMeshes/Cube.Cube").WithCancellation(Cancellation.Token);
                Func<int> read = () => { if (loaded.Slot > 0 && loaded.Generation > 0) return 1; return 0; };
                await AvidContinuations.NextTickAsync();
                Result = read();
                """),
        })
        {
            string source = Source.Replace("// BODY", body, StringComparison.Ordinal)
                .Replace("DelayAsync(0.01f)", "DelayAsync(0.01f).WithCancellation(Cancellation.Token)", StringComparison.Ordinal)
                .Replace("NextTickAsync()", "NextTickAsync().WithCancellation(Cancellation.Token)", StringComparison.Ordinal);
            SemanticDocument document = CSharpGuestContinuationTests.Analyze(source, $"Scripts/ManagedAsync_{name}.cs");
            CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
            Check(lowered.Succeeded, name + ": " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
            SemanticDocument legacyDocument = CSharpGuestSemanticFixture.WithoutNamedGenericShapes(document);
            Check(CSharpGuestLowerer.Lower(legacyDocument with { SchemaVersion = 27, SemanticVersion = "1.31" }, new string('d', 64)).Succeeded,
                "Semantic 27/1.31 managed async and owned closures remain executable");
            if (!name.StartsWith("owned-", StringComparison.Ordinal)) Check(CSharpGuestLowerer.Lower(legacyDocument with { SchemaVersion = 26, SemanticVersion = "1.30" }, new string('d', 64)).Succeeded,
                "Semantic 26/1.30 managed async state remains executable");
            GuestModule module = lowered.Module!;
            Check(module.Functions.SelectMany(fn => fn.Blocks).SelectMany(block => block.Instructions)
                .Any(op => op.Op == GuestContinuationState.StoreOp), "C# suspension must use typed state store");
            Check(module.Functions.SelectMany(fn => fn.Blocks).SelectMany(block => block.Instructions)
                .Any(op => op.Op == GuestContinuationState.ReadOp), "C# restoration must use typed state read");
            GuestStateSlot resultSlot = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Result:", StringComparison.Ordinal));
            Check(resultSlot.TypeId == "type:int32" && resultSlot.Size == 4, "native fixture result type");
            WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
            Check(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
            Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes), "async state deterministic round trip");
            Check(!CSharpGuestLowerer.Lower(document, new string('d', 64), enableDebugInstrumentation: true).Succeeded,
                "managed debug pause frame remains rejected until its ownership is connected");
            string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
            if (!string.IsNullOrWhiteSpace(output))
            {
                Directory.CreateDirectory(output);
                File.WriteAllBytes(Path.Combine(output, $"csharp-managed-async-{name}.wasm"), compiled.Bytes);
                File.WriteAllBytes(Path.Combine(output, $"csharp-managed-async-{name}.guest-ir.json"), GuestIrSerializer.Serialize(module));
                File.WriteAllText(Path.Combine(output, $"csharp-managed-async-{name}.result-offset"), resultSlot.Offset.ToString(CultureInfo.InvariantCulture));
            }
            ++count;
        }
        string localCaptureSource = Source.Replace("// BODY", """
            int value = 5;
            Func<int> increment = () => ++value;
            await AvidContinuations.NextTickAsync();
            Result = increment();
            """, StringComparison.Ordinal);
        SemanticDocument localCapture = CSharpGuestContinuationTests.Analyze(localCaptureSource, "Scripts/AsyncOwnedCaptureBoundary.cs");
        CSharpGuestLoweringResult captured = CSharpGuestLowerer.Lower(localCapture, new string('d', 64));
        Check(captured.Succeeded && WasmModuleCompiler.Compile(captured.Module!).Succeeded,
            "async-owned closure environments execute through validated scope and rooted state plans");
        SemanticDocument malformedScope = localCapture with { AsyncMethods = localCapture.AsyncMethods.Select(method =>
            method with { LexicalScopes = Array.Empty<SemanticAsyncLexicalScope>() }).ToArray() };
        CSharpGuestLoweringResult malformedRejected = CSharpGuestLowerer.Lower(malformedScope, new string('d', 64));
        Check(!malformedRejected.Succeeded && malformedRejected.Diagnostics.Any(item => item.Code == "ASCG1001"),
            "missing async closure scope must fail artifact validation before code generation");
        CSharpGuestLoweringResult legacyCapture = CSharpGuestLowerer.Lower(
            CSharpGuestSemanticFixture.WithoutNamedGenericShapes(malformedScope) with
            { SchemaVersion = 26, SemanticVersion = "1.30" }, new string('d', 64));
        Check(!legacyCapture.Succeeded && legacyCapture.Diagnostics.Any(item => item.Code == "ASCG1024"),
            "legacy async capture analysis stays readable but must not execute without ownership");
        SemanticDocument invocation = CSharpGuestContinuationTests.Analyze("""
            using AvidScript;
            public class Worker {
                public int Value;
                public async void Run(int amount) {
                    await AvidContinuations.NextTickAsync();
                    Value += amount;
                }
            }
            public static class Script {
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { new Worker().Run(3); }
            }
            """, "Scripts/AsyncInvocationBoundary.cs");
        CSharpGuestLoweringResult pendingInvocation = CSharpGuestLowerer.Lower(invocation, new string('d', 64));
        Check(pendingInvocation.Succeeded && WasmModuleCompiler.Compile(pendingInvocation.Module!).Succeeded,
            "valid invocation contract compiles a persistent receiver and parameter restore path");
        SemanticAsyncMethod invocationMethod = invocation.AsyncMethods.Single();
        CSharpGuestLoweringResult missingInputs = CSharpGuestLowerer.Lower(invocation with { AsyncMethods = new[] {
            invocationMethod with { InvocationInputs = Array.Empty<SemanticAsyncStateSlot>() } } }, new string('d', 64));
        Check(!missingInputs.Succeeded && missingInputs.Diagnostics.Any(item => item.Code == "ASCG1001"), "missing receiver state is an invalid artifact");
        CSharpGuestLoweringResult missingFrame = CSharpGuestLowerer.Lower(invocation with { AsyncMethods = new[] {
            invocationMethod with { Segments = invocationMethod.Segments.Select(segment => segment.AwaitSite is null ? segment : segment with {
                AwaitSite = segment.AwaitSite with { StateFrame = null } }).ToArray() } } }, new string('d', 64));
        Check(!missingFrame.Succeeded && missingFrame.Diagnostics.Any(item => item.Code == "ASCG1001"), "missing persistent invocation frame is an invalid artifact");
        CSharpGuestLoweringResult downgraded = CSharpGuestLowerer.Lower(invocation with { SchemaVersion = 27, SemanticVersion = "1.31" }, new string('d', 64));
        Check(!downgraded.Succeeded && downgraded.Diagnostics.Any(item => item.Code == "ASCG1001"), "old contract cannot authorize callable async state");
        return count + 7;
    }

    private const string Source = """
        using System;
        using System.Runtime.InteropServices;
        using AvidScript;
        namespace Game;
        public class Counter
        {
            public int Value;
            public Counter Next;
            public Counter(int value) { Value = value; }
        }
        public static class Script
        {
            public static int Result;
            [AvidTransient] private static AvidCancellationSource Cancellation;
            private static void Replace(ref Counter counter) { counter = new Counter(20); }
            private static Func<int> Make(Counter counter)
            {
                return () => { counter.Value += 3; return counter.Value; };
            }
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
            public static async void BeginPlay()
            {
                Cancellation = AvidCancellationSource.Create();
                // BODY
            }
            [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
            public static void Tick(float dt) { }
            [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
            public static void EndPlay()
            {
                AvidCancellationSource cancellation = Cancellation;
                cancellation.Cancel();
                cancellation.Release();
            }
        }
        public struct Snapshot
        {
            public Counter Reference;
            public int Marker;
        }
        """;
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
