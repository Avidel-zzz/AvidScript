#pragma once

#include "AvidScriptActorBinding.h"
#include "AvidScriptObjectRegistry.h"

#include "CoreMinimal.h"

class USceneComponent;

struct FAvidScriptSceneComponentBindingResult
{
	bool bSucceeded = false;
	FAvidScriptObjectHandle Handle;
	FString ObjectPath;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
	FVector WorldLocation = FVector::ZeroVector;
	FAvidScriptObjectHandleResult ObjectResult;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptSceneComponentBinding
{
public:
	static bool GetWorldLocation(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ComponentHandle,
		FVector& OutWorldLocation,
		FAvidScriptSceneComponentBindingResult& OutResult);

	static bool SetWorldLocation(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ComponentHandle,
		const FVector& WorldLocation,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptSceneComponentBindingResult& OutResult);

private:
	static USceneComponent* ResolveSceneComponent(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ComponentHandle,
		FAvidScriptSceneComponentBindingResult& OutResult);

	static void SetSuccess(
		FAvidScriptSceneComponentBindingResult& OutResult,
		const FAvidScriptObjectHandleResult& ObjectResult,
		const FVector& WorldLocation);

	static void SetFailure(
		FAvidScriptSceneComponentBindingResult& OutResult,
		const FAvidScriptObjectHandleResult& ObjectResult,
		const FString& ErrorCategory,
		const FString& NextAction);
};
