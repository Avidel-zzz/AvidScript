using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using AvidScript;

public static class Script
{
    public static int CleanupCount;
    public static int Result;
    public static int TestMode;

    private static async Task<int> LoadAsync(int mode)
    {
        await AvidContinuations.NextTickAsync();
        if (mode == 1 || mode == 3 || mode == 4)
        {
            throw new InvalidOperationException();
        }
        if (mode == 2)
        {
            throw new ArgumentException();
        }
        return 12;
    }

    public static async Task<int> RunAsync(int mode)
    {
        try
        {
            int value = await LoadAsync(mode);
            return value;
        }
        catch (InvalidOperationException)
        {
            if (mode == 4)
            {
                throw;
            }
            return 7;
        }
        finally
        {
            CleanupCount++;
            if (mode == 3)
            {
                throw new ArgumentException();
            }
        }
    }

    public static async Task<int> RunNestedAsync()
    {
        try
        {
            int value = await RunAsync(1);
            return value;
        }
        finally
        {
            CleanupCount += 10;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        if (TestMode == 5)
        {
            Result = await RunNestedAsync();
        }
        else
        {
            Result = await RunAsync(TestMode);
        }
    }
}
