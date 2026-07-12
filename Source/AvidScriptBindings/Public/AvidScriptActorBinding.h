#pragma once

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptActorTransformBatch.h"

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
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale3D = FVector::OneVector;
	FAvidScriptObjectHandleResult ObjectResult;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptActorBinding
{
public:
	static bool GetActorTransform(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FAvidScriptActorTransformSnapshot& OutTransform,
		FAvidScriptActorBindingResult& OutResult);

	static bool GetActorTransforms(
		const FAvidScriptObjectRegistry& Registry,
		TConstArrayView<FAvidScriptObjectHandle> ActorHandles,
		TArray<FAvidScriptActorTransformSnapshot>& OutTransforms,
		FAvidScriptActorTransformBatchResult& OutResult);

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

	static bool GetActorRotation(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FRotator& OutRotation,
		FAvidScriptActorBindingResult& OutResult);

	static bool SetActorRotation(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		const FRotator& Rotation,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptActorBindingResult& OutResult);

	static bool GetActorScale3D(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FVector& OutScale3D,
		FAvidScriptActorBindingResult& OutResult);

	static bool SetActorScale3D(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		const FVector& Scale3D,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptActorBindingResult& OutResult);

	static bool GetRootComponentHandle(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FAvidScriptObjectHandle& OutComponentHandle,
		FAvidScriptActorBindingResult& OutResult);

private:
	static AActor* ResolveActor(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FAvidScriptActorBindingResult& OutResult);

	static void SetSuccess(
		FAvidScriptActorBindingResult& OutResult,
		const FAvidScriptObjectHandleResult& ObjectResult,
		const FVector& Location,
		const FRotator& Rotation,
		const FVector& Scale3D);

	static void SetFailure(
		FAvidScriptActorBindingResult& OutResult,
		const FAvidScriptObjectHandleResult& ObjectResult,
		const FString& ErrorCategory,
		const FString& NextAction);
};
