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
};
