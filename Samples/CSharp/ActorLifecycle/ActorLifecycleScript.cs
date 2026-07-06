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
        Actor.SetLocation(100.0f + 120.0f * deltaSeconds, 200.0f, 300.0f);
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

    [DllImport("env", EntryPoint = "actor_set_location")]
    private static extern int ActorSetLocation(int slot, int generation, float x, float y, float z);
}
