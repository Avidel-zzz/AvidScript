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
        else
            Result = await CancellationScript.CatchAllAsync();
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
