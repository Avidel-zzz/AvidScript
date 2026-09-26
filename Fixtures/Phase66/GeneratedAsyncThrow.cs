using System;
using System.Threading.Tasks;
using AvidScript;

[UClass]
public partial class AsyncThrowActor : AvidActor
{
    [UProperty] public int Value { get; set; }
    [UProperty] public int Stage { get; set; }

    [UFunction]
    public int GetScriptValue()
    {
        Stage = 0;
        RunAndStore(Value);
        return Value;
    }

    private async void RunAndStore(int scenario)
    {
        int value = await GeneratedThrowFlow.Run(scenario);
        Stage = 1;
        // Leave an observable suspension after exception replacement and cleanup.
        await AvidContinuations.DelayAsync(1.0f);
        Value = value;
        Stage = 2;
    }
}

public static class GeneratedThrowFlow
{
    private const int CodeOffset = 0;

    private static async Task<int> Child(AvidCancellationToken token)
    {
        await AvidContinuations.NextTickAsync().WithCancellation(token);
        return 7;
    }

    public static async Task<int> Run(int scenario)
    {
        AvidCancellationSource cancellation = AvidCancellationSource.Create();
        Task<int> pending = Child(cancellation.Token);
        Task<int> saved = pending;
        if (scenario == 1 || scenario == 3 || scenario == 4) cancellation.Cancel();
        int result = 0;
        int trace = 0;
        try
        {
            try
            {
                if (scenario == 4)
                    await AvidContinuations.NextTickAsync().WithCancellation(cancellation.Token);
                int value = await saved;
                result = value;
                if (scenario == 5) throw new ArgumentException();
            }
            catch (OperationCanceledException)
            {
                trace = trace * 10 + 1;
                throw new ArgumentException();
            }
            finally
            {
                trace = trace * 10 + 2;
                if (scenario == 2 || scenario == 3) throw new InvalidOperationException();
            }
        }
        catch (ArgumentException)
        {
            trace = trace * 10 + 3;
            result = 80;
        }
        catch (InvalidOperationException)
        {
            trace = trace * 10 + 5;
            result = 90;
        }
        finally
        {
            trace = trace * 10 + 4;
            cancellation.Release();
        }
        // Retain aliases to a completed/faulted child across a final suspension.
        await AvidContinuations.NextTickAsync();
        return result * 100000 + trace + CodeOffset;
    }
}
