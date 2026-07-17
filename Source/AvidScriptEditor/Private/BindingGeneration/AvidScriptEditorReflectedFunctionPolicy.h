#pragma once

#include "CoreMinimal.h"

class UFunction;

class FAvidScriptEditorReflectedFunctionPolicy
{
public:
	static bool Evaluate(
		const UFunction* Function,
		FString& OutCategory,
		FString& OutSource);
};
