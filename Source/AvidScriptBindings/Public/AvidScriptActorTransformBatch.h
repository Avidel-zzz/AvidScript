#pragma once

#include "AvidScriptObjectRegistry.h"

#include "CoreMinimal.h"

inline constexpr int32 AvidScriptMaximumActorTransformBatchSize = 256;

struct FAvidScriptActorTransformSnapshot
{
	FAvidScriptObjectHandle Handle;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale3D = FVector::OneVector;
};

struct FAvidScriptActorTransformBatchResult
{
	bool bSucceeded = false;
	int32 ProcessedCount = 0;
	int32 FailedIndex = INDEX_NONE;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
};