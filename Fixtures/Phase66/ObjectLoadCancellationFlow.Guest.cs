public static class ObjectCancellationGuestEntry
{
    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        int result = await ObjectCancellationScript.RunAsync();
        ObjectCancellationScript.Result = result;
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (deltaSeconds < -1.5f) ObjectCancellationScript.Lifetime.Release();
        else if (deltaSeconds < 0.0f) ObjectCancellationScript.Lifetime.Cancel();
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() { ObjectCancellationScript.Lifetime.Release(); }
}
