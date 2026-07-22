using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class PlayablePickupScript
{
    private const int RespawnTimerId = 7;
    private const float RotationSpeed = 90.0f;
    private const float RespawnDelay = 3.0f;
    [AvidPersist]
    private static bool Collected;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        if (Collected)
        {
            UE.SetTimer(RespawnDelay, RespawnTimerId);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (Collected)
        {
            return;
        }

        FRotator rotation = UE.Self.GetActorRotation();
        UE.Self.SetActorRotation(
            rotation + new FRotator(0.0f, RotationSpeed * deltaSeconds, 0.0f),
            false);
    }

    public static void OnBeginOverlap(AActor otherActor, FVector location)
    {
        if (Collected || !otherActor.HasHandle || !otherActor.ActorHasTag("Player"))
        {
            return;
        }

        Collected = true;
        UE.Self.SetActorHiddenInGame(true);
        UE.Self.SetActorEnableCollision(false);
        UE.SetTimer(RespawnDelay, RespawnTimerId);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle)
    {
        if (callbackId != RespawnTimerId)
        {
            return;
        }

        Collected = false;
        UE.Self.SetActorHiddenInGame(false);
        UE.Self.SetActorEnableCollision(true);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value)
    {
    }
}
