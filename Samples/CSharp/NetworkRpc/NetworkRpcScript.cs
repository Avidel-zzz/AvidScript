using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class NetworkRpcScript
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        AAvidScriptBindingRuntimeNetworkTestActor self = UE.Self;
        AActor actor = self;
        if (actor.HasAuthority())
        {
            self.ClientApplyValue(20);
            self.MulticastAnnounceValue(30);
            return;
        }

        self.ServerSubmitValue(10);
    }
}
