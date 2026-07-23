using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class TypedProjectApiScript
{
    [AvidTransient]
    private static AAvidScriptTypedTestProjectile Projectile;

    [AvidTransient]
    private static int TickCount;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        AAvidScriptTypedTestActor self = UE.Self;
        self.ApplyGameplayValue(4.0f);

        AAvidScriptTypedTestProjectile projectile =
            UE.SpawnActor(ProjectClasses.Projectile, FTransform.Identity);
        Projectile = projectile;
        TickCount = 0;
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        AAvidScriptTypedTestProjectile projectile = Projectile;
        if (!projectile.IsValid)
        {
            return;
        }

        // Object wrappers expose only direct-base conversions, so inheritance
        // traversal stays explicit while every upcast remains guest-only.
        AAvidScriptTypedTestActor typedActor = projectile;
        AActor actor = typedActor;
        AAvidScriptTypedTestActor checkedActor =
            AAvidScriptTypedTestActor.TryCast(actor);
        if (!checkedActor.IsValid)
        {
            return;
        }

        AAvidScriptTypedTestProjectile checkedProjectile =
            AAvidScriptTypedTestProjectile.TryCast(checkedActor);
        if (!checkedProjectile.IsValid)
        {
            return;
        }
        checkedProjectile.ActivateProjectile();

        AAvidScriptTypedTestActor self = UE.Self;
        AActor selfActor = self;
        AAvidScriptTypedTestActor checkedSelf =
            AAvidScriptTypedTestActor.TryCast(selfActor);
        AAvidScriptTypedTestProjectile mismatch =
            AAvidScriptTypedTestProjectile.TryCast(checkedSelf);
        if (!mismatch.IsValid)
        {
            self.ApplyGameplayValue(2.0f);
        }

        int nextTickCount = TickCount + 1;
        TickCount = nextTickCount;
        if (nextTickCount >= 2)
        {
            UE.DestroyActor(actor);
            Projectile = default;
            TickCount = 0;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
    }
}
