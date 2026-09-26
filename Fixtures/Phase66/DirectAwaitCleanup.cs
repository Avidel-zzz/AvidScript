using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using AvidScript;

public sealed class AwaitValue
{
    public int Value;
    public AwaitValue(int value) { Value = value; }
}

public static class Script
{
    public static int Result;
    public static int CleanupCount;
    public static int CatchCount;
    public static int CleanupMode;
    public static int ReloadMode;
    public static int BeginCount;
    internal static AvidCancellationSource Lifetime;

    public static async Task<int> RunAsync()
    {
        AwaitValue captured = new AwaitValue(16);
        try
        {
            await AvidContinuations.NextTickAsync()
                .WithCancellation(Lifetime.Token);
            return captured.Value;
        }
        catch (InvalidOperationException)
        {
            CatchCount++;
            return 17;
        }
        finally
        {
            CleanupCount++;
            if (CleanupMode != 0) throw new InvalidOperationException();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        BeginCount++;
        Lifetime = AvidCancellationSource.Create();
        Task<int> pending = RunAsync();
        if (ReloadMode != 0) Lifetime.Cancel();
        // A reload candidate can trap after owning a queued cancellation and a
        // managed capture. CleanupCount is still zero before any resume occurs.
        if (ReloadMode == 2) Result = 1 / CleanupCount;
        Result = await pending;
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (deltaSeconds < 0.0f) Lifetime.Cancel();
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int mode, float cancel)
    {
        ReloadMode = mode;
        if (cancel < 0.0f) Lifetime.Cancel();
    }
}
