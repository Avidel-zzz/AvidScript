using System.Runtime.InteropServices;

namespace AvidScript;

public static class GeneratedBindingLifecycleScript
{
    private const int ContinuationCallbackId = 62002;
    private const int TimerCallbackId = 62003;
    private const int PackagedFaultProbeEvent = 62004;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        USceneComponent root = UE.Self.RootComponent;
        FVector rootLocation = root.GetWorldLocation();
        SetScale(
            UE.Self.CustomTimeDilation * 2.0f + rootLocation.X,
            3.0f,
            4.0f);
        UE.SetTimer(0.05f, TimerCallbackId);
        AvidContinuations.Delay(0.04f, ContinuationCallbackId);
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

    [AvidContinuation(ContinuationCallbackId)]
    public static void OnContinuation()
    {
        FVector scale = UE.Self.GetActorScale3D();
        SetScale(scale.X, scale.Y + 40.0f, scale.Z);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        SetScale(1.0f, 1.0f, 1.0f);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle)
    {
        if (callbackId == TimerCallbackId)
        {
            FVector scale = UE.Self.GetActorScale3D();
            SetScale(scale.X, scale.Y, scale.Z + 50.0f);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value)
    {
        if (eventId == PackagedFaultProbeEvent)
        {
            int divisor = eventId - PackagedFaultProbeEvent;
            int unreachableValue = 1 / divisor;
            SetScale(unreachableValue, 0.0f, 0.0f);
            return;
        }

        FVector scale = UE.Self.GetActorScale3D();
        SetScale(scale.X, value, scale.Z);
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
