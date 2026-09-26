using System;
using System.Threading.Tasks;
using AvidScript;

[UClass]
public partial class SharedTaskActor : AvidActor
{
    [UProperty] public int Value { get; set; }

    [UFunction]
    public int GetScriptValue()
    {
        RunAndStore(Value);
        return Value;
    }

    private async void RunAndStore(int seed)
    {
        int value = await SharedTaskFlow.Run(seed);
        Value = value;
    }
}

public static class SharedTaskFlow
{
    private const int CodeOffset = 0;
    public static int Completed;

    private static async Task<int> Child(int value, AvidCancellationToken token)
    {
        await AvidContinuations.NextTickAsync().WithCancellation(token);
        return value + CodeOffset;
    }

    public static async Task<int> Run(int seed)
    {
        AvidCancellationSource cancellation = AvidCancellationSource.Create();
        Task<int> pending = Child(seed, cancellation.Token);
        Task<int> saved = pending;
        pending = Child(seed + 1, cancellation.Token);
        if (seed == -1) cancellation.Cancel();
        int result = 0;
        try
        {
            for (int i = 0; i < 2; i++)
            {
                Task<int> iteration = saved;
                if (i == 1) iteration = pending;
                int value = await iteration;
                result += value;
                if (i == 0) continue;
                break;
            }
        }
        catch (TaskCanceledException) { result = 90; }
        finally { cancellation.Release(); }

        await AvidContinuations.NextTickAsync();
        // Trap after suspension, with Task locals still owned by this method.
        if (seed == -2) result = 1 / (seed + 2);
        Completed++;
        return result;
    }
}
