using System.Runtime.InteropServices;

namespace AvidScript;

public static class GameplayScript
{
    private const float RotationSpeedDegreesPerSecond = 90.0f;

    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        FRotator rotation = UE.Self.GetActorRotation();
        UE.Self.SetActorRotation(
            new FRotator(
                rotation.Pitch,
                rotation.Yaw + RotationSpeedDegreesPerSecond * deltaSeconds,
                rotation.Roll),
            false);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_gameplay_event")]
    public static void OnGameplayEvent(
        int eventType,
        int primaryId,
        int secondaryId,
        int objectSlot,
        int objectGeneration,
        float x,
        float y,
        float z)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
    }
}
