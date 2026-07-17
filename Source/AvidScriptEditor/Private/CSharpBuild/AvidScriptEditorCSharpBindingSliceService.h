#pragma once

#include "CoreMinimal.h"

struct FAvidScriptCSharpBindingEmitResult;
struct FAvidScriptFrontendBindingPackage;

struct FAvidScriptEditorCSharpBindingSliceResult
{
	bool bSucceeded = false;
	int32 RequestedBindingCount = 0;
	FString ErrorCategory;
	FString ErrorSource;
	FString NextAction;
	FString ErrorMessage;
};

class FAvidScriptEditorCSharpBindingSliceService
{
public:
	static bool Publish(
		const FString& AuthorizationDescriptorPath,
		const FAvidScriptFrontendBindingPackage& Provenance,
		const FString& OutputRoot,
		FAvidScriptCSharpBindingEmitResult& OutPackage,
		FAvidScriptEditorCSharpBindingSliceResult& OutResult);
};
