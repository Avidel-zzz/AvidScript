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
        self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        AActor self = UE.Self;
        ElapsedSeconds += deltaSeconds;
        self.CustomTimeDilation = 1.0f + ElapsedSeconds * 0.05f;

        FVector scale = self.GetActorScale3D();
        self.SetActorScale3D(
            scale + new FVector(0.5f * deltaSeconds, 0.0f, 0.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        AActor self = UE.Self;
        self.CustomTimeDilation = 1.0f;
        self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
    }
}
