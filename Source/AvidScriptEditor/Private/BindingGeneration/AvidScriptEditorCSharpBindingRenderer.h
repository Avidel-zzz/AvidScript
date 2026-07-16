#pragma once

#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "CoreMinimal.h"

class FAvidScriptEditorCSharpBindingRenderer
{
public:
	static bool EmitReferenceSource(
		const FAvidScriptBindingPackageModel& Package,
		const FString& DescriptorHash,
		FString& OutSource,
		FString& OutErrorCategory,
		FString& OutErrorSource);

	static bool EmitManifest(
		const FAvidScriptBindingPackageModel& Package,
		const FString& DescriptorHash,
		const FString& SourceHash,
		FString& OutManifest);
};
