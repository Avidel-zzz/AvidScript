using System.Runtime.InteropServices;

namespace AvidScript;

public static class ActorLifecycleScript
{
    public static int Main() => 0;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        Actor.SetLocation(100.0f, 200.0f, 300.0f);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        Actor.AddLocationOffset(120.0f * deltaSeconds, 0.0f, 0.0f);
    }
}

public static class Actor
{
    private const int OwnerSlot = 1;
    private const int OwnerGeneration = 1;

    public static bool SetLocation(float x, float y, float z)
    {
        return ActorSetLocation(OwnerSlot, OwnerGeneration, x, y, z) != 0;
    }

    public static bool AddLocationOffset(float x, float y, float z)
    {
        return ActorAddLocationOffset(OwnerSlot, OwnerGeneration, x, y, z) != 0;
    }

    [DllImport("env", EntryPoint = "actor_set_location")]
    private static extern int ActorSetLocation(int slot, int generation, float x, float y, float z);

    [DllImport("env", EntryPoint = "actor_add_location_offset")]
    private static extern int ActorAddLocationOffset(int slot, int generation, float x, float y, float z);
}
