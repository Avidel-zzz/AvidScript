#include "Tests/AvidScriptNetworkTopologyTestTypes.h"

#include "Net/UnrealNetwork.h"

AAvidScriptNetworkTopologyTestActor::AAvidScriptNetworkTopologyTestActor()
{
	bReplicates = true;
	bOnlyRelevantToOwner = true;
	SetNetUpdateFrequency(30.0f);
	SetMinNetUpdateFrequency(10.0f);

	ScriptComponent = CreateDefaultSubobject<UAvidScriptComponent>(
		TEXT("AvidScriptNetworkTopology"));
	ScriptComponent->SetScriptManifestPath(
		TEXT("Saved/AvidScriptCSharpGuest/Profiles/profile_network_topology/profile_network_topology.avidscript.json"));
}

void AAvidScriptNetworkTopologyTestActor::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(
		AAvidScriptNetworkTopologyTestActor,
		ReplicatedScore);
}

void AAvidScriptNetworkTopologyTestActor::ServerSubmitValue_Implementation(
	const int32 Value)
{
	++NativeServerRpcCount;
	LastNativeServerValue = Value;
}

void AAvidScriptNetworkTopologyTestActor::ServerConfirmRepNotify_Implementation(
	const int32 Value)
{
	++ClientAckCount;
	LastAckValue = Value;
}

void AAvidScriptNetworkTopologyTestActor::OnRep_ReplicatedScore()
{
	++NativeRepNotifyCount;
	LastNativeRepNotifyValue = ReplicatedScore;
}

void AAvidScriptNetworkTopologyTestActor::RecordScriptServerHandler(
	const int32 Value)
{
	++ScriptServerRpcCount;
	LastScriptServerValue = Value;
	ForceNetUpdate();
}

void AAvidScriptNetworkTopologyTestActor::RecordScriptRepNotify(
	const int32 Value)
{
	++ScriptRepNotifyCount;
	LastScriptRepNotifyValue = Value;
}
