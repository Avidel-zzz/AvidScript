using System.Runtime.InteropServices;

namespace AvidScript;

public static class ActorLifecycleScript
{
    private static float ElapsedSeconds;

    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        ElapsedSeconds = 0.0f;
        UE.Self.SetActorLocation(new FVector(100.0f, 200.0f, 300.0f));
        UE.Self.AddActorWorldOffset(new FVector(0.0f, 0.0f, 0.0f));
        UE.Self.SetActorRotation(FRotator.Zero);
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
        FVector rootLocation = UE.Self.GetRootComponent().GetWorldLocation();
        UE.Self.GetRootComponent().SetWorldLocation(rootLocation);
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

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        UE.Self.SetActorLocation(FVector.Zero);
        UE.Self.SetActorRotation(FRotator.Zero);
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
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
}