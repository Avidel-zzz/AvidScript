#pragma once

#include "CoreMinimal.h"

class FAvidScriptObjectRegistry;
struct FAvidScriptObjectHandle;

enum class EAvidScriptBindingReloadEffect : uint8
{
	None,
	ActorTransform,
	SceneComponentTransform,
	Unsupported
};

struct FAvidScriptBindingHostEffectPrepareResult
{
	bool bSucceeded = false;
	FString ErrorCategory;
	FString ErrorSource;
	FString ErrorDetails;
};

class AVIDSCRIPTBINDINGS_API IAvidScriptBindingHostEffectJournal
{
public:
	virtual ~IAvidScriptBindingHostEffectJournal() = default;

	virtual bool PrepareEffect(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		EAvidScriptBindingReloadEffect Effect,
		FAvidScriptBindingHostEffectPrepareResult& OutResult) = 0;
};

AVIDSCRIPTBINDINGS_API const TCHAR* LexToString(EAvidScriptBindingReloadEffect Effect);
AVIDSCRIPTBINDINGS_API bool TryParseAvidScriptBindingReloadEffect(
	const FString& Value,
	EAvidScriptBindingReloadEffect& OutEffect);
