using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using AvidScript;

public static class Script
{
    public static int Result;
    public static int CleanupCount;
    public static int TestMode;

    private static T Identity<T>(T value) => value;

    private static async Task<int> LoadAsync(int mode)
    {
        int sum = 0;
        await AvidContinuations.NextTickAsync();
        foreach (int value in new[] { 3, 5, 8 })
        {
            sum += Identity(value);
        }
        if (mode == 1)
        {
            throw new InvalidOperationException();
        }
        if (mode == 2)
        {
            throw new ArgumentException();
        }
        if (mode == 4)
        {
            throw new Exception();
        }
        return sum;
    }

    public static async Task<int> RunAsync(int mode)
    {
        try
        {
            await AvidContinuations.NextTickAsync();
            int value = await LoadAsync(mode);
            return value;
        }
        catch (ArgumentNullException)
        {
            return 99;
        }
        catch (SystemException)
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
        Result = await RunAsync(TestMode);
    }
}
