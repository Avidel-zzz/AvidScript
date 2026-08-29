using System.Runtime.InteropServices;

namespace AvidScript;

public static class LatentGameplayScript
{
    private static int GameplayPhase;

    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        GameplayPhase = 1;
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));

        await UKismetSystemLibrary.DelayAsync(0.25f);

        GameplayPhase = 2;
        UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (GameplayPhase == 2 && deltaSeconds > 0.0f)
        {
            GameplayPhase = 3;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        GameplayPhase = 0;
    }
}
