#include "AvidScriptEditorGeneratedBindingService.h"

#include "GeneratedBindings/AvidScriptEditorGeneratedBindingIrBuilder.h"
#include "GeneratedBindings/AvidScriptEditorGeneratedBindingSourceEmitter.h"
#include "Misc/FileHelper.h"

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

bool FAvidScriptEditorGeneratedBindingService::GenerateProjectModuleFromDescriptorFile(
	const FString& ProjectFile,
	const FString& DescriptorFile,
	FAvidScriptEditorGeneratedBindingResult& OutResult)
{
	FString DescriptorJson;
	if (!FFileHelper::LoadFileToString(DescriptorJson, *DescriptorFile))
	{
		OutResult = FAvidScriptEditorGeneratedBindingResult();
		OutResult.ErrorCategory = TEXT("generated_binding_descriptor_unreadable");
		OutResult.ErrorSource = DescriptorFile;
		OutResult.ErrorMessage = FString::Printf(
			TEXT("Generated binding descriptor could not be read: %s"),
			*DescriptorFile);
		return false;
	}

	FAvidScriptGeneratedBindingPackageIr Package;
	if (!BuildIr(DescriptorJson, Package, OutResult))
	{
		return false;
	}
	if (Package.Bindings.IsEmpty())
	{
		return true;
	}
	return EmitProjectModule(ProjectFile, Package, OutResult);
}
