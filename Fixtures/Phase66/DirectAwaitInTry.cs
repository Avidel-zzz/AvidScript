using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using AvidScript;

public static class Script
{
    public static int Result;
    public static int CleanupCount;

    public static async Task<int> RunAsync()
    {
        try
        {
            await AvidContinuations.NextTickAsync();
            return 16;
        }
        catch (InvalidOperationException)
        {
            return 17;
        }
        finally
        {
            CleanupCount++;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        Result = await RunAsync();
    }
}
