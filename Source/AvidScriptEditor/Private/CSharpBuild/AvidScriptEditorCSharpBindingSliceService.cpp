#include "CSharpBuild/AvidScriptEditorCSharpBindingSliceService.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptFrontendReport.h"
#include "AvidScriptHash.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
void SetAvidScriptCSharpBindingSliceFailure(
	FAvidScriptEditorCSharpBindingSliceResult& OutResult,
	const FString& ErrorCategory,
	const FString& ErrorSource,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorSource = ErrorSource;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("%s: %s"),
		*ErrorCategory,
		*ErrorSource);
}

bool IsAvidScriptCSharpBindingSliceStableId(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

FString NormalizeAvidScriptCSharpBindingSlicePath(FString Path)
{
	if (!FPaths::IsRelative(Path))
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
	}
	else
	{
		Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Path));
	}
	FPaths::NormalizeFilename(Path);
	return Path;
}
} // namespace

bool FAvidScriptEditorCSharpBindingSliceService::Publish(
	const FString& AuthorizationDescriptorPath,
	const FAvidScriptFrontendBindingPackage& Provenance,
	const FString& OutputRoot,
	FAvidScriptCSharpBindingEmitResult& OutPackage,
	FAvidScriptEditorCSharpBindingSliceResult& OutResult)
{
	OutPackage = FAvidScriptCSharpBindingEmitResult();
	OutResult = FAvidScriptEditorCSharpBindingSliceResult();
	OutResult.RequestedBindingCount = Provenance.UsedImports.Num();

	if (!Provenance.bPresent
		|| Provenance.UsedImportCount != Provenance.UsedImports.Num()
		|| Provenance.UsedImports.IsEmpty())
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_provenance_invalid"),
			TEXT("binding_package.used_imports"),
			TEXT("rerun the bootstrap build and preserve complete binding provenance"));
		return false;
	}

	const FString FullDescriptorPath = NormalizeAvidScriptCSharpBindingSlicePath(AuthorizationDescriptorPath);
	const FString ProvenanceDescriptorPath = NormalizeAvidScriptCSharpBindingSlicePath(Provenance.DescriptorFile);
	if (FullDescriptorPath != ProvenanceDescriptorPath)
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_descriptor_mismatch"),
			Provenance.DescriptorFile,
			TEXT("use the authorization descriptor recorded by the bootstrap report"));
		return false;
	}

	FString AuthorizationJson;
	if (!FFileHelper::LoadFileToString(AuthorizationJson, *FullDescriptorPath))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_descriptor_read_failed"),
			FullDescriptorPath,
			TEXT("republish the complete authorization binding package"));
		return false;
	}
	if (Provenance.DescriptorSha256.IsEmpty()
		|| FAvidScriptHash::Sha256HexUtf8(AuthorizationJson) != Provenance.DescriptorSha256)
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_descriptor_hash_mismatch"),
			FullDescriptorPath,
			TEXT("rerun bootstrap with an untampered authorization package"));
		return false;
	}

	FAvidScriptBindingPackageModel AuthorizationModel;
	FString ParseCategory;
	FString ParseSource;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			AuthorizationJson,
			AuthorizationModel,
			ParseCategory,
			ParseSource))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			ParseCategory,
			ParseSource,
			TEXT("regenerate the authorization descriptor from UE reflection"));
		return false;
	}
	const TConstArrayView<FAvidScriptObjectLifecycleBindingSpec> LifecycleSpecs =
		FAvidScriptObjectLifecycleBindings::GetSpecs();
	const int32 ReflectedBindingCount = AuthorizationModel.Bindings.Num();
	const bool bPublishesLifecycleCapabilities = !AuthorizationModel.ClassReferences.IsEmpty();
	const int32 ExpectedProfileImportCount = ReflectedBindingCount
		+ (bPublishesLifecycleCapabilities ? LifecycleSpecs.Num() : 0);
	if (AuthorizationModel.PackageName != Provenance.PackageName
		|| AuthorizationModel.PackageHash != Provenance.PackageHash
		|| ExpectedProfileImportCount != Provenance.ProfileImportCount)
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_package_identity_mismatch"),
			Provenance.PackageName,
			TEXT("rerun bootstrap with matching package and descriptor provenance"));
		return false;
	}

	TSet<FString> SeenRequestedStableIds;
	TSet<FString> RequestedReflectedStableIds;
	TArray<FAvidScriptReflectedFunctionSelection> FunctionSelections;
	TArray<FAvidScriptReflectedPropertySelection> PropertySelections;
	FunctionSelections.Reserve(Provenance.UsedImports.Num());
	PropertySelections.Reserve(Provenance.UsedImports.Num());
	for (const FAvidScriptFrontendBindingImport& Import : Provenance.UsedImports)
	{
		if (!IsAvidScriptCSharpBindingSliceStableId(Import.StableId))
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_stable_id_invalid"),
				Import.StableId,
				TEXT("rerun bootstrap with canonical lowercase SHA-256 stable identities"));
			return false;
		}
		if (SeenRequestedStableIds.Contains(Import.StableId))
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_duplicate_stable_id"),
				Import.StableId,
				TEXT("deduplicate bootstrap used_imports before slice generation"));
			return false;
		}
		SeenRequestedStableIds.Add(Import.StableId);

		if (AuthorizationModel.Bindings.IsValidIndex(Import.Ordinal))
		{
			const FAvidScriptBindingFunctionModel& Binding = AuthorizationModel.Bindings[Import.Ordinal];
			if (Binding.StableId != Import.StableId)
			{
				SetAvidScriptCSharpBindingSliceFailure(
					OutResult,
					TEXT("slice_binding_missing"),
					Import.StableId,
					TEXT("regenerate the complete package and rerun bootstrap"));
				return false;
			}
			if (Binding.HostImport.Module != Import.Module
				|| Binding.HostImport.Name != Import.Name
				|| Binding.HostImport.Signature != Import.Signature)
			{
				SetAvidScriptCSharpBindingSliceFailure(
					OutResult,
					TEXT("slice_import_identity_mismatch"),
					Import.StableId,
					TEXT("rerun bootstrap with untampered import provenance"));
				return false;
			}

			RequestedReflectedStableIds.Add(Import.StableId);
			if (Binding.BindingKind == TEXT("property_get"))
			{
				PropertySelections.Add({ Binding.OwnerClass, FName(*Binding.UeMember) });
			}
			else
			{
				FunctionSelections.Add({ Binding.OwnerClass, FName(*Binding.UeMember) });
			}
			continue;
		}

		if (!bPublishesLifecycleCapabilities
			|| Import.Ordinal < ReflectedBindingCount)
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_binding_missing"),
				Import.StableId,
				TEXT("regenerate the complete package and rerun bootstrap"));
			return false;
		}
		const int32 LifecycleSpecIndex = Import.Ordinal - ReflectedBindingCount;
		if (LifecycleSpecIndex >= LifecycleSpecs.Num()
			|| LifecycleSpecs[LifecycleSpecIndex].StableId != Import.StableId)
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_binding_missing"),
				Import.StableId,
				TEXT("regenerate the complete package and rerun bootstrap"));
			return false;
		}
		const FAvidScriptObjectLifecycleBindingSpec& LifecycleSpec = LifecycleSpecs[LifecycleSpecIndex];
		if (LifecycleSpec.ModuleName != Import.Module
			|| LifecycleSpec.ImportName != Import.Name
			|| LifecycleSpec.Signature != Import.Signature)
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_import_identity_mismatch"),
				Import.StableId,
				TEXT("rerun bootstrap with untampered import provenance"));
			return false;
		}
	}

	FString SliceDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	ClassReferences.Reserve(AuthorizationModel.ClassReferences.Num());
	for (const FAvidScriptBindingClassReferenceModel& Reference : AuthorizationModel.ClassReferences)
	{
		ClassReferences.Add({
			Reference.ScriptName,
			Reference.ClassPath,
			Reference.BaseClassPath,
			Reference.LoadPolicy
		});
	}
	if (!FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
			AuthorizationModel.PackageName,
			FunctionSelections,
			PropertySelections,
			ClassReferences,
			SliceDescriptorJson,
			GenerateResult))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			GenerateResult.ErrorCategory,
			GenerateResult.ErrorSource,
			GenerateResult.NextAction);
		return false;
	}

	FAvidScriptBindingPackageModel SliceModel;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			SliceDescriptorJson,
			SliceModel,
			ParseCategory,
			ParseSource))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			ParseCategory,
			ParseSource,
			TEXT("repair the generated slice descriptor contract"));
		return false;
	}
	if (SliceModel.Bindings.Num() != RequestedReflectedStableIds.Num())
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_selection_mismatch"),
			AuthorizationModel.PackageName,
			TEXT("regenerate the slice from stable reflected selections"));
		return false;
	}
	if (SliceModel.ClassReferences.Num() != AuthorizationModel.ClassReferences.Num())
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_class_reference_mismatch"),
			AuthorizationModel.PackageName,
			TEXT("preserve the complete authorization class table in the runtime slice"));
		return false;
	}
	for (const FAvidScriptBindingFunctionModel& Binding : SliceModel.Bindings)
	{
		if (!RequestedReflectedStableIds.Contains(Binding.StableId))
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_selection_mismatch"),
				Binding.StableId,
				TEXT("regenerate the slice from stable reflected selections"));
			return false;
		}
	}

	if (!FAvidScriptEditorCSharpBindingEmitter::PublishDescriptor(
			SliceDescriptorJson,
			OutputRoot,
			OutPackage))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			OutPackage.ErrorCategory,
			OutPackage.ErrorSource,
			OutPackage.NextAction);
		return false;
	}

	OutResult.bSucceeded = true;
	return true;
}
