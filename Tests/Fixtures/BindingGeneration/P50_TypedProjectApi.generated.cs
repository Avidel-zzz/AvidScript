[StructLayout(LayoutKind.Sequential)]
public readonly struct TSubclassOfAActor
{
    private readonly int Ordinal;

    internal TSubclassOfAActor(int ordinal)
    {
        Ordinal = ordinal;
    }

    internal int AvidScriptOrdinal => Ordinal;
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct TSubclassOfAStaticMeshActor
{
    private readonly int Ordinal;

    internal TSubclassOfAStaticMeshActor(int ordinal)
    {
        Ordinal = ordinal;
    }

    internal int AvidScriptOrdinal => Ordinal;
}

public static class ProjectClasses
{
    public static TSubclassOfAStaticMeshActor ProjectileClass => new(0);
}

[StructLayout(LayoutKind.Sequential)]
internal readonly struct FAvidScriptObjectHandle
{
    internal readonly int Slot;
    internal readonly int Generation;

    internal FAvidScriptObjectHandle(int slot, int generation)
    {
        Slot = slot;
        Generation = generation;
    }
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct UObject
{
    private readonly int Slot;
    private readonly int Generation;

    internal UObject(int slot, int generation)
    {
        Slot = slot;
        Generation = generation;
    }

    internal int AvidScriptSlot => Slot;
    internal int AvidScriptGeneration => Generation;
    public bool IsNull => Slot == 0 && Generation == 0;
    public bool HasHandle => Slot > 0 && Generation > 0;
    public bool IsValid => Slot > 0 && Generation > 0;
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct AActor
{
    private readonly int Slot;
    private readonly int Generation;

    internal AActor(int slot, int generation)
    {
        Slot = slot;
        Generation = generation;
    }

    internal int AvidScriptSlot => Slot;
    internal int AvidScriptGeneration => Generation;
    public bool IsNull => Slot == 0 && Generation == 0;
    public bool HasHandle => Slot > 0 && Generation > 0;
    public bool IsValid => Slot > 0 && Generation > 0;

    public static implicit operator UObject(AActor value)
    {
        return new(value.Slot, value.Generation);
    }

    public UObject TryCast()
    {
        if (AvidScriptNative.ObjectTypeIsA(Slot, Generation, 0) != 0)
        {
            return new(Slot, Generation);
        }
        return default;
    }
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct AStaticMeshActor
{
    private readonly int Slot;
    private readonly int Generation;

    internal AStaticMeshActor(int slot, int generation)
    {
        Slot = slot;
        Generation = generation;
    }

    internal int AvidScriptSlot => Slot;
    internal int AvidScriptGeneration => Generation;
    public bool IsNull => Slot == 0 && Generation == 0;
    public bool HasHandle => Slot > 0 && Generation > 0;
    public bool IsValid => Slot > 0 && Generation > 0;

    public static implicit operator AActor(AStaticMeshActor value)
    {
        return new(value.Slot, value.Generation);
    }

    public AActor TryCast()
    {
        if (AvidScriptNative.ObjectTypeIsA(Slot, Generation, 1) != 0)
        {
            return new(Slot, Generation);
        }
        return default;
    }
}

public static class UE
{
    public static AStaticMeshActor Self
    {
        get
        {
            long packedHandle = OwnerGetHandle();
            return new((int)packedHandle, (int)(packedHandle >> 32));
        }
    }
    public static int SetTimer(float delaySeconds, int callbackId) => AvidScriptRuntimeNative.TimerSetOnce(delaySeconds, callbackId);
    public static bool CancelTimer(int timerHandle) => AvidScriptRuntimeNative.TimerCancel(timerHandle) != 0;

    public static AStaticMeshActor SpawnActor(TSubclassOfAStaticMeshActor actorClass, FTransform transform)
    {
        AvidScriptNative.Invoke0000(
            actorClass.AvidScriptOrdinal, in transform,
            out FAvidScriptObjectHandle actorHandle);
        return new(actorHandle.Slot, actorHandle.Generation);
    }

    public static AActor SpawnActor(TSubclassOfAActor actorClass, FTransform transform)
    {
        AvidScriptNative.Invoke0000(
            actorClass.AvidScriptOrdinal, in transform,
            out FAvidScriptObjectHandle actorHandle);
        return new(actorHandle.Slot, actorHandle.Generation);
    }

    public static bool DestroyActor(AActor actor)
        => AvidScriptNative.Invoke0001(actor.AvidScriptSlot, actor.AvidScriptGeneration) != 0;

    public static bool IsA(AActor actor, TSubclassOfAActor actorClass)
        => AvidScriptNative.Invoke0002(
            actor.AvidScriptSlot, actor.AvidScriptGeneration,
            actorClass.AvidScriptOrdinal) != 0;

    [DllImport("avidscript", EntryPoint = "avid_owner_get_handle")]
    private static extern long OwnerGetHandle();
}

internal static class AvidScriptRuntimeNative
{
    [DllImport("env", EntryPoint = "timer_set_once")]
    internal static extern int TimerSetOnce(float delaySeconds, int callbackId);

    [DllImport("env", EntryPoint = "timer_cancel")]
    internal static extern int TimerCancel(int timerHandle);
}

internal static class AvidScriptNative
{
    [DllImport("avidscript", EntryPoint = "avid_object_spawn_actor")]
    internal static extern int Invoke0000(int classOrdinal, in FTransform transform, out FAvidScriptObjectHandle actorHandle);

    [DllImport("avidscript", EntryPoint = "avid_object_destroy_actor")]
    internal static extern int Invoke0001(int slot, int generation);

    [DllImport("avidscript", EntryPoint = "avid_object_is_a")]
    internal static extern int Invoke0002(int slot, int generation, int classOrdinal);

    [DllImport("avidscript", EntryPoint = "avid_object_type_is_a")]
    internal static extern int ObjectTypeIsA(int slot, int generation, int targetOrdinal);
}
