using System.Runtime.InteropServices;

namespace AvidScript;

public static class AsyncArrayForeachScript
{
    private static int CompletedSteps;

    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        int[] scalePercent = new[] { 100, 110, 125 };
        CompletedSteps = 0;

        foreach (int percent in scalePercent)
        {
            await AvidContinuations.NextTickAsync();
            float scale = (float)percent / 100.0f;
            UE.Self.SetActorScale3D(new FVector(scale, scale, scale));
            ++CompletedSteps;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        CompletedSteps = 0;
    }
}
