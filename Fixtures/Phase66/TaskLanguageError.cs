using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace AvidScript;

public static class TaskLanguageError
{
    private static float ElapsedSeconds;

    private static async Task<int> LoadAsync(bool fail)
    {
        await AvidContinuations.NextTickAsync();
        if (fail)
        {
            throw new InvalidOperationException();
        }
        return 12;
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        await LoadAsync(true);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        ElapsedSeconds += deltaSeconds;
    }
}
