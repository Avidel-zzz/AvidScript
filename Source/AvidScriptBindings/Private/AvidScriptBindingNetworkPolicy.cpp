#include "AvidScriptBindingNetworkPolicy.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

const TCHAR* LexToString(const EAvidScriptBindingNetworkMode Mode)
{
	switch (Mode)
	{
	case EAvidScriptBindingNetworkMode::None:
		return TEXT("none");
	case EAvidScriptBindingNetworkMode::Server:
		return TEXT("server");
	case EAvidScriptBindingNetworkMode::Client:
		return TEXT("client");
	case EAvidScriptBindingNetworkMode::Multicast:
		return TEXT("multicast");
	default:
		return TEXT("invalid");
	}
}

bool TryParseAvidScriptBindingNetworkMode(
	const FString& Value,
	EAvidScriptBindingNetworkMode& OutMode)
{
	if (Value == TEXT("none"))
	{
		OutMode = EAvidScriptBindingNetworkMode::None;
		return true;
	}
	if (Value == TEXT("server"))
	{
		OutMode = EAvidScriptBindingNetworkMode::Server;
		return true;
	}
	if (Value == TEXT("client"))
	{
		OutMode = EAvidScriptBindingNetworkMode::Client;
		return true;
	}
	if (Value == TEXT("multicast"))
	{
		OutMode = EAvidScriptBindingNetworkMode::Multicast;
		return true;
	}
	return false;
}

bool TryResolveAvidScriptBindingNetworkContract(
	const UFunction& Function,
	FAvidScriptBindingNetworkContract& OutContract)
{
	OutContract = {};
	const bool bServer = Function.HasAnyFunctionFlags(FUNC_NetServer);
	const bool bClient = Function.HasAnyFunctionFlags(FUNC_NetClient);
	const bool bMulticast = Function.HasAnyFunctionFlags(FUNC_NetMulticast);
	const int32 DirectionCount = static_cast<int32>(bServer)
		+ static_cast<int32>(bClient)
		+ static_cast<int32>(bMulticast);
	const bool bNetworked = Function.HasAnyFunctionFlags(FUNC_Net);
	const bool bReliable = Function.HasAnyFunctionFlags(FUNC_NetReliable);
	if (!bNetworked)
	{
		return DirectionCount == 0 && !bReliable;
	}
	if (DirectionCount != 1)
	{
		return false;
	}

	OutContract.Mode = bServer
		? EAvidScriptBindingNetworkMode::Server
		: bClient
			? EAvidScriptBindingNetworkMode::Client
			: EAvidScriptBindingNetworkMode::Multicast;
	OutContract.bReliable = bReliable;
	return true;
}

bool IsAvidScriptBindingNetworkOwnerClass(const UClass* OwnerClass)
{
	return OwnerClass != nullptr
		&& (OwnerClass->IsChildOf(AActor::StaticClass())
			|| OwnerClass->IsChildOf(UActorComponent::StaticClass()));
}

const TCHAR* LexToString(
	const EAvidScriptBindingPropertyReplicationMode Mode)
{
	switch (Mode)
	{
	case EAvidScriptBindingPropertyReplicationMode::None:
		return TEXT("none");
	case EAvidScriptBindingPropertyReplicationMode::Replicated:
		return TEXT("replicated");
	case EAvidScriptBindingPropertyReplicationMode::RepNotify:
		return TEXT("rep_notify");
	default:
		return TEXT("invalid");
	}
}

bool TryParseAvidScriptBindingPropertyReplicationMode(
	const FString& Value,
	EAvidScriptBindingPropertyReplicationMode& OutMode)
{
	if (Value == TEXT("none"))
	{
		OutMode = EAvidScriptBindingPropertyReplicationMode::None;
		return true;
	}
	if (Value == TEXT("replicated"))
	{
		OutMode = EAvidScriptBindingPropertyReplicationMode::Replicated;
		return true;
	}
	if (Value == TEXT("rep_notify"))
	{
		OutMode = EAvidScriptBindingPropertyReplicationMode::RepNotify;
		return true;
	}
	return false;
}

bool TryResolveAvidScriptBindingPropertyReplicationContract(
	const FProperty& Property,
	FAvidScriptBindingPropertyReplicationContract& OutContract)
{
	OutContract = {};
	const bool bReplicated = Property.HasAnyPropertyFlags(CPF_Net);
	const bool bRepNotify = Property.HasAnyPropertyFlags(CPF_RepNotify);
	if (!bReplicated)
	{
		return !bRepNotify && Property.RepNotifyFunc.IsNone();
	}
	UClass* OwnerClass = Cast<UClass>(Property.GetOwnerStruct());
	if (OwnerClass != nullptr)
	{
		OwnerClass->SetUpRuntimeReplicationData();
	}
	if (OwnerClass == nullptr
		|| Property.RepIndex == INDEX_NONE
		|| !OwnerClass->ClassReps.IsValidIndex(Property.RepIndex)
		|| OwnerClass->ClassReps[Property.RepIndex].Property != &Property)
	{
		return false;
	}
	if (bRepNotify != !Property.RepNotifyFunc.IsNone())
	{
		return false;
	}

	OutContract.Mode = bRepNotify
		? EAvidScriptBindingPropertyReplicationMode::RepNotify
		: EAvidScriptBindingPropertyReplicationMode::Replicated;
	OutContract.RepNotifyFunction = Property.RepNotifyFunc;
	return true;
}
