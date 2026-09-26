using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncTaskOwnerFlowTests
{
    private static int passed;
    private static readonly string[] Owners = { "outer", "local", "alias" };
    private static readonly IReadOnlyDictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>> NoReleases =
        new Dictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>>();

    public static int Run()
    {
        passed = 0;
        var linear = new[] { Block(0, new[] { "outer" }, 1), Block(1, new[] { "outer" }, 2), Block(2) };
        var state = Solve(linear, NoReleases);
        Check(state.DefiniteAtEntry[0].Count == 0 && state.DefiniteAtEntry[1].SequenceEqual(new[] { "outer" })
            && state.DefiniteAtExit[1].SequenceEqual(new[] { "outer" }),
            "replacement has an old owner before the write and one live slot afterwards");

        var diamond = new[] { Block(0, Array.Empty<string>(), 1, 2), Block(1, new[] { "local" }, 3),
            Block(2, Array.Empty<string>(), 3), Block(3) };
        state = Solve(diamond, NoReleases);
        Check(state.PossibleAtEntry[3].SequenceEqual(new[] { "local" }) && state.DefiniteAtEntry[3].Count == 0,
            "conditional initialization requires guarded cleanup but cannot authorize a read");
        var branchRetirement = Release((1, 3, new[] { "local" }));
        state = Solve(diamond, branchRetirement);
        Check(state.PossibleAtEntry[3].Count == 0
            && state.ReleasedOnEdge[new(1, 3)].SequenceEqual(new[] { "local" })
            && state.ReleasedOnEdge[new(2, 3)].Count == 0, "only the branch that acquired an owner releases it");

        // Edges model continue, break, failure/cancellation dispatch and a normal
        // backedge. An outer alias remains owned when the iteration local exits.
        var loop = new[] { Block(0, new[] { "outer", "alias" }, 1),
            Block(1, Array.Empty<string>(), 2, 4), Block(2, new[] { "local" }, 3, 1, 4, 5),
            Block(3, Array.Empty<string>(), 1), Block(4), Block(5) };
        var loopReleases = Release((2, 1, new[] { "local" }), (2, 4, new[] { "local" }),
            (2, 5, new[] { "local" }), (3, 1, new[] { "local" }));
        state = Solve(loop, loopReleases);
        Check(!state.PossibleAtEntry[1].Contains("local") && !state.PossibleAtEntry[2].Contains("local"),
            "loop declaration is reached without a previous iteration owner");
        Check(state.DefiniteAtEntry[3].Count == 3, "suspension within the iteration retains all live slots");
        Check(state.DefiniteAtEntry[4].SequenceEqual(new[] { "alias", "outer" })
            && state.DefiniteAtEntry[5].SequenceEqual(new[] { "alias", "outer" }),
            "break and exceptional exit retain outer owners and retire only the local");
        Check(new[] { new SemanticAsyncTaskOwnerEdge(2, 1), new(2, 4), new(2, 5), new(3, 1) }
            .All(edge => state.ReleasedOnEdge[edge].SequenceEqual(new[] { "local" })),
            "all iteration exits have an explicit cleanup set");
        var missingBackedge = Release((2, 1, new[] { "local" }), (2, 4, new[] { "local" }), (2, 5, new[] { "local" }));
        var leaked = Solve(loop, missingBackedge);
        Check(leaked.PossibleAtEntry[2].Contains("local") && !leaked.DefiniteAtEntry[2].Contains("local"),
            "a missing scope exit remains observable as a possible overwritten owner");

        var entryCycle = new[] { Block(0, new[] { "local" }, 0, 1), Block(1) };
        state = Solve(entryCycle, NoReleases);
        Check(state.PossibleAtEntry[0].Contains("local") && state.DefiniteAtEntry[0].Count == 0,
            "the external entry path remains empty even when a backedge enters it");
        state = Solve(entryCycle, Release((0, 0, new[] { "local" })));
        Check(state.PossibleAtEntry[0].Count == 0 && state.DefiniteAtEntry[1].Contains("local"),
            "retirement on one successor must not retire the sibling successor");

        var conditionalExit = new[] { Block(0, Array.Empty<string>(), 1, 2), Block(1, new[] { "local" }, 2),
            Block(2, Array.Empty<string>(), 3), Block(3) };
        state = Solve(conditionalExit, Release((2, 3, new[] { "local", "alias" })));
        Check(state.ReleasedOnEdge[new(2, 3)].SequenceEqual(new[] { "local" })
            && state.PossibleAtEntry[3].Count == 0, "cleanup includes possible owners, excluding never-created slots");

        var unreachable = linear.Append(Block(7, new[] { "alias" }, 2)).ToArray();
        state = Solve(unreachable, NoReleases);
        Check(!state.PossibleAtEntry.ContainsKey(7) && !state.PossibleAtEntry[2].Contains("alias"),
            "unreachable producers cannot authorize ownership at a live merge");
        Check(Canonical(Solve(loop.Reverse().ToArray(), loopReleases)) == Canonical(Solve(loop, loopReleases)),
            "block ordering does not change the solved ownership sets");

        Reject(null!, linear, NoReleases, "null owners");
        Reject(Owners, null!, NoReleases, "null blocks");
        Reject(Owners, linear, null!, "null release facts");
        Reject(Array.Empty<string>(), linear, NoReleases, "empty ownership universe");
        Reject(new[] { "local", "local" }, linear, NoReleases, "duplicate owner identities");
        Reject(new[] { "" }, linear, NoReleases, "empty owner identity");
        Reject(Enumerable.Range(0, 9).Select(i => "owner" + i).ToArray(), linear, NoReleases, "owner limit");
        Reject(Owners, Array.Empty<SemanticAsyncTaskOwnerBlock>(), NoReleases, "empty CFG");
        Reject(Owners, new[] { Block(1) }, NoReleases, "missing entry");
        Reject(Owners, new[] { Block(-1), Block(0) }, NoReleases, "negative ordinal");
        Reject(Owners, new[] { Block(0), Block(0) }, NoReleases, "duplicate block identity");
        Reject(Owners, new[] { Block(0, Array.Empty<string>(), 5) }, NoReleases, "missing successor");
        Reject(Owners, new[] { Block(0, Array.Empty<string>(), 0, 0) }, NoReleases, "duplicate successor");
        Reject(Owners, new[] { Block(0, new[] { "unknown" }) }, NoReleases, "foreign generated owner");
        Reject(Owners, new[] { Block(0, new[] { "local", "local" }) }, NoReleases, "duplicate generated fact");
        Reject(Owners, linear, Release((0, 2, new[] { "local" })), "release on a nonexistent edge");
        Reject(Owners, linear, Release((9, 2, new[] { "local" })), "release from an unknown block");
        Reject(Owners, linear, Release((0, 1, new[] { "unknown" })), "foreign released owner");
        Reject(Owners, linear, Release((0, 1, new[] { "local", "local" })), "duplicate released owner");
        Reject(Owners, Enumerable.Range(0, 65).Select(i => Block(i)).ToArray(), NoReleases, "CFG budget");

        // Independent oracle explores the finite set of concrete ownership masks,
        // rather than reproducing the solver's may/must fixed-point algorithm.
        Random random = new(6612);
        for (int sample = 0; sample < 256; ++sample)
        {
            int count = random.Next(2, 7);
            var graph = Enumerable.Range(0, count).Select(id => Block(id,
                Owners.Where(_ => random.Next(3) == 0).ToArray(),
                Enumerable.Range(0, count).Where(_ => random.Next(3) == 0).ToArray())).ToArray();
            var exits = new Dictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>>();
            foreach (var node in graph)
                foreach (int target in node.Successors)
                    exits.Add(new(node.Ordinal, target), Owners.Where(_ => random.Next(3) == 0).ToArray());
            CompareConcretePaths(graph, exits, sample);
        }
        Check(true, "256 cyclic and branching CFGs agree with finite concrete path exploration");
        return passed;
    }

    private static void CompareConcretePaths(SemanticAsyncTaskOwnerBlock[] graph,
        IReadOnlyDictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>> releases, int sample)
    {
        int Mask(IEnumerable<string> ids) => ids.Aggregate(0, (bits, id) => bits | (1 << Array.IndexOf(Owners, id)));
        var paths = new HashSet<(int Block, int Owners)>();
        var queue = new Queue<(int Block, int Owners)>();
        queue.Enqueue((0, 0));
        while (queue.TryDequeue(out var path))
        {
            if (!paths.Add(path)) continue;
            int after = path.Owners | Mask(graph[path.Block].GeneratedOwners);
            foreach (int target in graph[path.Block].Successors)
                queue.Enqueue((target, after & ~Mask(releases[new(path.Block, target)])));
        }
        var actual = Solve(graph, releases);
        foreach (var group in paths.GroupBy(path => path.Block))
        {
            int beforeMay = group.Aggregate(0, (bits, path) => bits | path.Owners);
            int beforeMust = group.Aggregate(7, (bits, path) => bits & path.Owners);
            int generated = Mask(graph[group.Key].GeneratedOwners);
            if (Mask(actual.PossibleAtEntry[group.Key]) != beforeMay
                || Mask(actual.DefiniteAtEntry[group.Key]) != beforeMust
                || Mask(actual.PossibleAtExit[group.Key]) != (beforeMay | generated)
                || Mask(actual.DefiniteAtExit[group.Key]) != (beforeMust | generated))
                throw new InvalidOperationException($"Concrete paths disagree at sample {sample}, block {group.Key}.");
            foreach (int target in graph[group.Key].Successors)
            {
                var edge = new SemanticAsyncTaskOwnerEdge(group.Key, target);
                if (Mask(actual.ReleasedOnEdge[edge]) != ((beforeMay | generated) & Mask(releases[edge])))
                    throw new InvalidOperationException($"Concrete cleanup disagrees at sample {sample}, edge {edge}.");
            }
        }
        if (actual.PossibleAtEntry.Count != paths.Select(path => path.Block).Distinct().Count())
            throw new InvalidOperationException("Unreachable blocks entered the solved state.");
    }

    private static SemanticAsyncTaskOwnerBlock Block(int id, IReadOnlyList<string>? generated = null, params int[] next) =>
        new(id, generated ?? Array.Empty<string>(), next);
    private static Dictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>> Release(
        params (int Source, int Target, string[] Owners)[] edges) =>
        edges.ToDictionary(edge => new SemanticAsyncTaskOwnerEdge(edge.Source, edge.Target), edge => (IReadOnlyList<string>)edge.Owners);
    private static SemanticAsyncTaskOwnerStates Solve(IReadOnlyList<SemanticAsyncTaskOwnerBlock> blocks,
        IReadOnlyDictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>> releases)
    {
        if (!SemanticAsyncTaskOwnerFlowSolver.TrySolve(0, Owners, blocks, releases, out var states))
            throw new InvalidOperationException("Valid ownership graph was rejected.");
        return states!;
    }
    private static void Reject(IReadOnlyCollection<string> owners, IReadOnlyList<SemanticAsyncTaskOwnerBlock> blocks,
        IReadOnlyDictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>> releases, string reason) =>
        Check(!SemanticAsyncTaskOwnerFlowSolver.TrySolve(0, owners, blocks, releases, out _), reason);
    private static string Canonical(SemanticAsyncTaskOwnerStates states) => string.Join("|",
        states.PossibleAtEntry.Keys.Order().Select(id => $"{id}:"
            + string.Join(",", states.PossibleAtEntry[id]) + "/" + string.Join(",", states.DefiniteAtEntry[id])
            + "/" + string.Join(",", states.PossibleAtExit[id]) + "/" + string.Join(",", states.DefiniteAtExit[id])))
        + string.Join("|", states.ReleasedOnEdge.OrderBy(pair => pair.Key.Source).ThenBy(pair => pair.Key.Target)
            .Select(pair => $"{pair.Key}:" + string.Join(",", pair.Value)));
    private static void Check(bool condition, string reason)
    {
        if (!condition) throw new InvalidOperationException(reason);
        passed++;
    }
}
