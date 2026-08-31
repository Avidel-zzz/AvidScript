#include "GeneratedTypes/AvidScriptEditorGeneratedTypeReloadPolicy.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr TCHAR GeneratedPackageDescriptorRelativePath[] =
	TEXT("Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json");

FString NormalizeGeneratedTypeReloadPolicyPath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		FPaths::RemoveDuplicateSlashes(Path);
	}
	return Path;
}

bool IsSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character)
			&& !(Character >= TEXT('a') && Character <= TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool LoadDescriptorObject(
	const FString& DescriptorPath,
	TSharedPtr<FJsonObject>& OutDescriptor,
	FString& OutErrorCategory,
	FString& OutErrorMessage)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *DescriptorPath))
	{
		OutErrorCategory = TEXT("generated_type_descriptor_read_failed");
		OutErrorMessage = FString::Printf(
			TEXT("Generated type package descriptor cannot be read: %s"),
			*DescriptorPath);
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, OutDescriptor)
		|| !OutDescriptor.IsValid())
	{
		OutErrorCategory = TEXT("generated_type_descriptor_json_invalid");
		OutErrorMessage = FString::Printf(
			TEXT("Generated type package descriptor is not valid JSON: %s"),
			*DescriptorPath);
		return false;
	}
	return true;
}
} // namespace

FString FAvidScriptEditorGeneratedTypeReloadPolicy::GetDefaultDescriptorPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	return Plugin.IsValid()
		? FPaths::Combine(Plugin->GetBaseDir(), GeneratedPackageDescriptorRelativePath)
		: FString();
}

bool FAvidScriptEditorGeneratedTypeReloadPolicy::ReadPackageId(
	const FString& DescriptorPath,
	FString& OutPackageId,
	FString& OutErrorCategory,
	FString& OutErrorMessage)
{
	TSharedPtr<FJsonObject> Descriptor;
	if (!LoadDescriptorObject(
			DescriptorPath,
			Descriptor,
			OutErrorCategory,
			OutErrorMessage))
	{
		return false;
	}
	double SchemaVersion = 0.0;
	if (!Descriptor->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
		|| static_cast<int32>(SchemaVersion) != 1
		|| !Descriptor->TryGetStringField(TEXT("package_id"), OutPackageId)
		|| !IsSha256(OutPackageId))
	{
		OutErrorCategory = TEXT("generated_type_descriptor_identity_invalid");
		OutErrorMessage = TEXT(
			"Generated type package descriptor has an invalid schema or package id.");
		return false;
	}
	return true;
}

bool FAvidScriptEditorGeneratedTypeReloadPolicy::ReadDescriptorIdentity(
	const FString& DescriptorPath,
	FAvidScriptEditorGeneratedTypeDescriptorIdentity& OutIdentity,
	FString& OutErrorCategory,
	FString& OutErrorMessage)
{
	OutIdentity = FAvidScriptEditorGeneratedTypeDescriptorIdentity();
	OutErrorCategory.Reset();
	OutErrorMessage.Reset();

	TSharedPtr<FJsonObject> Descriptor;
	if (!LoadDescriptorObject(
			DescriptorPath,
			Descriptor,
			OutErrorCategory,
			OutErrorMessage))
	{
		return false;
	}
	double DescriptorSchemaVersion = 0.0;
	const TSharedPtr<FJsonObject>* Reload = nullptr;
	double ReloadSchemaVersion = 0.0;
	FString Classification;
	if (!Descriptor->TryGetNumberField(TEXT("schema_version"), DescriptorSchemaVersion)
		|| static_cast<int32>(DescriptorSchemaVersion) != 1
		|| !Descriptor->TryGetStringField(TEXT("package_id"), OutIdentity.PackageId)
		|| !Descriptor->TryGetObjectField(TEXT("reload"), Reload)
		|| Reload == nullptr
		|| !(*Reload)->TryGetNumberField(TEXT("schema_version"), ReloadSchemaVersion)
		|| static_cast<int32>(ReloadSchemaVersion) != 1
		|| !(*Reload)->TryGetStringField(TEXT("classification"), Classification)
		|| !(*Reload)->TryGetStringField(
			TEXT("native_structure_sha256"),
			OutIdentity.NativeStructureSha256)
		|| !(*Reload)->TryGetStringField(
			TEXT("previous_native_structure_sha256"),
			OutIdentity.PreviousNativeStructureSha256)
		|| !(*Reload)->TryGetStringField(
			TEXT("previous_package_id"),
			OutIdentity.PreviousPackageId)
		|| !IsSha256(OutIdentity.PackageId)
		|| !IsSha256(OutIdentity.NativeStructureSha256))
	{
		OutErrorCategory = TEXT("generated_type_reload_metadata_invalid");
		OutErrorMessage = TEXT(
			"Generated type package descriptor reload metadata is incomplete or invalid.");
		return false;
	}

	if (Classification == TEXT("initial_install"))
	{
		OutIdentity.Classification =
			EAvidScriptEditorGeneratedTypeReloadClassification::InitialInstall;
		if (!OutIdentity.PreviousPackageId.IsEmpty()
			|| !OutIdentity.PreviousNativeStructureSha256.IsEmpty())
		{
			OutErrorCategory = TEXT("generated_type_reload_initial_chain_invalid");
			OutErrorMessage = TEXT(
				"Initial generated type package unexpectedly names a previous package.");
			return false;
		}
		return true;
	}

	if (!IsSha256(OutIdentity.PreviousPackageId)
		|| !IsSha256(OutIdentity.PreviousNativeStructureSha256))
	{
		OutErrorCategory = TEXT("generated_type_reload_previous_identity_invalid");
		OutErrorMessage = TEXT(
			"Generated type reload package has an invalid previous package identity.");
		return false;
	}
	if (Classification == TEXT("body_only"))
	{
		OutIdentity.Classification =
			EAvidScriptEditorGeneratedTypeReloadClassification::BodyOnly;
		if (OutIdentity.NativeStructureSha256 !=
			OutIdentity.PreviousNativeStructureSha256)
		{
			OutErrorCategory = TEXT("generated_type_body_only_structure_mismatch");
			OutErrorMessage = TEXT(
				"Body-only generated type package changed its native structure identity.");
			return false;
		}
		return true;
	}
	if (Classification == TEXT("native_rebuild_required"))
	{
		OutIdentity.Classification =
			EAvidScriptEditorGeneratedTypeReloadClassification::NativeRebuildRequired;
		if (OutIdentity.NativeStructureSha256 ==
			OutIdentity.PreviousNativeStructureSha256)
		{
			OutErrorCategory = TEXT("generated_type_rebuild_structure_unchanged");
			OutErrorMessage = TEXT(
				"Native rebuild classification did not change the native structure identity.");
			return false;
		}
		return true;
	}

	OutErrorCategory = TEXT("generated_type_reload_classification_unknown");
	OutErrorMessage = FString::Printf(
		TEXT("Generated type reload classification is unsupported: %s"),
		*Classification);
	return false;
}

bool FAvidScriptEditorGeneratedTypeReloadPolicy::ApplyPublishedDescriptor(
	const FString& DescriptorPath,
	const EAvidScriptEditorGeneratedTypeReloadClassification Classification,
	FAvidScriptEditorGeneratedTypeReloadServiceResult& OutResult)
{
	OutResult = FAvidScriptEditorGeneratedTypeReloadServiceResult();
	OutResult.DescriptorPath = NormalizeGeneratedTypeReloadPolicyPath(DescriptorPath);
	FString IdentityErrorCategory;
	FString IdentityErrorMessage;
	if (!ReadDescriptorIdentity(
			OutResult.DescriptorPath,
			OutResult.DescriptorIdentity,
			IdentityErrorCategory,
			IdentityErrorMessage)
		|| OutResult.DescriptorIdentity.Classification != Classification)
	{
		OutResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::Rejected;
		OutResult.ErrorCategory = IdentityErrorCategory.IsEmpty()
			? FString(TEXT("generated_type_reload_classification_drift"))
			: IdentityErrorCategory;
		OutResult.ErrorMessage = IdentityErrorMessage.IsEmpty()
			? FString(TEXT("Generated type reload classification changed before apply."))
			: IdentityErrorMessage;
		OutResult.NextAction = TEXT("rebuild the generated type package and retry");
		return false;
	}

	if (Classification ==
		EAvidScriptEditorGeneratedTypeReloadClassification::NativeRebuildRequired)
	{
		OutResult.bSucceeded = true;
		OutResult.Status =
			EAvidScriptEditorGeneratedTypeReloadStatus::NativeRebuildRequired;
		OutResult.ErrorCategory = TEXT("generated_type_native_rebuild_required");
		OutResult.ErrorMessage = TEXT(
			"Generated C# type declarations changed the native UClass shell.");
		OutResult.NextAction = TEXT(
			"run the controlled no-clean UE5.8 Editor build, then restart the Editor");
		return true;
	}

	FString RuntimeError;
	OutResult.bRuntimeMutationAttempted = true;
	if (Classification ==
		EAvidScriptEditorGeneratedTypeReloadClassification::InitialInstall)
	{
		if (!FAvidScriptGeneratedTypeRuntimeHost::Get()
				.InstallPackageFromDescriptorFile(OutResult.DescriptorPath, RuntimeError))
		{
			OutResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::Rejected;
			OutResult.ErrorCategory = TEXT("generated_type_initial_install_rejected");
			OutResult.ErrorMessage = RuntimeError;
			OutResult.NextAction = TEXT(
				"inspect the generated package identity and restart the Editor if a stale package is active");
			return false;
		}
		OutResult.bSucceeded = true;
		OutResult.bRuntimeApplied = true;
		OutResult.Status =
			EAvidScriptEditorGeneratedTypeReloadStatus::InitialInstallApplied;
		return true;
	}

	if (!FAvidScriptGeneratedTypeRuntimeHost::Get().ReloadPackageFromDescriptorFile(
			OutResult.DescriptorPath,
			OutResult.RuntimeReloadResult,
			RuntimeError))
	{
		if (OutResult.RuntimeReloadResult.Disposition ==
			EAvidScriptGeneratedTypePackageReloadDisposition::NativeRebuildRequired)
		{
			OutResult.bSucceeded = true;
			OutResult.Status =
				EAvidScriptEditorGeneratedTypeReloadStatus::NativeRebuildRequired;
			OutResult.ErrorCategory = TEXT("generated_type_runtime_structure_drift");
			OutResult.ErrorMessage =
				OutResult.RuntimeReloadResult.StructuralChangeReason;
			OutResult.NextAction = TEXT(
				"run the controlled no-clean UE5.8 Editor build, then restart the Editor");
			return true;
		}
		OutResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::Rejected;
		OutResult.ErrorCategory = TEXT("generated_type_body_reload_rejected");
		OutResult.ErrorMessage = RuntimeError;
		OutResult.NextAction = TEXT(
			"fix the reported package or runtime error; the previous package remains active");
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.bRuntimeApplied = true;
	OutResult.Status = EAvidScriptEditorGeneratedTypeReloadStatus::BodyOnlyApplied;
	return true;
}
