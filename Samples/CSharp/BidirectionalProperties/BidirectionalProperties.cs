using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class BidirectionalProperties
{
    private static float ElapsedSeconds;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        AActor self = UE.Self;
        ElapsedSeconds = 0.0f;
        self.CustomTimeDilation = 1.0f;
        self.SetActorLocation(new FVector(0.0f, 0.0f, 100.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        AActor self = UE.Self;
        ElapsedSeconds += deltaSeconds;
        self.CustomTimeDilation = 1.0f + ElapsedSeconds * 0.05f;

        FVector location = self.GetActorLocation();
        self.SetActorLocation(
            location + new FVector(60.0f * deltaSeconds, 0.0f, 0.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        AActor self = UE.Self;
        self.CustomTimeDilation = 1.0f;
    }
}
