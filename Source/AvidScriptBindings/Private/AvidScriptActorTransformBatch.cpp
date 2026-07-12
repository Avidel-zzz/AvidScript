#include "AvidScriptActorTransformBatch.h"

#include "AvidScriptActorBinding.h"

bool FAvidScriptActorBinding::GetActorTransforms(
	const FAvidScriptObjectRegistry& Registry,
	TConstArrayView<FAvidScriptObjectHandle> ActorHandles,
	TArray<FAvidScriptActorTransformSnapshot>& OutTransforms,
	FAvidScriptActorTransformBatchResult& OutResult)
{
	OutTransforms.Reset();
	OutResult = FAvidScriptActorTransformBatchResult();

	if (ActorHandles.Num() > AvidScriptMaximumActorTransformBatchSize)
	{
		OutResult.ErrorCategory = TEXT("batch_too_large");
		OutResult.NextAction = FString::Printf(
			TEXT("Split the request into batches of at most %d actor handles."),
			AvidScriptMaximumActorTransformBatchSize);
		OutResult.ErrorMessage = FString::Printf(
			TEXT("AvidScript actor transform batch error | category=batch_too_large | count=%d | max=%d"),
			ActorHandles.Num(),
			AvidScriptMaximumActorTransformBatchSize);
		return false;
	}

	OutTransforms.Reserve(ActorHandles.Num());
	for (int32 Index = 0; Index < ActorHandles.Num(); ++Index)
	{
		FAvidScriptActorTransformSnapshot Snapshot;
		FAvidScriptActorBindingResult ItemResult;
		if (!GetActorTransform(Registry, ActorHandles[Index], Snapshot, ItemResult))
		{
			OutResult.ProcessedCount = Index;
			OutResult.FailedIndex = Index;
			OutResult.ErrorCategory = ItemResult.ErrorCategory;
			OutResult.NextAction = ItemResult.NextAction;
			OutResult.ErrorMessage = FString::Printf(
				TEXT("AvidScript actor transform batch error | index=%d | %s"),
				Index,
				ItemResult.ErrorMessage.IsEmpty() ? TEXT("actor transform read failed") : *ItemResult.ErrorMessage);
			OutTransforms.Reset();
			return false;
		}
		OutTransforms.Add(MoveTemp(Snapshot));
	}

	OutResult.bSucceeded = true;
	OutResult.ProcessedCount = OutTransforms.Num();
	return true;
}
