// Appended to AsyncCancellationFlow.cs by the compiler test. The shared method
// bodies remain identical to the .NET reference; this file supplies UE callbacks.
public static class CancellationGuestEntry
{
    public static int TestCase;
    public static int Result;

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        CancellationScript.Lifetime = AvidScript.AvidCancellationSource.Create();
        if (TestCase < 3)
            Result = await CancellationScript.OuterAsync(TestCase);
        else if (TestCase == 3)
            Result = await CancellationScript.UnhandledAsync();
        else if (TestCase == 4)
            Result = await CancellationScript.NestedAsync();
        else if (TestCase == 5)
            Result = await CancellationScript.CatchAllAsync();
        else if (TestCase == 6)
        {
            // Hold a Task local until both child callbacks have completed. The
            // final await must take the ready path without another callback.
            System.Threading.Tasks.Task<int> terminal = CancellationScript.UnhandledAsync();
            await AvidScript.AvidContinuations.NextTickAsync();
            await AvidScript.AvidContinuations.NextTickAsync();
            await AvidScript.AvidContinuations.NextTickAsync();
            Result = await terminal;
            return;
        }
        else if (TestCase == 7)
        {
            // A real VM trap after a successful await must not acquire the
            // language diagnostic of an earlier, already handled cancellation.
            int value = await CancellationScript.OuterAsync(0);
            Result = value / (TestCase - 7);
        }
        else if (TestCase < 12)
            Result = await CancellationScript.RepeatedAsync(TestCase - 8);
        else
            Result = await CancellationScript.ConditionalAsync(TestCase - 12);
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (deltaSeconds < 0.0f) CancellationScript.Lifetime.Cancel();
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        CancellationScript.Lifetime.Release();
    }
}
