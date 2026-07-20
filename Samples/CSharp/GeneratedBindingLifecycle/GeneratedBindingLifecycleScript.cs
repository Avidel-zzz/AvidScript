using System.Runtime.InteropServices;

namespace AvidScript;

public static class GeneratedBindingLifecycleScript
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        SetScale(2.0f, 3.0f, 4.0f);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        FVector scale = UE.Self.GetActorScale3D();
        SetScale(
            scale.X + deltaSeconds,
            scale.Y,
            scale.Z);
    }

    private static void SetScale(float x, float y, float z)
    {
        UE.Self.SetActorScale3D(new FVector(x, y, z));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
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
}
