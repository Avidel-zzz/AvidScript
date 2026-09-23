using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncInvocationTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); ++count; }
        const string source = """
            using System; using AvidScript;
            public class Worker {
                public int Value;
                public async void Run(int amount, Worker peer) {
                    Func<int> read = () => Value + amount + peer.Value;
                    await AvidContinuations.NextTickAsync();
                    amount += 3;
                    Value = read();
                    await AvidContinuations.NextTickAsync();
                    Value += amount;
                }
                public async void NoClosure(int amount) {
                    await AvidContinuations.NextTickAsync();
                    Value += amount;
                }
                private static async void Helper(int amount) {
                    await AvidContinuations.NextTickAsync();
                    amount++;
                }
                public async void NoAwait(int amount) { Value = amount; }
            }
            public static class Script {
                [AvidExport("avid_on_begin_play")]
                public static void Begin() { new Worker().Run(2, new Worker()); }
            }
            """;
        SemanticDocument document = Analyze(source);
        Check(document.Succeeded, string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        Check(document.SchemaVersion == 29 && document.SemanticVersion == "1.33", "async callable invocation is versioned");
        Check(SemanticClosureContractValidator.IsValid(document), "complete invocation, lexical scope and closure contracts validate");
        Check(document.AsyncMethods.Count == 4 && document.AsyncMethods.All(method => method.ExportName is null
            && method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering), "member calls have no invented WASM export and use resumable CFG");
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            SemanticCallable callable = document.Callables.Single(item => item.MethodSymbolId == method.MethodSymbolId);
            Check(method.InvocationInputs.Count == callable.Parameters.Count + (callable.IsStatic ? 0 : 1), "all value parameters and only instance receivers are inputs");
            foreach (SemanticAsyncStateSlot input in method.InvocationInputs)
                Check(method.Segments.Where(segment => segment.AwaitSite is not null).All(segment =>
                    segment.AwaitSite!.StateFrame!.Slots.Contains(input)), "every suspension retains the invocation inputs including captured parameters");
        }
        SemanticAsyncMethod run = document.AsyncMethods.Single(method => method.MethodSymbolId.Contains(".Run(", StringComparison.Ordinal));
        Check(document.Reachability!.ReachableCallableIds.Contains(run.MethodSymbolId), "ordinary call reachability includes async member bodies");
        Check(document.ClosureEnvironments.Single().Cells.Any(cell => cell.Kind == "receiver"), "instance capture keeps the original receiver identity");
        Check(SemanticSerializer.Serialize(document).SequenceEqual(SemanticSerializer.Serialize(Analyze(source))), "deterministic invocation projection");
        Check(SemanticAsyncInvocationValidator.IsValid(SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document))), "round trip retains invocation metadata");

        var badMethods = new List<SemanticAsyncMethod> {
            run with { InvocationInputs = Array.Empty<SemanticAsyncStateSlot>() },
            run with { InvocationInputs = run.InvocationInputs.Concat(new[] {run.InvocationInputs[0]}).ToArray() },
            run with { InvocationInputs = run.InvocationInputs.Reverse().ToArray() },
            run with { InvocationInputs = run.InvocationInputs.Select((slot, index) => index == 0 ? slot with { TypeId = "type:int32" } : slot).ToArray() },
            run with { InvocationInputs = run.InvocationInputs.Select((slot, index) => index == 0 ? slot with { SymbolId = "receiver:foreign" } : slot).ToArray() },
            run with { ExportName = "forged_export" },
            run with { Lowering = SemanticAsyncMethod.ReentrantZeroHeapCpsLowering },
            run with { InvocationInputs = null! },
            run with { Segments = run.Segments.Select(segment => segment.AwaitSite is null ? segment : segment with {
                AwaitSite = segment.AwaitSite with { StateFrame = null } }).ToArray() },
        };
        foreach (SemanticAsyncMethod bad in badMethods)
            Check(!SemanticClosureContractValidator.IsValid(document with { AsyncMethods = document.AsyncMethods.Select(method => method == run ? bad : method).ToArray() }), "tampered invocation metadata fails closed");
        Check(!SemanticAsyncInvocationValidator.IsValid(document with { SchemaVersion = 27, SemanticVersion = "1.31" }), "old version cannot carry new invocation inputs");
        Check(!SemanticAsyncInvocationValidator.IsValid(document with { SchemaVersion = SemanticContract.CurrentSchemaVersion + 1, SemanticVersion = "1.33" }), "unknown future contract is rejected");
        SemanticCallable runCallable = document.Callables.Single(item => item.MethodSymbolId == run.MethodSymbolId);
        Check(!SemanticAsyncInvocationValidator.IsValid(document with { Callables = document.Callables.Select(callable => callable == runCallable ? callable with {
            Parameters = callable.Parameters.Select(parameter => parameter with { RefKind = "ref" }).ToArray() } : callable).ToArray() }), "borrowed inputs cannot cross a suspension");
        SemanticDocument oversized = Analyze("public class Worker { public async void Run(" +
            string.Join(", ", Enumerable.Range(0, 64).Select(index => "int value" + index)) + ") { } }");
        Check(!oversized.Succeeded && oversized.Diagnostics.Any(item => item.Code == "ASCS5414"), "receiver counts toward the input limit even without awaits");
        return count;
    }

    private static SemanticDocument Analyze(string source) => SemanticAsyncTests.Analyze(source, "Scripts/AsyncInvocation.cs");
}
