#pragma once

#include "AvidScriptObjectRegistry.h"

#include "CoreMinimal.h"

class AActor;

enum class EAvidScriptActorWritePolicy : uint8
{
	ReadOnly,
	AllowWrites
};

struct FAvidScriptActorBindingResult
{
	bool bSucceeded = false;
	FAvidScriptObjectHandle Handle;
	FString ObjectPath;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
	FVector Location = FVector::ZeroVector;
	FAvidScriptObjectHandleResult ObjectResult;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptActorBinding
{
public:
	static bool GetActorLocation(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FVector& OutLocation,
		FAvidScriptActorBindingResult& OutResult);

	static bool SetActorLocation(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		const FVector& Location,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptActorBindingResult& OutResult);

	static bool AddActorLocationOffset(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		const FVector& Offset,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptActorBindingResult& OutResult);

private:
	static AActor* ResolveActor(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FAvidScriptActorBindingResult& OutResult);

	static void SetSuccess(
		FAvidScriptActorBindingResult& OutResult,
		const FAvidScriptObjectHandleResult& ObjectResult,
		const FVector& Location);

	static void SetFailure(
		FAvidScriptActorBindingResult& OutResult,
		const FAvidScriptObjectHandleResult& ObjectResult,
		const FString& ErrorCategory,
		const FString& NextAction);
};
