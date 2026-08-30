using System.Runtime.InteropServices;

namespace AvidScript;

public static class NetworkTopologyScript
{
    private const int ExpectedValue = 41;
    private const int SubmitFromClient = 5705;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        AAvidScriptNetworkTopologyTestActor self = UE.Self;
        AActor actor = self;
        if (actor.HasAuthority())
        {
            return;
        }

        AvidContinuations.NextTick(SubmitFromClient);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
    }

    [AvidContinuation(SubmitFromClient)]
    public static void SubmitValueFromClient()
    {
        UE.Self.ServerSubmitValue(ExpectedValue);
    }

    [AvidEvent(AvidEvents.ServerSubmitValue)]
    public static void HandleServerSubmitValue(int value)
    {
        AAvidScriptNetworkTopologyTestActor self = UE.Self;
        self.ReplicatedScore = value;
        self.RecordScriptServerHandler(value);
    }

    [AvidEvent(AvidEvents.OnRep_ReplicatedScore)]
    public static void HandleReplicatedScoreChanged()
    {
        AAvidScriptNetworkTopologyTestActor self = UE.Self;
        int value = self.ReplicatedScore;
        self.RecordScriptRepNotify(value);
        self.ServerConfirmRepNotify(value);
    }
}
