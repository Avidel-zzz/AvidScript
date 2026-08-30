using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class ReplicatedPropertyScript
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        AAvidScriptBindingRuntimeNetworkTestActor self = UE.Self;
        AActor actor = self;
        if (!actor.HasAuthority())
        {
            return;
        }

        self.ReplicatedScore = self.ReplicatedScore + 10;
        self.ReplicatedRoutedValue = 20;
    }
}
