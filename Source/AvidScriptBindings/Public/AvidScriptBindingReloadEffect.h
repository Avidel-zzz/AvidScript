#pragma once

#include "CoreMinimal.h"

class FAvidScriptObjectRegistry;
class FProperty;
struct FAvidScriptObjectHandle;

enum class EAvidScriptBindingReloadEffect : uint8
{
	None,
	ActorTransform,
	SceneComponentTransform,
	ReflectedProperty,
	ContinuationProducer,
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

	virtual bool PrepareReflectedProperty(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		FProperty& Property,
		FAvidScriptBindingHostEffectPrepareResult& OutResult)
	{
		OutResult = FAvidScriptBindingHostEffectPrepareResult();
		OutResult.ErrorCategory = TEXT("binding_reload_effect_unsupported");
		OutResult.ErrorSource = Target.GetPathName();
		OutResult.ErrorDetails = TEXT("The host effect journal has no reflected property adapter.");
		return false;
	}
};

AVIDSCRIPTBINDINGS_API const TCHAR* LexToString(EAvidScriptBindingReloadEffect Effect);
AVIDSCRIPTBINDINGS_API bool TryParseAvidScriptBindingReloadEffect(
	const FString& Value,
	EAvidScriptBindingReloadEffect& OutEffect);
