using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class PickupRushScript
{
    public const int CollectEventId = 64001;
    public const int RestartEventId = 64002;

    private const int RespawnTimerId = 11;
    private const int GoalScore = 5;
    private const int Playing = 1;
    private const int Won = 2;
    private const int Lost = 3;
    private const float RoundSeconds = 20.0f;
    private const float RespawnSeconds = 0.35f;
    private const float RotationDegreesPerSecond = 120.0f;

    [AvidPersist]
    private static int State;
    [AvidPersist]
    private static int Score;
    [AvidPersist]
    private static float RemainingSeconds;
    [AvidPersist]
    private static FVector Origin;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        Origin = UE.Self.GetActorLocation();
        Restart();
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        FRotator rotation = UE.Self.GetActorRotation();
        float direction = State == Lost ? -1.0f : 1.0f;
        UE.Self.SetActorRotation(
            rotation + new FRotator(
                0.0f,
                RotationDegreesPerSecond * direction * deltaSeconds,
                0.0f),
            false);

        if (State != Playing)
        {
            return;
        }

        RemainingSeconds -= deltaSeconds;
        if (RemainingSeconds <= 0.0f)
        {
            State = Lost;
            UE.Self.SetActorHiddenInGame(false);
            UE.Self.SetActorEnableCollision(true);
            UE.Self.SetActorScale3D(new FVector(0.5f, 0.5f, 0.5f));
        }
    }

    public static void OnBeginOverlap(AActor otherActor, FVector location)
    {
        if (!otherActor.HasHandle)
        {
            return;
        }

        if (State == Won || State == Lost)
        {
            Restart();
            return;
        }
        Collect();
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value)
    {
        if (eventId == CollectEventId)
        {
            Collect();
            return;
        }
        if (eventId == RestartEventId)
        {
            Restart();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle)
    {
        if (callbackId != RespawnTimerId || State != Playing)
        {
            return;
        }
        FVector pickupLocation = GetPickupLocation(Score);
        FRotator pickupRotation = UE.Self.GetActorRotation();
        UE.Self.Teleport(pickupLocation, pickupRotation);
        UE.Self.SetActorHiddenInGame(false);
        UE.Self.SetActorEnableCollision(true);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        UE.Self.SetActorHiddenInGame(false);
        UE.Self.SetActorEnableCollision(false);
    }

    private static void Collect()
    {
        if (State != Playing)
        {
            return;
        }

        Score += 1;
        if (Score >= GoalScore)
        {
            State = Won;
            UE.Self.SetActorHiddenInGame(false);
            FVector winLocation = Origin + new FVector(0.0f, 0.0f, 180.0f);
            FRotator winRotation = UE.Self.GetActorRotation();
            UE.Self.Teleport(winLocation, winRotation);
            UE.Self.SetActorScale3D(new FVector(2.5f, 2.5f, 2.5f));
            UE.Self.SetActorEnableCollision(true);
            return;
        }

        float scale = 1.0f + 0.2f * Score;
        UE.Self.SetActorScale3D(new FVector(scale, scale, scale));
        UE.Self.SetActorHiddenInGame(true);
        UE.Self.SetActorEnableCollision(false);
        UE.SetTimer(RespawnSeconds, RespawnTimerId);
    }

    private static void Restart()
    {
        State = Playing;
        Score = 0;
        RemainingSeconds = RoundSeconds;
        UE.Self.Teleport(Origin, FRotator.Zero);
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
        UE.Self.SetActorHiddenInGame(false);
        UE.Self.SetActorEnableCollision(true);
    }

    private static FVector GetPickupLocation(int score)
    {
        switch (score)
        {
            case 1:
                return Origin + new FVector(300.0f, 0.0f, 0.0f);
            case 2:
                return Origin + new FVector(300.0f, 300.0f, 0.0f);
            case 3:
                return Origin + new FVector(0.0f, 300.0f, 0.0f);
            default:
                return Origin + new FVector(-250.0f, 120.0f, 0.0f);
        }
    }
}
