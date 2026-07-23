#pragma once

#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptActorTransformBatch.h"

#include "CoreMinimal.h"

class AActor;

enum class EAvidScriptActorWritePolicy : uint8
{
	ReadOnly,
	AllowWrites
};

enum class EAvidScriptBindingDiagnosticsPolicy : uint8
{
	IncludeObjectPath,
	OmitObjectPath
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
		FAvidScriptActorBindingResult& OutResult,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool GetActorTransforms(
		const FAvidScriptObjectRegistry& Registry,
		TConstArrayView<FAvidScriptObjectHandle> ActorHandles,
		TArray<FAvidScriptActorTransformSnapshot>& OutTransforms,
		FAvidScriptActorTransformBatchResult& OutResult,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool GetActorLocation(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FVector& OutLocation,
		FAvidScriptActorBindingResult& OutResult,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool SetActorLocation(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		const FVector& Location,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptActorBindingResult& OutResult,
		IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool AddActorLocationOffset(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		const FVector& Offset,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptActorBindingResult& OutResult,
		IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool GetActorRotation(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FRotator& OutRotation,
		FAvidScriptActorBindingResult& OutResult,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool SetActorRotation(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		const FRotator& Rotation,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptActorBindingResult& OutResult,
		IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool GetActorScale3D(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FVector& OutScale3D,
		FAvidScriptActorBindingResult& OutResult,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool SetActorScale3D(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		const FVector& Scale3D,
		EAvidScriptActorWritePolicy WritePolicy,
		FAvidScriptActorBindingResult& OutResult,
		IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

	static bool GetRootComponentHandle(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FAvidScriptObjectHandle& OutComponentHandle,
		FAvidScriptActorBindingResult& OutResult,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy = EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);

private:
	static AActor* ResolveActor(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		FAvidScriptActorBindingResult& OutResult,
		EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy);
	static bool PrepareTransformWrite(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ActorHandle,
		AActor& Actor,
		IAvidScriptBindingHostEffectJournal* HostEffectJournal,
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
