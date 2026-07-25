#pragma once

#include "AvidScriptBindingDescriptor.h"
#include "CoreMinimal.h"

struct FAvidScriptEditorCSharpObjectFactorySurface
{
	const FAvidScriptBindingObjectFactoryModel* Factory = nullptr;
	const FAvidScriptBindingTypeModel* ResultType = nullptr;
	const FAvidScriptBindingTypeModel* OuterType = nullptr;
	FString PropertyName;
	FString FactoryTokenName;
	FString TypeTokenName;
};

class FAvidScriptEditorCSharpObjectFactoryRenderer
{
public:
	static bool ValidateBindingContract(
		FString& OutErrorCategory,
		FString& OutErrorSource);

	static bool BuildSurfaces(
		const FAvidScriptBindingPackageModel& Package,
		TSet<FString>& InOutCSharpTypeNames,
		TArray<FAvidScriptEditorCSharpObjectFactorySurface>& OutSurfaces,
		FString& OutErrorCategory,
		FString& OutErrorSource);

	static void AppendCapabilityTokens(
		const TArray<FAvidScriptEditorCSharpObjectFactorySurface>& Surfaces,
		TArray<FString>& Lines);

	static void AppendFacadeMethods(
		const TArray<FAvidScriptEditorCSharpObjectFactorySurface>& Surfaces,
		TArray<FString>& Lines);

	static void AppendNativeImports(
		bool bNeedsLeadingBlank,
		TArray<FString>& Lines);
};
