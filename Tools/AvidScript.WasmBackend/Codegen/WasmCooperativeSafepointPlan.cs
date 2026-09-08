using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

public sealed record WasmCompilationOptions(
    bool EnableCooperativeSafepoints = false,
    uint SafepointInterval = 256);

internal sealed class WasmCooperativeSafepointPlan
{
    public const string ImportId = "avidscript:cooperative_safepoint_poll";
    public const string ImportModule = "avidscript";
    public const string ImportName = "avid_cooperative_safepoint_poll";
    public const uint CounterGlobalIndex = 1;

    private readonly IReadOnlyDictionary<string, IReadOnlySet<string>> loopPollBlocks;
    private readonly IReadOnlySet<string> recursiveFunctions;
    private readonly IReadOnlyList<string> siteIdentities;

    private WasmCooperativeSafepointPlan(
        bool enabled,
        uint interval,
        IReadOnlyDictionary<string, IReadOnlySet<string>> inLoopPollBlocks,
        IReadOnlySet<string> inRecursiveFunctions)
    {
        Enabled = enabled;
        Interval = interval;
        loopPollBlocks = inLoopPollBlocks;
        recursiveFunctions = inRecursiveFunctions;
        LoopPollCount = loopPollBlocks.Values.Sum(blocks => blocks.Count);
        RecursiveFunctionCount = recursiveFunctions.Count;
        siteIdentities = loopPollBlocks
            .SelectMany(pair => pair.Value.Select(
                blockId => $"edge:{pair.Key}:{blockId}"))
            .Concat(recursiveFunctions.Select(
                functionId => $"entry:{functionId}"))
            .OrderBy(identity => identity, StringComparer.Ordinal)
            .ToArray();
        SiteSha256 = Convert.ToHexString(SHA256.HashData(
            Encoding.UTF8.GetBytes(string.Join('\n', siteIdentities))))
            .ToLowerInvariant();
    }

    public bool Enabled { get; }

    public uint Interval { get; }

    public int LoopPollCount { get; }

    public int RecursiveFunctionCount { get; }

    public int SiteCount => siteIdentities.Count;

    public string SiteSha256 { get; }

    public WasmCooperativeSafepointAttestation? CreateAttestation(
        int emittedPollCount)
    {
        if (!Enabled)
        {
            return null;
        }
        if (emittedPollCount != SiteCount)
        {
            throw new InvalidOperationException(
                $"Cooperative safepoint emission count differs from the plan: planned={SiteCount} emitted={emittedPollCount}.");
        }
        return new WasmCooperativeSafepointAttestation(
            2,
            Interval,
            LoopPollCount,
            RecursiveFunctionCount,
            SiteCount,
            SiteSha256,
            true);
    }

    public static WasmCooperativeSafepointPlan Create(
        GuestModule module,
        WasmCompilationOptions options)
    {
        ArgumentNullException.ThrowIfNull(module);
        ArgumentNullException.ThrowIfNull(options);
        if (!options.EnableCooperativeSafepoints)
        {
            return new WasmCooperativeSafepointPlan(
                false,
                0,
                new Dictionary<string, IReadOnlySet<string>>(StringComparer.Ordinal),
                new HashSet<string>(StringComparer.Ordinal));
        }
        if (options.SafepointInterval is 0 or > 65536)
        {
            throw new InvalidOperationException(
                "Cooperative safepoint interval must be in the range 1..65536.");
        }

        Dictionary<string, IReadOnlySet<string>> loopPollBlocks = new(StringComparer.Ordinal);
        foreach (GuestFunction function in module.Functions)
        {
            loopPollBlocks.Add(function.Id, FindFeedbackEdgeSources(function));
        }

        HashSet<string> recursiveFunctions = FindRecursiveFunctions(module);
        return new WasmCooperativeSafepointPlan(
            true,
            options.SafepointInterval,
            loopPollBlocks,
            recursiveFunctions);
    }

    public bool PollAtFunctionEntry(string functionId)
    {
        return Enabled && recursiveFunctions.Contains(functionId);
    }

    public bool PollBeforeTerminator(string functionId, string blockId)
    {
        return Enabled
            && loopPollBlocks.TryGetValue(functionId, out IReadOnlySet<string>? blocks)
            && blocks.Contains(blockId);
    }

    private static IReadOnlySet<string> FindFeedbackEdgeSources(GuestFunction function)
    {
        Dictionary<string, GuestBasicBlock> blocks = function.Blocks.ToDictionary(
            block => block.Id,
            StringComparer.Ordinal);
        Dictionary<string, byte> state = function.Blocks.ToDictionary(
            block => block.Id,
            _ => (byte)0,
            StringComparer.Ordinal);
        HashSet<string> pollBlocks = new(StringComparer.Ordinal);

        void Visit(string blockId)
        {
            state[blockId] = 1;
            foreach (string targetId in EnumerateTargets(blocks[blockId].Terminator))
            {
                if (!state.TryGetValue(targetId, out byte targetState))
                {
                    continue;
                }
                if (targetState == 1)
                {
                    pollBlocks.Add(blockId);
                }
                else if (targetState == 0)
                {
                    Visit(targetId);
                }
            }
            state[blockId] = 2;
        }

        if (blocks.ContainsKey(function.EntryBlockId))
        {
            Visit(function.EntryBlockId);
        }
        foreach (GuestBasicBlock block in function.Blocks)
        {
            if (state[block.Id] == 0)
            {
                Visit(block.Id);
            }
        }
        return pollBlocks;
    }

    private static IEnumerable<string> EnumerateTargets(GuestTerminator terminator)
    {
        if (!string.IsNullOrEmpty(terminator.TargetBlockId))
        {
            yield return terminator.TargetBlockId;
        }
        if (!string.IsNullOrEmpty(terminator.FalseTargetBlockId)
            && !string.Equals(
                terminator.FalseTargetBlockId,
                terminator.TargetBlockId,
                StringComparison.Ordinal))
        {
            yield return terminator.FalseTargetBlockId;
        }
    }

    private static HashSet<string> FindRecursiveFunctions(GuestModule module)
    {
        HashSet<string> functionIds = module.Functions
            .Select(function => function.Id)
            .ToHashSet(StringComparer.Ordinal);
        Dictionary<string, string[]> callees = module.Functions.ToDictionary(
            function => function.Id,
            function => function.Blocks
                .SelectMany(block => block.Instructions)
                .Where(instruction => instruction.Op == "call"
                    && instruction.TargetId is not null
                    && functionIds.Contains(instruction.TargetId))
                .Select(instruction => instruction.TargetId!)
                .Distinct(StringComparer.Ordinal)
                .ToArray(),
            StringComparer.Ordinal);
        HashSet<string> recursive = new(StringComparer.Ordinal);
        foreach (string functionId in functionIds)
        {
            HashSet<string> visited = new(StringComparer.Ordinal);
            Stack<string> pending = new(callees[functionId].Reverse());
            while (pending.Count > 0)
            {
                string candidate = pending.Pop();
                if (candidate == functionId)
                {
                    recursive.Add(functionId);
                    break;
                }
                if (!visited.Add(candidate))
                {
                    continue;
                }
                foreach (string callee in callees[candidate].Reverse())
                {
                    pending.Push(callee);
                }
            }
        }
        return recursive;
    }
}
