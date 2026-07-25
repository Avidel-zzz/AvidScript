#pragma once

#include "CoreMinimal.h"

class FProperty;

class FAvidScriptEditorReflectedPropertyPolicy
{
public:
	static bool EvaluateReadable(
		const FProperty* Property,
		FString& OutCategory,
		FString& OutSource);

	static bool EvaluateWritable(
		const FProperty* Property,
		FString& OutDispatchMode,
		FString& OutWritePolicy,
		FString& OutCategory,
		FString& OutSource);
};
