#pragma once

#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "CoreMinimal.h"

class FAvidScriptEditorCSharpDefaultValueFormatter
{
public:
	static bool TryFormat(
		const FAvidScriptBindingValueModel& Value,
		const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
		FString& OutExpression);
};
