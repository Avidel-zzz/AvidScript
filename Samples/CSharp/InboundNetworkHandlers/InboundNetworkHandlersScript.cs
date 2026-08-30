using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class InboundNetworkHandlersScript
{
    [AvidPersist]
    private static int ServerRpcCount;

    [AvidPersist]
    private static int RepNotifyCount;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        ServerRpcCount = 0;
        RepNotifyCount = 0;
    }

    [AvidEvent(AvidEvents.ServerSubmitValue)]
    public static void HandleServerSubmitValue(int value)
    {
        ++ServerRpcCount;
        AAvidScriptBindingRuntimeNetworkTestActor self = UE.Self;
        self.ReplicatedScore = value;
    }

    [AvidEvent(AvidEvents.OnRep_ReplicatedScore)]
    public static void HandleReplicatedScoreChanged()
    {
        ++RepNotifyCount;
    }
}
