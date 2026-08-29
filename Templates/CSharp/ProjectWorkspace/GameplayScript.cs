using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class GameplayScript
{
    private const float RotationSpeedDegreesPerSecond = 90.0f;
    private const int DelayedBeginPlay = 1;
    private const int DefaultMeshLoaded = 2;

    [AvidPersist]
    private static float TotalRotationDegrees;

    [AvidTransient]
    private static AvidContinuation PendingBeginPlay;

    [AvidTransient]
    private static AvidContinuation PendingDefaultMesh;

    [AvidTransient]
    private static bool HasAwaitedDefaultMesh;

    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
        PendingBeginPlay = AvidContinuations.Delay(0.25f, DelayedBeginPlay);
        PendingDefaultMesh = AvidAssets.LoadObjectAsync(
            "/Engine/EngineMeshes/Cube.Cube",
            DefaultMeshLoaded);

        await AvidContinuations.NextTickAsync();
        AvidLoadedObject loadedObject = await AvidAssets.LoadObjectAsync(
            "/Engine/EngineMeshes/Cube.Cube");
        HasAwaitedDefaultMesh = loadedObject.IsValid;
        UE.Self.SetActorScale3D(new FVector(1.05f, 1.05f, 1.05f));
    }

    [AvidContinuation(DelayedBeginPlay)]
    public static void OnDelayedBeginPlay()
    {
        UE.Self.SetActorScale3D(new FVector(1.05f, 1.0f, 1.0f));
    }

    [AvidContinuation(DefaultMeshLoaded)]
    public static void OnDefaultMeshLoaded(
        AvidContinuationStatus status,
        AvidLoadedObject loadedObject)
    {
        if (status == AvidContinuationStatus.Completed && loadedObject.IsValid)
        {
            UE.Self.SetActorScale3D(new FVector(1.05f, 1.05f, 1.05f));
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        TotalRotationDegrees += RotationSpeedDegreesPerSecond * deltaSeconds;
        FRotator rotation = UE.Self.GetActorRotation();
        UE.Self.SetActorRotation(
            new FRotator(
                rotation.Pitch,
                rotation.Yaw + RotationSpeedDegreesPerSecond * deltaSeconds,
                rotation.Roll),
            false);
        UE.Self.SetActorScale3D(
            new FVector(1.0f + TotalRotationDegrees / 1000.0f, 1.0f, 1.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value)
    {
    }

    public static void OnBeginOverlap(AActor otherActor, FVector location)
    {
    }

    public static void OnEndOverlap(AActor otherActor, FVector location)
    {
    }

    public static void OnHit(AActor otherActor, FVector normalImpulse)
    {
    }

    public static void OnInput(InputEvent input)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        if (HasAwaitedDefaultMesh)
        {
            HasAwaitedDefaultMesh = false;
        }
    }
}
