#pragma once

#include "CoreMinimal.h"

class UClass;
class UFunction;

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

AVIDSCRIPTBINDINGS_API const TCHAR* LexToString(EAvidScriptBindingNetworkMode Mode);
AVIDSCRIPTBINDINGS_API bool TryParseAvidScriptBindingNetworkMode(
	const FString& Value,
	EAvidScriptBindingNetworkMode& OutMode);
AVIDSCRIPTBINDINGS_API bool TryResolveAvidScriptBindingNetworkContract(
	const UFunction& Function,
	FAvidScriptBindingNetworkContract& OutContract);
AVIDSCRIPTBINDINGS_API bool IsAvidScriptBindingNetworkOwnerClass(
	const UClass* OwnerClass);
