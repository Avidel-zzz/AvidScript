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
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        ElapsedSeconds += deltaSeconds;
        FVector currentLocation = UE.Self.GetActorLocation();
        UE.Self.SetActorLocation(currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        UE.Self.SetActorLocation(FVector.Zero);
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

    [DllImport("env", EntryPoint = "actor_set_location")]
    internal static extern int ActorSetLocation(int slot, int generation, float x, float y, float z);

    [DllImport("env", EntryPoint = "actor_add_location_offset")]
    internal static extern int ActorAddLocationOffset(int slot, int generation, float x, float y, float z);

    [DllImport("env", EntryPoint = "owner_get_slot")]
    internal static extern int OwnerGetSlot();

    [DllImport("env", EntryPoint = "owner_get_generation")]
    internal static extern int OwnerGetGeneration();
}