#include "AvidScriptEditorGeneratedBindingService.h"

#include "GeneratedBindings/AvidScriptEditorGeneratedBindingIrBuilder.h"
#include "GeneratedBindings/AvidScriptEditorGeneratedBindingSourceEmitter.h"

bool FAvidScriptEditorGeneratedBindingService::BuildIr(
	const FString& DescriptorJson,
	FAvidScriptGeneratedBindingPackageIr& OutPackage,
	FAvidScriptEditorGeneratedBindingResult& OutResult)
{
	return FAvidScriptEditorGeneratedBindingIrBuilder::Build(
		DescriptorJson,
		OutPackage,
		OutResult);
}

bool FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
	const FString& ProjectFile,
	const FAvidScriptGeneratedBindingPackageIr& Package,
	FAvidScriptEditorGeneratedBindingResult& OutResult)
{
	return FAvidScriptEditorGeneratedBindingSourceEmitter::Emit(
		ProjectFile,
		Package,
		OutResult);
}

bool FAvidScriptEditorGeneratedBindingService::GenerateProjectModule(
	const FString& ProjectFile,
	const FString& DescriptorJson,
	FAvidScriptEditorGeneratedBindingResult& OutResult)
{
	FAvidScriptGeneratedBindingPackageIr Package;
	if (!BuildIr(DescriptorJson, Package, OutResult))
	{
		return false;
	}
	return EmitProjectModule(ProjectFile, Package, OutResult);
}
