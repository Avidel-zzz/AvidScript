#pragma once

#include "CoreMinimal.h"

class UClass;
class UFunction;
class FProperty;

enum class EAvidScriptBindingNetworkMode : uint8
{
	None,
	Server,
	Client,
	Multicast
};

struct FAvidScriptBindingNetworkContract
{
	EAvidScriptBindingNetworkMode Mode = EAvidScriptBindingNetworkMode::None;
	bool bReliable = false;

	bool IsNetworked() const
	{
		return Mode != EAvidScriptBindingNetworkMode::None;
	}

	bool operator==(const FAvidScriptBindingNetworkContract&) const = default;
};

enum class EAvidScriptBindingPropertyReplicationMode : uint8
{
	None,
	Replicated,
	RepNotify
};

struct FAvidScriptBindingPropertyReplicationContract
{
	EAvidScriptBindingPropertyReplicationMode Mode =
		EAvidScriptBindingPropertyReplicationMode::None;
	FName RepNotifyFunction;

	bool IsReplicated() const
	{
		return Mode != EAvidScriptBindingPropertyReplicationMode::None;
	}

	bool operator==(
		const FAvidScriptBindingPropertyReplicationContract&) const = default;
};

AVIDSCRIPTBINDINGS_API const TCHAR* LexToString(EAvidScriptBindingNetworkMode Mode);
AVIDSCRIPTBINDINGS_API bool TryParseAvidScriptBindingNetworkMode(
	const FString& Value,
	EAvidScriptBindingNetworkMode& OutMode);
AVIDSCRIPTBINDINGS_API bool TryResolveAvidScriptBindingNetworkContract(
	const UFunction& Function,
	FAvidScriptBindingNetworkContract& OutContract);
AVIDSCRIPTBINDINGS_API bool IsAvidScriptBindingNetworkOwnerClass(
	const UClass* OwnerClass);
AVIDSCRIPTBINDINGS_API const TCHAR* LexToString(
	EAvidScriptBindingPropertyReplicationMode Mode);
AVIDSCRIPTBINDINGS_API bool TryParseAvidScriptBindingPropertyReplicationMode(
	const FString& Value,
	EAvidScriptBindingPropertyReplicationMode& OutMode);
AVIDSCRIPTBINDINGS_API bool TryResolveAvidScriptBindingPropertyReplicationContract(
	const FProperty& Property,
	FAvidScriptBindingPropertyReplicationContract& OutContract);
