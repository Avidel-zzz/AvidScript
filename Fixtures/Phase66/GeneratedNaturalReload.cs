using System;
using System.Threading;
using System.Threading.Tasks;
using AvidScript;

[UClass]
public partial class ReloadFlowActor : AvidActor
{
    [UProperty] public int Value { get; set; }
    [UProperty] public int Stage { get; set; }

    protected override void BeginPlay()
    {
        int ordinal = ReloadFlow.Begin();
        Value += ReloadFlow.CodeOffset;
        Stage = 1;
        Store(Value);
        ReloadFlow.CheckCandidate(ordinal);
    }

    private async void Store(int seed)
    {
        int result = await ReloadFlow.Run(seed);
        Value = result;
        Stage = 2;
    }

    [UFunction] public int GetBeginCount() => ReloadFlow.BeginCount;
    [UFunction] public int GetCompletedCount() => ReloadFlow.Completed;
    [UFunction] public int GetCleanedCount() => ReloadFlow.Cleaned;
}

[UClass]
public partial class ReloadFlowComponent : AvidActorComponent
{
    [UProperty] public int Value { get; set; }
    [UProperty] public int Stage { get; set; }

    protected override void BeginPlay()
    {
        int ordinal = ReloadFlow.Begin();
        Value += ReloadFlow.CodeOffset;
        Stage = 1;
        Store(Value);
        ReloadFlow.CheckCandidate(ordinal);
    }

    private async void Store(int seed)
    {
        int result = await ReloadFlow.Run(seed);
        Value = result;
        Stage = 2;
    }

    [UFunction] public int GetBeginCount() => ReloadFlow.BeginCount;
    [UFunction] public int GetCompletedCount() => ReloadFlow.Completed;
    [UFunction] public int GetCleanedCount() => ReloadFlow.Cleaned;
}

public static class ReloadFlow
{
    public const int CodeOffset = 0;
    private const int RejectBegin = 0;
    private const int RejectAsyncSeed = 0;
    public static int BeginCount;
    public static int Completed;
    public static int Cleaned;
    [AvidTransient] public static int CandidateBegins;

    public static int Begin() { CandidateBegins++; return ++BeginCount; }

    public static void CheckCandidate(int ordinal)
    {
        if (RejectBegin != 0 && CandidateBegins == RejectBegin)
            throw new InvalidOperationException();
    }

    private static async Task<int> Child(int seed, CancellationToken token)
    {
        await AvidContinuations.NextTickAsync().WithCancellation(token);
        return seed + 7 + CodeOffset;
    }

    public static async Task<int> Run(int seed)
    {
        AvidCancellationSource cancellation = AvidCancellationSource.Create();
        CancellationToken token = cancellation.Token;
        Task<int> pending = Child(seed, token);
        int result = 0;
        try
        {
            result = await pending;
        }
        catch (OperationCanceledException error)
        {
            result = error.CancellationToken == token ? 90 : -1;
        }
        finally
        {
            Cleaned++;
            cancellation.Release();
        }
        await AvidContinuations.DelayAsync(0.1f);
        Completed++;
        if (RejectAsyncSeed != 0 && seed == RejectAsyncSeed)
            throw new InvalidOperationException();
        return result;
    }
}
