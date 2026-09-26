// Appended to the shared cancellation source by the formal lifecycle build.
public static class CancellationLifecycleEntry
{
    public static int BeginCount;
    public static int ReloadMode;
    public static int Result;

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        BeginCount++;
        CancellationScript.Lifetime = AvidScript.AvidCancellationSource.Create();
        System.Threading.Tasks.Task<int> pending = CancellationScript.ConditionalAsync(0);
        if (ReloadMode != 0) CancellationScript.Lifetime.Cancel();
        // The rejected candidate already owns tasks, state and queued work.
        if (ReloadMode == 2) Result = 1 / (ReloadMode - 2);
        Result = await pending;
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (deltaSeconds < 0.0f) CancellationScript.Lifetime.Cancel();
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int mode, float cancel)
    {
        ReloadMode = mode;
        if (cancel < 0.0f) CancellationScript.Lifetime.Cancel();
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() => CancellationScript.Lifetime.Release();
}
