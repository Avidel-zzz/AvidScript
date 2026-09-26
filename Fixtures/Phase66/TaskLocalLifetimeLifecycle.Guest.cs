public static class TaskLifetimeLifecycleEntry
{
    public static int BeginCount;
    public static int ReloadMode;
    public static int Result;

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        BeginCount++;
        TaskLifetimeScript.Lifetime = AvidScript.AvidCancellationSource.Create();
        System.Threading.Tasks.Task<int> pending = TaskLifetimeScript.Run();
        if (ReloadMode != 0) TaskLifetimeScript.Lifetime.Cancel();
        // Fail only after the candidate owns replacement Tasks and queued work.
        if (ReloadMode == 2) Result = 1 / (ReloadMode - 2);
        Result = await pending;
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds) { }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int mode, float cancel)
    {
        ReloadMode = mode;
        if (cancel < 0.0f) TaskLifetimeScript.Lifetime.Cancel();
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() => TaskLifetimeScript.Lifetime.Release();
}
