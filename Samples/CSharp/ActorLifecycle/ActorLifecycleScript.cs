using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace AvidScript;

public static class ActorLifecycleScript
{
    private const int DeferredBeginPlay = 9;
    private const int DefaultMeshLoaded = 10;
    private static float ElapsedSeconds;
    private static AActor ActiveOverlapActor;
    private static bool HasActiveOverlap;
    private static bool HasPreviousInput;

    [AvidTransient]
    private static bool HasAwaitedDefaultMesh;
    private static int LastInputActionId;
    private static int LastInputTriggerEvent;

    [AvidTransient]
    private static AvidContinuation PendingDefaultMesh;

    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        UE.Self.SetActorLocation(new FVector(100.0f, 200.0f, 300.0f));
        UE.Self.AddActorWorldOffset(new FVector(0.0f, 0.0f, 0.0f));
        UE.Self.SetActorRotation(FRotator.Zero);
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
        UE.Self.GetRootComponent().SetWorldLocation(
            UE.Self.GetRootComponent().GetWorldLocation());
        UE.SetTimer(0.05f, 7);
        AvidContinuations.Delay(0.04f, DeferredBeginPlay);
        PendingDefaultMesh = AvidAssets.LoadObjectAsync(
            "/Engine/EngineMeshes/Cube.Cube",
            DefaultMeshLoaded);

        FVector loadedMeshOffset = new FVector(0.0f, 0.0f, 10.0f);
        await AvidContinuations.NextTickAsync();
        AvidLoadedObject loadedObject = await AvidAssets.LoadObjectAsync(
            "/Engine/EngineMeshes/Cube.Cube");
        await AvidContinuations.NextTickAsync();
        if (!loadedObject.IsValid)
        {
            return;
        }
        HasAwaitedDefaultMesh = loadedObject.IsValid;
        UE.Self.AddActorWorldOffset(loadedMeshOffset);
    }

    [AvidContinuation(DeferredBeginPlay)]
    public static void OnDeferredBeginPlay()
    {
        UE.Self.AddActorWorldOffset(new FVector(0.0f, 40.0f, 0.0f));
    }

    [AvidContinuation(DefaultMeshLoaded)]
    public static void OnDefaultMeshLoaded(
        AvidContinuationStatus status,
        AvidLoadedObject loadedObject)
    {
        if (status == AvidContinuationStatus.Completed && loadedObject.IsValid)
        {
            UE.Self.AddActorWorldOffset(new FVector(0.0f, 0.0f, 10.0f));
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        ElapsedSeconds += deltaSeconds;
        FVector currentLocation = UE.Self.GetActorLocation();
        UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
        FRotator currentRotation = UE.Self.GetActorRotation();
        UE.Self.SetActorRotation(currentRotation + new FRotator(0.0f, 90.0f * deltaSeconds, 0.0f));
        FVector currentScale = UE.Self.GetActorScale3D();
        UE.Self.SetActorScale3D(currentScale + new FVector(0.0f, 0.0f, 0.6f * deltaSeconds));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle)
    {
        UE.Self.AddActorWorldOffset(new FVector(0.0f, 0.0f, 50.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value)
    {
        UE.Self.AddActorWorldOffset(new FVector(0.0f, value, 0.0f));
    }

    public static void OnBeginOverlap(AActor otherActor, FVector location)
    {
        if (!otherActor.IsValid)
        {
            return;
        }

        if (HasActiveOverlap && ActiveOverlapActor.Matches(otherActor))
        {
            return;
        }

        HasActiveOverlap = true;
        ActiveOverlapActor = otherActor;
        otherActor.SetActorLocation(location + new FVector(0.0f, 10.0f, 0.0f));
    }

    public static void OnEndOverlap(AActor otherActor, FVector location)
    {
        if (!HasActiveOverlap || !otherActor.IsValid || !ActiveOverlapActor.Matches(otherActor))
        {
            return;
        }

        HasActiveOverlap = false;
        otherActor.SetActorLocation(location + new FVector(0.0f, 0.0f, 5.0f));
    }

    public static void OnHit(AActor otherActor, FVector normalImpulse)
    {
        otherActor.AddActorWorldOffset(normalImpulse);
    }

    public static void OnInput(InputEvent input)
    {
        if (HasPreviousInput && input.ActionId == LastInputActionId && input.TriggerEvent == LastInputTriggerEvent)
        {
            return;
        }

        HasPreviousInput = true;
        LastInputActionId = input.ActionId;
        LastInputTriggerEvent = input.TriggerEvent;
        UE.Self.SetActorLocation(input.Value + new FVector(input.ActionId, input.TriggerEvent, 0.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        if (HasAwaitedDefaultMesh)
        {
            HasAwaitedDefaultMesh = false;
        }

        UE.Self.SetActorLocation(FVector.Zero);
        UE.Self.SetActorRotation(FRotator.Zero);
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
    }
}

[AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
public sealed class AvidContinuationAttribute : Attribute
{
    public AvidContinuationAttribute(int callbackId)
    {
        CallbackId = callbackId;
    }

    public int CallbackId { get; }
}

[AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = false)]
public sealed class AvidTransientAttribute : Attribute
{
}

public enum AvidContinuationStatus
{
    Completed = 1,
    Failed = 2,
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct AvidLoadedObject
{
    internal readonly int Slot;
    internal readonly int Generation;

    internal AvidLoadedObject(int slot, int generation)
    {
        Slot = slot;
        Generation = generation;
    }

    public bool IsNull => Slot == 0 && Generation == 0;
    public bool HasHandle => Slot > 0 && Generation > 0;
    public bool IsValid => Slot > 0 && Generation > 0;
}

public readonly struct AvidContinuation
{
    private readonly long Token;

    internal AvidContinuation(long token)
    {
        Token = token;
    }

    public bool IsValid => Token != 0;
    public bool Cancel() => Native.ContinuationCancel(Token) != 0;
}

public static class AvidContinuations
{
    public static AvidContinuation Delay(float delaySeconds, int callbackId)
    {
        return new AvidContinuation(Native.ContinuationDelay(delaySeconds, callbackId));
    }

    public static AvidDelayAwaitable DelayAsync(float delaySeconds)
    {
        return default;
    }

    public static AvidContinuation NextTick(int callbackId)
    {
        return new AvidContinuation(Native.ContinuationDelay(0.0f, callbackId));
    }

    public static AvidDelayAwaitable NextTickAsync()
    {
        return default;
    }
}

public static class AvidAssets
{
    public static AvidObjectAwaitable LoadObjectAsync(string assetPath)
    {
        return default;
    }

    public static AvidContinuation LoadObjectAsync(string assetPath, int callbackId)
    {
        return new AvidContinuation(Native.ContinuationLoadObject(assetPath, callbackId));
    }
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct FVector
{
    public readonly float X;
    public readonly float Y;
    public readonly float Z;

    public FVector(float x, float y, float z)
    {
        X = x;
        Y = y;
        Z = z;
    }

    public static FVector Zero => new FVector(0.0f, 0.0f, 0.0f);
    public static FVector operator +(FVector left, FVector right)
    {
        return new FVector(left.X + right.X, left.Y + right.Y, left.Z + right.Z);
    }
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct FRotator
{
    public readonly float Pitch;
    public readonly float Yaw;
    public readonly float Roll;

    public FRotator(float pitch, float yaw, float roll)
    {
        Pitch = pitch;
        Yaw = yaw;
        Roll = roll;
    }

    public static FRotator Zero => new FRotator(0.0f, 0.0f, 0.0f);

    public static FRotator operator +(FRotator left, FRotator right)
    {
        return new FRotator(left.Pitch + right.Pitch, left.Yaw + right.Yaw, left.Roll + right.Roll);
    }
}

public readonly struct InputEvent
{
    public readonly int ActionId;
    public readonly int TriggerEvent;
    public readonly FVector Value;

    internal InputEvent(int actionId, int triggerEvent, FVector value)
    {
        ActionId = actionId;
        TriggerEvent = triggerEvent;
        Value = value;
    }
}

public readonly struct FTransform
{
    public readonly FVector Translation;
    public readonly FRotator Rotation;
    public readonly FVector Scale3D;

    public FTransform(FVector translation, FRotator rotation, FVector scale3D)
    {
        Translation = translation;
        Rotation = rotation;
        Scale3D = scale3D;
    }
}
public readonly struct AActor
{
    private readonly int Slot;
    private readonly int Generation;

    internal AActor(int slot, int generation)
    {
        Slot = slot;
        Generation = generation;
    }

    public bool IsValid => Slot > 0 && Generation > 0;

    public bool Matches(AActor other)
    {
        return Slot == other.Slot && Generation == other.Generation;
    }

    public FVector GetActorLocation()
    {
        Native.ActorGetLocation(Slot, Generation, out FVector location);
        return location;
    }

    public bool SetActorLocation(FVector location)
    {
        return Native.ActorSetLocation(Slot, Generation, location.X, location.Y, location.Z) != 0;
    }

    public bool AddActorWorldOffset(FVector delta)
    {
        return Native.ActorAddLocationOffset(Slot, Generation, delta.X, delta.Y, delta.Z) != 0;
    }

    public FRotator GetActorRotation()
    {
        Native.ActorGetRotation(Slot, Generation, out FRotator rotation);
        return rotation;
    }

    public bool SetActorRotation(FRotator rotation)
    {
        return Native.ActorSetRotation(Slot, Generation, rotation.Pitch, rotation.Yaw, rotation.Roll) != 0;
    }

    public FVector GetActorScale3D()
    {
        Native.ActorGetScale(Slot, Generation, out FVector scale);
        return scale;
    }

    public bool SetActorScale3D(FVector scale)
    {
        return Native.ActorSetScale(Slot, Generation, scale.X, scale.Y, scale.Z) != 0;
    }

    public FTransform GetActorTransform()
    {
        return new FTransform(GetActorLocation(), GetActorRotation(), GetActorScale3D());
    }

    public USceneComponent GetRootComponent()
    {
        Native.ActorGetRootComponent(Slot, Generation, out FObjectHandle handle);
        return new USceneComponent(handle.Slot, handle.Generation);
    }
}

[StructLayout(LayoutKind.Sequential)]
internal readonly struct FObjectHandle
{
    internal readonly int Slot;
    internal readonly int Generation;
}

public readonly struct USceneComponent
{
    private readonly int Slot;
    private readonly int Generation;

    internal USceneComponent(int slot, int generation)
    {
        Slot = slot;
        Generation = generation;
    }

    public bool IsValid => Slot > 0 && Generation > 0;

    public FVector GetWorldLocation()
    {
        Native.SceneComponentGetWorldLocation(Slot, Generation, out FVector location);
        return location;
    }

    public bool SetWorldLocation(FVector location)
    {
        return Native.SceneComponentSetWorldLocation(Slot, Generation, location.X, location.Y, location.Z) != 0;
    }
}

public static class UE
{
    public static AActor Self => new AActor(Native.OwnerGetSlot(), Native.OwnerGetGeneration());

    public static int SetTimer(float delaySeconds, int callbackId) => Native.TimerSetOnce(delaySeconds, callbackId);
    public static bool CancelTimer(int timerHandle) => Native.TimerCancel(timerHandle) != 0;
}

public static class Actor
{
    public static bool SetLocation(float x, float y, float z)
    {
        return UE.Self.SetActorLocation(new FVector(x, y, z));
    }

    public static bool AddLocationOffset(float x, float y, float z)
    {
        return UE.Self.AddActorWorldOffset(new FVector(x, y, z));
    }
}

public readonly struct AvidDelayAwaitable
{
    public AvidDelayAwaiter GetAwaiter() => default;
}

public readonly struct AvidDelayAwaiter : INotifyCompletion
{
    public bool IsCompleted => false;

    public void OnCompleted(Action continuation)
    {
    }

    public void GetResult()
    {
    }
}

public readonly struct AvidObjectAwaitable
{
    public AvidObjectAwaiter GetAwaiter() => default;
}

public readonly struct AvidObjectAwaiter : INotifyCompletion
{
    public bool IsCompleted => false;

    public void OnCompleted(Action continuation)
    {
    }

    public AvidLoadedObject GetResult() => default;
}

internal static class Native
{
    [DllImport("env", EntryPoint = "actor_get_location")]
    internal static extern int ActorGetLocation(int slot, int generation, out FVector location);

    [DllImport("env", EntryPoint = "actor_get_rotation")]
    internal static extern int ActorGetRotation(int slot, int generation, out FRotator rotation);

    [DllImport("env", EntryPoint = "actor_set_rotation")]
    internal static extern int ActorSetRotation(int slot, int generation, float pitch, float yaw, float roll);

    [DllImport("env", EntryPoint = "actor_get_scale")]
    internal static extern int ActorGetScale(int slot, int generation, out FVector scale);

    [DllImport("env", EntryPoint = "actor_set_scale")]
    internal static extern int ActorSetScale(int slot, int generation, float x, float y, float z);

    [DllImport("env", EntryPoint = "actor_get_root_component")]
    internal static extern int ActorGetRootComponent(int slot, int generation, out FObjectHandle handle);

    [DllImport("env", EntryPoint = "scene_component_get_world_location")]
    internal static extern int SceneComponentGetWorldLocation(int slot, int generation, out FVector location);

    [DllImport("env", EntryPoint = "scene_component_set_world_location")]
    internal static extern int SceneComponentSetWorldLocation(int slot, int generation, float x, float y, float z);

    [DllImport("env", EntryPoint = "actor_set_location")]
    internal static extern int ActorSetLocation(int slot, int generation, float x, float y, float z);

    [DllImport("env", EntryPoint = "actor_add_location_offset")]
    internal static extern int ActorAddLocationOffset(int slot, int generation, float x, float y, float z);

    [DllImport("env", EntryPoint = "owner_get_slot")]
    internal static extern int OwnerGetSlot();

    [DllImport("env", EntryPoint = "owner_get_generation")]
    internal static extern int OwnerGetGeneration();

    [DllImport("env", EntryPoint = "timer_set_once")]
    internal static extern int TimerSetOnce(float delaySeconds, int callbackId);

    [DllImport("env", EntryPoint = "timer_cancel")]
    internal static extern int TimerCancel(int timerHandle);

    [DllImport("env", EntryPoint = "continuation_delay")]
    internal static extern long ContinuationDelay(float delaySeconds, int callbackId);

    [DllImport("env", EntryPoint = "continuation_load_object")]
    internal static extern long ContinuationLoadObject(string assetPath, int callbackId);

    [DllImport("env", EntryPoint = "continuation_cancel")]
    internal static extern int ContinuationCancel(long continuationToken);

    [DllImport("env", EntryPoint = "continuation_result_read")]
    internal static extern int ContinuationResultRead(
        int bindingOrdinal,
        int resultSlot,
        int resultGeneration,
        int outputAddress,
        int byteCount);

    [DllImport("env", EntryPoint = "continuation_state_store")]
    internal static extern int ContinuationStateStore(
        long continuationToken,
        int inputAddress,
        int byteCount);

    [DllImport("env", EntryPoint = "continuation_state_read")]
    internal static extern int ContinuationStateRead(
        long continuationToken,
        int outputAddress,
        int byteCount);
}
