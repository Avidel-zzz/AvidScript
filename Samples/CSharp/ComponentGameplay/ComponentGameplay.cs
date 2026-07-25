using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class ComponentGameplay
{
    [AvidTransient]
    private static UAvidScriptTypedTestSceneComponent DynamicComponent;

    [AvidTransient]
    private static UAvidScriptTypedTestSceneComponent ExistingComponent;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        AAvidScriptTypedTestActor self = UE.Self;
        UAvidScriptTypedTestSceneComponent existing =
            UE.FindComponent(self, ProjectTypes.GameplayComponent);
        UAvidScriptTypedTestSceneComponent created =
            UE.CreateComponent(self, ProjectFactories.GameplayComponent);

        ExistingComponent = existing;
        DynamicComponent = created;
        UE.AttachTo(
            created,
            existing,
            UE.AttachmentRule.KeepRelative,
            false);
        self.ApplyGameplayValue(1.0f);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        UAvidScriptTypedTestSceneComponent component = DynamicComponent;
        if (!component.IsValid)
        {
            return;
        }

        component.ApplyGameplayPulse(deltaSeconds);
        UE.Self.ApplyGameplayValue(2.0f);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        UAvidScriptTypedTestSceneComponent component = DynamicComponent;
        DynamicComponent = default;
        ExistingComponent = default;
        if (!component.IsValid)
        {
            return;
        }

        UE.Detach(component, UE.DetachmentRule.KeepWorld);
        UE.Release(component);
    }
}
