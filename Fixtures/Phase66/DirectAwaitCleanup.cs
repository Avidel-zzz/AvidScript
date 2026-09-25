using System.Runtime.InteropServices;
using System.Threading.Tasks;
using AvidScript;

public static class Script
{
    public static int Result;
    public static int CleanupCount;
    private static AvidCancellationSource Lifetime;

    public static async Task<int> RunAsync()
    {
        try
        {
            await AvidContinuations.NextTickAsync()
                .WithCancellation(Lifetime.Token);
            return 16;
        }
        finally
        {
            CleanupCount++;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        Lifetime = AvidCancellationSource.Create();
        Result = await RunAsync();
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (deltaSeconds < 0.0f) Lifetime.Cancel();
    }
}
