using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class DynamicProjectileScript
{
    private const int SpawnTimerId = 49;
    private const int DestroyTimerId = 50;
    private const float SpawnDelaySeconds = 0.01f;
    private const float DestroyDelaySeconds = 0.5f;

    [AvidTransient]
    private static AActor Projectile;
    [AvidTransient]
    private static bool ProjectileAlive;
    [AvidTransient]
    private static float ActiveSeconds;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        UE.SetTimer(SpawnDelaySeconds, SpawnTimerId);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (!ProjectileAlive || !Projectile.HasHandle)
        {
            return;
        }

        ActiveSeconds += deltaSeconds;
        FVector location = Projectile.GetActorLocation();
        float uniformScale = 1.0f + ActiveSeconds + location.Z * 0.0f;
        Projectile.SetActorScale3D(new FVector(uniformScale, uniformScale, uniformScale));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle)
    {
        if (callbackId == SpawnTimerId)
        {
            Projectile = UE.SpawnActor(
                ProjectClasses.TwinStickProjectile,
                new FTransform(
                    new FVector(300.0f, 0.0f, 150.0f),
                    FRotator.Zero,
                    new FVector(1.0f, 1.0f, 1.0f)));
            ProjectileAlive = false;
            if (Projectile.HasHandle)
            {
                ProjectileAlive = UE.IsA(
                    Projectile,
                    ProjectClasses.TwinStickProjectile);
            }

            if (ProjectileAlive)
            {
                UE.SetTimer(DestroyDelaySeconds, DestroyTimerId);
            }

            return;
        }

        if (callbackId == DestroyTimerId && ProjectileAlive)
        {
            UE.DestroyActor(Projectile);
            ProjectileAlive = false;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
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
