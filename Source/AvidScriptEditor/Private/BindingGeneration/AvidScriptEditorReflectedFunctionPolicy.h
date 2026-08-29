#pragma once

#include "CoreMinimal.h"

class UFunction;

struct FAvidScriptEditorLatentFunctionContract
{
	bool bLatent = false;
	FString LatentInfoParameter;
	FString WorldContextParameter;
};

class FAvidScriptEditorReflectedFunctionPolicy
{
public:
	static bool Evaluate(
		const UFunction* Function,
		FString& OutCategory,
		FString& OutSource,
		FAvidScriptEditorLatentFunctionContract* OutLatentContract = nullptr);
};
