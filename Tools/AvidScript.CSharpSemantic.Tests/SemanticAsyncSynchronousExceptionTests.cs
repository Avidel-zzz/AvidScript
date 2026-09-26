using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncSynchronousExceptionTests
{
    public static int Run()
    {
        int passed = 0;
        void Check(bool condition, string reason)
        {
            if (!condition) throw new InvalidOperationException(reason);
            passed++;
        }
        SemanticDocument Compile(string body)
        {
            var result = Analyze(Source(body));
            Check(SemanticContract.HasAsyncSynchronousExceptions(result)
                && result.Diagnostics.All(item => item.Severity != "error" || item.Code is "ASCS3001" or "ASCS5422"),
                "Synchronous async source failed: " + body + " | " + string.Join(" | ", result.Diagnostics.Select(item => item.Code + ": " + item.Message)));
            Check(SemanticAsyncSynchronousExceptionValidator.IsValid(result), "Invalid synchronous routes: " + body);
            Check(SemanticAsyncInvocationValidator.IsValid(result), "Invalid async invocation: " + body);
            Check(SemanticAsyncScopeValidator.IsValid(result), "Invalid scopes: " + body);
            var bytes = SemanticSerializer.Serialize(result);
            Check(bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(Source(body))))
                && bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
                "New route serialization must be deterministic");
            return result;
        }
        foreach (string body in new[]
        {
            "int value = Value(mode); await AvidContinuations.NextTickAsync(); return value;",
            "await AvidContinuations.NextTickAsync(); return Value(mode);",
            "try { Value(mode); } catch (ArgumentException) { Trace++; } await AvidContinuations.NextTickAsync(); return 7;",
            "await AvidContinuations.NextTickAsync(); try { Value(mode); } finally { Trace++; } return 7;",
            "try { await Read(Value(mode)); return 7; } catch (ArgumentException) { return 8; }",
            "try { await Read(mode); return 7; } catch (ArgumentException) { return 8; }",
            "try { target.Property = await Read(mode); return 7; } finally { Value(mode); }",
            "try { Select(mode).Property = await Read(mode); return 7; } catch (ArgumentException) { return 8; } finally { Trace++; }",
            "try { ((Target)null!).Field = await Read(mode); return 7; } finally { Value(mode); }",
            "try { new Target().Property = await Read(mode); return 7; } finally { Trace++; }",
            "while (Value(mode) > 0) { await AvidContinuations.NextTickAsync(); mode = 0; } return 7;",
            "for (int i = Value(mode); i > 0; i = Value(0)) { await AvidContinuations.NextTickAsync(); } return 7;",
            "do { await AvidContinuations.NextTickAsync(); mode = 0; } while (Value(mode) > 0); return 7;",
            "Task<int> pending = Read(mode); Task<int> alias = pending; try { pending = Read(Value(mode)); } catch (ArgumentException) { Trace++; } int value = await pending; int other = await alias; return value + other;",
        }) Compile(body);

        const string nested = """
            try {
                try { Select(mode).Property = await Read(mode); return Value(mode); }
                catch (ArgumentException) { Value(mode); return 8; }
                finally { Value(mode); }
            } catch (InvalidOperationException) { return 9; }
            finally { Trace++; }
            """;
        var baseline = Compile(nested);
        var method = baseline.AsyncMethods.Single(item => item.MethodSymbolId.Contains(".Run("));
        Check(SemanticAsyncExceptionOwnerFlow.TryAnalyze(method, out var owners)
            && method.ExceptionPlan!.Regions.Where(region => region.Kind == "finally")
                .SelectMany(region => region.Segments).Distinct().Any(ordinal =>
                    owners[ordinal].HasFlag(SemanticAsyncExceptionOwnerState.Cancellation)
                    && method.Segments[ordinal].SynchronousExceptionTarget is not null),
            "Cleanup must be able to replace a propagating cancellation with a new error");
        var member = method.Segments.Single(segment => segment.AwaitSite?.MemberAssignment is not null);
        var write = method.Segments[member.AwaitSite!.MemberAssignment!.WriteSegmentOrdinal];
        Check(member.SynchronousExceptionTarget is not null && write.SynchronousExceptionTarget == member.SynchronousExceptionTarget
            && member.Transfer!.PrimaryTarget == write.Ordinal && member.Transfer.SecondaryTarget != write.Ordinal,
            "Receiver evaluation and success-only writeback need distinct failure sites with the same lexical handler");

        SemanticDocument Replace(SemanticAsyncSegment changed) => baseline with
        {
            AsyncMethods = baseline.AsyncMethods.Select(item => item != method ? item : item with
            {
                Segments = item.Segments.Select(segment => segment.Ordinal == changed.Ordinal ? changed : segment).ToArray(),
            }).ToArray(),
        };
        void Reject(SemanticDocument candidate, string reason) => Check(
            !SemanticAsyncInvocationValidator.IsValid(candidate), reason);
        foreach (var segment in method.Segments.Where(segment => segment.SynchronousExceptionTarget is not null))
        {
            Reject(Replace(segment with { SynchronousExceptionTarget = null }), "Missing evaluation route");
            foreach (int target in new[] { -1, method.Segments.Count, segment.Ordinal, write.Ordinal }.Distinct())
                if (target != segment.SynchronousExceptionTarget)
                    Reject(Replace(segment with { SynchronousExceptionTarget = target }), "Invalid or bypassed evaluation route");
            foreach (int target in method.ExceptionPlan!.ExceptionScopes!.SelectMany(scope => new[] { scope.DispatchTarget, scope.UnwindTarget }).Distinct())
                if (target != segment.SynchronousExceptionTarget)
                    Reject(Replace(segment with { SynchronousExceptionTarget = target }), "Wrong sibling handler or skipped finally");
        }
        var pure = method.Segments.First(segment => segment.SynchronousExceptionTarget is null);
        Reject(Replace(pure with { SynchronousExceptionTarget = member.SynchronousExceptionTarget }), "Synthetic dispatch cannot carry an evaluation failure");
        foreach (var (schema, version) in new[] { (44, "1.53"), (45, "1.54"), (46, "1.55"), (47, "1.56"), (49, "1.58"), (50, "1.58"), (51, "1.60") })
            Reject(baseline with { SchemaVersion = schema, SemanticVersion = version }, "Wrong route version pair");
        Reject(baseline with { AsyncMethods = baseline.AsyncMethods.Select(item => item with { ExceptionPlan = null }).ToArray() },
            "Methods require exception owner and region information");
        Reject(baseline with { AsyncMethods = baseline.AsyncMethods.Select(item => item != method ? item : item with
        {
            ExceptionPlan = item.ExceptionPlan! with
            {
                Regions = item.ExceptionPlan!.Regions.Select(region => region with
                {
                    Segments = region.Segments.Where(ordinal => ordinal != member.Ordinal).ToArray(),
                }).ToArray(),
            },
        }).ToArray() }, "Source evaluation cannot lose its bound region membership");
        Reject(baseline with { AsyncMethods = baseline.AsyncMethods.Select(item => item != method ? item : item with
        {
            ExceptionPlan = item.ExceptionPlan! with
            {
                ExceptionScopes = item.ExceptionPlan!.ExceptionScopes!.Append(item.ExceptionPlan.ExceptionScopes![0]).ToArray(),
            },
        }).ToArray() }, "Duplicate scope owners must be rejected without throwing");
        JsonNode missing = JsonNode.Parse(SemanticSerializer.Serialize(baseline))!;
        foreach (JsonNode? item in missing["async_methods"]!.AsArray())
            foreach (JsonNode? segment in item!["segments"]!.AsArray())
                segment!.AsObject().Remove("synchronous_exception_target");
        Reject(SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(missing.ToJsonString())), "Serialized omission cannot silently downgrade routes");
        var legacy = Analyze(Source("await AvidContinuations.NextTickAsync(); return Value(mode);"), enabled: false);
        Check(!Encoding.UTF8.GetString(SemanticSerializer.Serialize(legacy)).Contains("synchronous_exception_target", StringComparison.Ordinal),
            "Legacy output must omit the new field");

        var live = Compile("int saved = 17; await AvidContinuations.NextTickAsync(); try { saved = Value(mode); } catch (ArgumentException) { return saved; } return saved;");
        var liveMethod = live.AsyncMethods.Single(item => item.MethodSymbolId.Contains(".Run("));
        var saved = live.Symbols.Single(symbol => symbol.ContainingSymbolId == liveMethod.MethodSymbolId && symbol.Name == "saved");
        var suspension = liveMethod.Segments.Single(segment => segment.AwaitSite is not null);
        Check(suspension.AwaitSite!.StateFrame!.Slots.Any(slot => slot.SymbolId == saved.Id),
            "A failed assignment must retain the previous local value across the preceding suspension");
        Check(!SemanticAsyncInvocationValidator.IsValid(live with { AsyncMethods = live.AsyncMethods.Select(item => item != liveMethod ? item : item with
        {
            Segments = item.Segments.Select(segment => segment != suspension ? segment : segment with
            {
                AwaitSite = segment.AwaitSite! with { StateFrame = segment.AwaitSite.StateFrame! with
                {
                    Slots = segment.AwaitSite.StateFrame.Slots.Where(slot => slot.SymbolId != saved.Id).ToArray(),
                } },
            }).ToArray(),
        }).ToArray() }), "Reader must reject a frame that loses a handler-only old value");

        var graph = new[]
        {
            new SemanticAsyncTaskOwnerBlock(0, new[] { "old" }, new[] { 1 }),
            new SemanticAsyncTaskOwnerBlock(1, new[] { "new" }, new[] { 2 }) { SynchronousExceptionTarget = 3 },
            new SemanticAsyncTaskOwnerBlock(2, Array.Empty<string>(), Array.Empty<int>()),
            new SemanticAsyncTaskOwnerBlock(3, Array.Empty<string>(), Array.Empty<int>()),
        };
        var returning = Compile("Task<int> pending = Read(mode); int value = await pending; return Value(value);");
        var returningMethod = returning.AsyncMethods.Single(item => item.MethodSymbolId.Contains(".Run("));
        var returnSegment = returningMethod.Segments.Single(segment => segment.Transfer?.Kind == SemanticAsyncMethod.ReturnTransferKind);
        Check(returnSegment.SynchronousExceptionTarget is not null
            && SemanticAsyncTaskLocalLifetimeValidator.TryAnalyze(returningMethod, out var returnFlow)
            && returnFlow!.ReleasedOnExit[returnSegment.Ordinal].SequenceEqual(returningMethod.TaskLocalSymbolIds!),
            "A return expression with a failure edge must still retire Task locals on successful return");
        var releases = new Dictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>>
        {
            [new(1, 3)] = new[] { "new" },
        };
        Check(SemanticAsyncTaskOwnerFlowSolver.TrySolve(0, new[] { "old", "new" }, graph, releases, out var state)
            && state!.DefiniteAtEntry[2].SequenceEqual(new[] { "new", "old" })
            && state.DefiniteAtEntry[3].SequenceEqual(new[] { "old" })
            && state.PossibleAtEntry[3].SequenceEqual(new[] { "old" })
            && state.ReleasedOnSynchronousException[new(1, 3)].Count == 0,
            "Failure before Task write must neither acquire nor release the uncommitted new owner");
        var sharedTarget = graph.Select(block => block.Ordinal == 1 ? block with { SynchronousExceptionTarget = 2 } : block).ToArray();
        Check(SemanticAsyncTaskOwnerFlowSolver.TrySolve(0, new[] { "old", "new" }, sharedTarget,
                new Dictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>>(), out state)
            && state!.PossibleAtEntry[2].SequenceEqual(new[] { "new", "old" })
            && state.DefiniteAtEntry[2].SequenceEqual(new[] { "old" }),
            "Merged normal and exceptional destinations require different may/must states");

        // Explore concrete owner sets over cycles and merges. This oracle has
        // no may/must data-flow equations and can distinguish pre-write failure.
        string[] ownerIds = { "a", "b", "c" };
        int Mask(IEnumerable<string> ids) => ids.Aggregate(0, (bits, id) => bits | (1 << Array.IndexOf(ownerIds, id)));
        Random random = new(6650);
        for (int sample = 0; sample < 256; sample++)
        {
            int size = random.Next(2, 8);
            var nodes = Enumerable.Range(0, size).Select(id => new SemanticAsyncTaskOwnerBlock(id,
                ownerIds.Where(_ => random.Next(3) == 0).ToArray(),
                Enumerable.Range(0, size).Where(_ => random.Next(3) == 0).ToArray())
            {
                SynchronousExceptionTarget = random.Next(3) == 0 ? null : random.Next(size),
            }).ToArray();
            var kills = nodes.SelectMany(node => node.AllTargets.Select(target => new SemanticAsyncTaskOwnerEdge(node.Ordinal, target)))
                .ToDictionary(edge => edge, _ => (IReadOnlyList<string>)ownerIds.Where(_ => random.Next(3) == 0).ToArray());
            var paths = new HashSet<(int Block, int Owners)>();
            var pending = new Queue<(int Block, int Owners)>();
            pending.Enqueue((0, 0));
            while (pending.TryDequeue(out var path))
            {
                if (!paths.Add(path)) continue;
                var node = nodes[path.Block];
                int committed = path.Owners | Mask(node.GeneratedOwners);
                foreach (int target in node.Successors)
                    pending.Enqueue((target, committed & ~Mask(kills[new(path.Block, target)])));
                if (node.SynchronousExceptionTarget is int failure)
                    pending.Enqueue((failure, path.Owners & ~Mask(kills[new(path.Block, failure)])));
            }
            bool matches = SemanticAsyncTaskOwnerFlowSolver.TrySolve(0, ownerIds, nodes, kills, out var solved);
            foreach (var group in paths.GroupBy(path => path.Block))
            {
                int may = group.Aggregate(0, (bits, path) => bits | path.Owners);
                int must = group.Aggregate(7, (bits, path) => bits & path.Owners);
                int generated = Mask(nodes[group.Key].GeneratedOwners);
                matches &= solved is not null && Mask(solved.PossibleAtEntry[group.Key]) == may
                    && Mask(solved.DefiniteAtEntry[group.Key]) == must
                    && Mask(solved.PossibleAtExit[group.Key]) == (may | generated)
                    && Mask(solved.DefiniteAtExit[group.Key]) == (must | generated);
                foreach (int target in nodes[group.Key].Successors)
                {
                    var edge = new SemanticAsyncTaskOwnerEdge(group.Key, target);
                    matches &= solved is not null && Mask(solved.ReleasedOnEdge[edge]) == ((may | generated) & Mask(kills[edge]));
                }
                if (nodes[group.Key].SynchronousExceptionTarget is int failure)
                {
                    var edge = new SemanticAsyncTaskOwnerEdge(group.Key, failure);
                    matches &= solved is not null && Mask(solved.ReleasedOnSynchronousException[edge]) == (may & Mask(kills[edge]));
                }
            }
            matches &= solved?.PossibleAtEntry.Count == paths.Select(path => path.Block).Distinct().Count();
            Check(matches, "Concrete ownership disagrees with synchronous failure flow at graph " + sample);
        }
        Check(!SemanticAsyncTaskOwnerFlowSolver.TrySolve(0, new[] { "old", "new" },
                graph.Select(block => block.Ordinal == 1 ? block with { SynchronousExceptionTarget = -1 } : block).ToArray(), releases, out _),
            "Malformed exceptional successor must be rejected");

        foreach (bool staticInitialization in new[] { false, true })
        {
            bool rejected = false;
            try
            {
                const string empty = "class Empty { }";
                SemanticAnalyzer.Analyze(empty, "Empty.cs", FrontendAnalyzer.Analyze(empty, "Empty.cs").Source.Sha256,
                    Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(),
                    enableAsyncExceptionFlow: true, enableAsyncCancellationFlow: staticInitialization,
                    enableStaticInitialization: staticInitialization, enableAsyncSynchronousExceptions: true);
            }
            catch (ArgumentException) { rejected = true; }
            Check(rejected, "Missing cancellation contract or legacy static envelope must be rejected");
        }

        DirectoryInfo? root = new(AppContext.BaseDirectory);
        while (root is not null && !File.Exists(Path.Combine(root.FullName, "AvidScript.uplugin"))) root = root.Parent;
        string fixturePath = Path.Combine(root!.FullName, "Fixtures/Phase66/AwaitMemberAssignment.cs");
        byte[] fixture = File.ReadAllBytes(fixturePath);
        Check(Convert.ToHexString(SHA256.HashData(fixture)).Equals("265a7e71e2c744681d099ee6b11a93fcf94b151980205cddf155c46f602249d9", StringComparison.OrdinalIgnoreCase),
            "The original C10 source must remain unchanged");
        var original = Analyze(Encoding.UTF8.GetString(fixture), sourceId: "Fixtures/Phase66/AwaitMemberAssignment.cs");
        Check(original.AsyncMethods.SelectMany(item => item.Segments).Count(segment => segment.AwaitSite?.MemberAssignment is not null) == 9
            && SemanticAsyncInvocationValidator.IsValid(original) && SemanticAsyncScopeValidator.IsValid(original),
            "The original 29-case source must retain all nine member await sites and validated failure routes: "
                + string.Join(" | ", original.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        Check(SemanticExceptionFlowContractValidator.IsValid(original)
            && SemanticLanguageErrorEffectPlanner.TryBuild(original, out var originalEffects)
            && originalEffects is { AsyncBoundaryMethodIds.Count: > 0 }
            && !originalEffects.OutcomeMethodIds.Intersect(original.AsyncMethods.Select(item => item.MethodSymbolId)).Any(),
            "Original C10 synchronous effects must stop at Task method boundaries");
        CheckEffectBoundaries(Check);
        return passed;
    }

    private static void CheckEffectBoundaries(Action<bool, string> check)
    {
        var document = Analyze("""
            using System; using System.Threading.Tasks;
            public static class Script {
                public static int Fail() { throw new ArgumentException(); }
                public static int Left(int n) { return n == 0 ? Fail() : Right(n - 1); }
                public static int Right(int n) { return Left(n); }
                public static int Value { get { return 1; } set { if (value < 0) Fail(); } }
                public static async Task<int> Run(int n) { Value = n; return Left(n); }
                public static async Task<int> Outer(int n) { int value = await Run(n); return value; }
                public static Task<int> Factory(int n) { return Run(n); }
                public static Task<int> ThrowingFactory(int n) { if (n < 0) Fail(); return Run(n); }
                public static int Good() { return 42; }
            }
            """);
        check(SemanticContract.HasAsyncSynchronousExceptions(document)
            && SemanticExceptionFlowContractValidator.IsValid(document), "Combined synchronous and async contracts must both validate: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        check(SemanticLanguageErrorEffectPlanner.TryBuild(document, out var plan) && plan is not null,
            "Mixed source must produce an effect plan");
        string Id(string name) => document.Callables.Single(callable =>
            callable.MethodSymbolId.Contains("." + name + "(", StringComparison.Ordinal)).MethodSymbolId;
        check(plan!.OutcomeMethodIds.ToHashSet(StringComparer.Ordinal).SetEquals(new[]
            { Id("Fail"), Id("Left"), Id("Right"), Id("set_Value"), Id("ThrowingFactory") }),
            "Recursion, setters and independently throwing factories need outcomes, while ordinary Task factories do not: "
                + string.Join(" | ", plan.OutcomeMethodIds) + " async=" + string.Join(" | ", document.AsyncMethods.Select(method => method.MethodSymbolId))
                + " diagnostics=" + string.Join(" | ", document.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        check(plan.AsyncBoundaryMethodIds.SequenceEqual(new[] { Id("Run") }),
            "Only the Task method calling throwing synchronous code is an outcome boundary");
        var shuffled = document with { Methods = document.Methods.Reverse().ToArray(),
            Callables = document.Callables.Reverse().ToArray(), ExceptionFlows = document.ExceptionFlows!.Reverse().ToArray() };
        check(SemanticLanguageErrorEffectPlanner.TryBuild(shuffled, out var reordered)
            && reordered!.OutcomeMethodIds.SequenceEqual(plan.OutcomeMethodIds)
            && reordered.AsyncBoundaryMethodIds.SequenceEqual(plan.AsyncBoundaryMethodIds),
            "Effect ordering must be independent of source collection enumeration");
        foreach (var broken in new[]
        {
            document with { Succeeded = true },
            document with { SchemaVersion = 47, SemanticVersion = "1.56" },
            document with { SemanticVersion = "1.58" },
            document with { AsyncMethods = document.AsyncMethods.Select(method => method with
            {
                Segments = method.Segments.Select(segment => segment with { SynchronousExceptionTarget = null }).ToArray(),
            }).ToArray() },
            document with { ExceptionFlows = document.ExceptionFlows!.Select(flow => flow with { MethodSymbolId = Id("Run") }).ToArray() },
        }) check(!SemanticExceptionFlowContractValidator.IsValid(broken)
            && !SemanticLanguageErrorEffectPlanner.TryBuild(broken, out _),
            "Effect composition must reject damaged async routes, sync flow identity and provenance");
        var legacy = Analyze("""
            using System; using System.Threading.Tasks;
            public static class Script {
                public static int Fail() { throw new ArgumentException(); }
                public static async Task<int> Run() { return Fail(); }
                public static Task<int> Factory() { return Run(); }
            }
            """, enabled: false);
        check(SemanticLanguageErrorEffectPlanner.TryBuild(legacy, out var oldPlan)
            && oldPlan!.AsyncBoundaryMethodIds.Count == 0
            && oldPlan.OutcomeMethodIds.Any(id => id.Contains(".Run(", StringComparison.Ordinal))
            && oldPlan.OutcomeMethodIds.Any(id => id.Contains(".Factory(", StringComparison.Ordinal)),
            "Legacy effect closure must retain its old rejection behavior instead of adopting new Task semantics");
    }

    private static string Source(string body) => """
        using System; using System.Threading.Tasks; using AvidScript;
        public sealed class Target { public int Field; public int Property { get; set; } }
        public static class Script {
            public static int Trace;
            public static Target Select(int mode) { if (mode == 7) throw new ArgumentException(); return new Target(); }
            public static int Value(int mode) { if (mode == 7) throw new ArgumentException(); return mode; }
            public static async Task<int> Read(int mode) { await AvidContinuations.NextTickAsync(); return mode; }
            public static async Task<int> Run(Target target, int mode) {
        """ + body + "} }";

    private static SemanticDocument Analyze(string source, bool enabled = true, string sourceId = "Scripts/AsyncSynchronousExceptions.cs") =>
        SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://Continuation.cs", true) }, new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
            enableAsyncSynchronousExceptions: enabled);

    private const string Facade = """
        using System; using System.Runtime.CompilerServices;
        namespace AvidScript;
        public readonly struct AvidCancellationToken { public readonly long Value; }
        public readonly struct AvidCancellationSource { public AvidCancellationToken Token => default; }
        public static class AvidContinuations { public static AvidDelayAwaitable NextTickAsync() => default; }
        public readonly struct AvidDelayAwaitable {
            public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => this;
            public AvidDelayAwaiter GetAwaiter() => default;
        }
        public readonly struct AvidDelayAwaiter : ICriticalNotifyCompletion {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }
        """;
}
