#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"

bool FAvidScriptEditorBindingDescriptorModelParser::Parse(
	const FString& Json,
	FAvidScriptBindingPackageModel& OutPackage,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	return FAvidScriptBindingDescriptorParser::Parse(
		Json,
		OutPackage,
		OutErrorCategory,
		OutErrorSource);
}
