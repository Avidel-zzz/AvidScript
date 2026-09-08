#pragma once

#include "AvidScriptVmArtifact.h"
#include "CoreMinimal.h"

class FJsonObject;

struct FAvidScriptEditorCooperativeSafepointMetadata
{
	bool bEnabled = false;
	FString ReceiptPath;
	FString ReceiptSha256;
	FAvidScriptVmCooperativeSafepointReceipt VmReceipt;
};

bool LoadAvidScriptEditorCooperativeSafepointMetadata(
	const TSharedRef<FJsonObject>& ManifestObject,
	const FString& OutputRoot,
	const FString& CanonicalWasmSha256,
	FAvidScriptEditorCooperativeSafepointMetadata& OutMetadata,
	FString& OutErrorCategory,
	FString& OutError);
