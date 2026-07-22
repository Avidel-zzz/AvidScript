#include "AvidScriptEditorCSharpBindingEmitter.h"

#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptHash.h"
#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "BindingGeneration/AvidScriptEditorCSharpBindingArtifact.h"
#include "BindingGeneration/AvidScriptEditorCSharpBindingRenderer.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace BindingArtifact = AvidScriptCSharpBindingArtifact;

namespace
{
void SetFailure(
	FAvidScriptCSharpBindingEmitResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& NextAction)
{
	OutResult = FAvidScriptCSharpBindingEmitResult();
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript C# binding emission error | category=%s | source=%s | next=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*NextAction);
}

bool ValidateCanonicalDescriptor(
	const FString& DescriptorJson,
	const FAvidScriptBindingPackageModel& Package,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	TArray<FAvidScriptReflectedFunctionSelection> FunctionSelections;
	TArray<FAvidScriptReflectedPropertySelection> PropertySelections;
	FunctionSelections.Reserve(Package.Bindings.Num());
	PropertySelections.Reserve(Package.Bindings.Num());
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		if (Binding.BindingKind == TEXT("property_get"))
		{
			PropertySelections.Add({ Binding.OwnerClass, FName(*Binding.UeMember) });
		}
		else
		{
			FunctionSelections.Add({ Binding.OwnerClass, FName(*Binding.UeMember) });
		}
	}
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	ClassReferences.Reserve(Package.ClassReferences.Num());
	for (const FAvidScriptBindingClassReferenceModel& Reference : Package.ClassReferences)
	{
		ClassReferences.Add({
			Reference.ScriptName,
			Reference.ClassPath,
			Reference.BaseClassPath,
			Reference.LoadPolicy
		});
	}

	FString CanonicalDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult RegenerationResult;
	if (!FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
		Package.PackageName,
		FunctionSelections,
		PropertySelections,
		ClassReferences,
		CanonicalDescriptorJson,
		RegenerationResult))
	{
		OutErrorCategory = TEXT("descriptor_regeneration_failed");
		OutErrorSource = RegenerationResult.ErrorSource.IsEmpty()
			? Package.PackageName
			: RegenerationResult.ErrorSource;
		return false;
	}
	if (CanonicalDescriptorJson != DescriptorJson)
	{
		OutErrorCategory = TEXT("descriptor_not_canonical");
		OutErrorSource = Package.PackageName;
		return false;
	}
	return true;
}

bool IsSafePackageName(const FString& Value)
{
	if (Value.IsEmpty())
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('.') && Character != TEXT('_') && Character != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

void ConvertToUtf8Bytes(const FString& Text, TArray<uint8>& OutBytes)
{
	const FTCHARToUTF8 Converted(*Text);
	OutBytes.Reset(Converted.Length());
	OutBytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
}

bool FileMatchesUtf8Bytes(const FString& Path, const FString& ExpectedText)
{
	TArray<uint8> ExistingBytes;
	TArray<uint8> ExpectedBytes;
	ConvertToUtf8Bytes(ExpectedText, ExpectedBytes);
	return FFileHelper::LoadFileToArray(ExistingBytes, *Path) && ExistingBytes == ExpectedBytes;
}

bool ExistingPackageMatches(
	const FString& PackageDirectory,
	const FString& Descriptor,
	const FString& Source,
	const FString& Manifest,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	const FString DescriptorPath = FPaths::Combine(PackageDirectory, BindingArtifact::DescriptorFileName);
	const FString SourcePath = FPaths::Combine(PackageDirectory, BindingArtifact::ReferenceSourceFileName);
	const FString ManifestPath = FPaths::Combine(PackageDirectory, BindingArtifact::ManifestFileName);
	if (!FileMatchesUtf8Bytes(DescriptorPath, Descriptor)
		|| !FileMatchesUtf8Bytes(SourcePath, Source)
		|| !FileMatchesUtf8Bytes(ManifestPath, Manifest))
	{
		SetFailure(
			OutResult,
			TEXT("package_conflict"),
			PackageDirectory,
			TEXT("Remove or repair the corrupt content-addressed package before publishing again."));
		return false;
	}
	OutResult.bReusedExistingPackage = true;
	OutResult.PackageDirectory = PackageDirectory;
	OutResult.DescriptorPath = DescriptorPath;
	OutResult.ReferenceSourcePath = SourcePath;
	OutResult.ManifestPath = ManifestPath;
	return true;
}

void SetPublishedPackageResult(
	const FAvidScriptCSharpBindingEmitResult& EmitResult,
	const FString& PackageDirectory,
	const bool bReusedExistingPackage,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	OutResult = EmitResult;
	OutResult.bReusedExistingPackage = bReusedExistingPackage;
	OutResult.PackageDirectory = PackageDirectory;
	OutResult.DescriptorPath = FPaths::Combine(PackageDirectory, BindingArtifact::DescriptorFileName);
	OutResult.ReferenceSourcePath = FPaths::Combine(PackageDirectory, BindingArtifact::ReferenceSourceFileName);
	OutResult.ManifestPath = FPaths::Combine(PackageDirectory, BindingArtifact::ManifestFileName);
}

bool PublishGeneratedPackage(
	const FString& OutputRoot,
	const FString& Descriptor,
	const FString& Source,
	const FString& Manifest,
	const FAvidScriptCSharpBindingEmitResult& EmitResult,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	if (OutputRoot.IsEmpty())
	{
		SetFailure(
			OutResult,
			TEXT("output_root_missing"),
			OutputRoot,
			TEXT("Provide a writable generated binding package root."));
		return false;
	}

	const FString FullOutputRoot = FPaths::ConvertRelativePathToFull(OutputRoot);
	const FString PackageParent = FPaths::Combine(FullOutputRoot, EmitResult.PackageName);
	const FString FinalDirectory = FPaths::Combine(PackageParent, EmitResult.ManifestHash);
	if (IFileManager::Get().DirectoryExists(*FinalDirectory))
	{
		FAvidScriptCSharpBindingEmitResult ValidationResult;
		if (!ExistingPackageMatches(FinalDirectory, Descriptor, Source, Manifest, ValidationResult))
		{
			OutResult = MoveTemp(ValidationResult);
			return false;
		}
		SetPublishedPackageResult(EmitResult, FinalDirectory, true, OutResult);
		return true;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.CreateDirectoryTree(*PackageParent))
	{
		SetFailure(
			OutResult,
			TEXT("publish_directory_failed"),
			PackageParent,
			TEXT("Verify that the generated binding output root is writable."));
		return false;
	}
	const FString StagingRoot = FPaths::Combine(FullOutputRoot, TEXT(".staging"));
	const FString StagingDirectory = FPaths::Combine(
		StagingRoot,
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!PlatformFile.CreateDirectoryTree(*StagingDirectory))
	{
		SetFailure(
			OutResult,
			TEXT("publish_directory_failed"),
			StagingDirectory,
			TEXT("Verify that the generated binding output root is writable."));
		return false;
	}

	const FString StagedDescriptor = FPaths::Combine(StagingDirectory, BindingArtifact::DescriptorFileName);
	const FString StagedSource = FPaths::Combine(StagingDirectory, BindingArtifact::ReferenceSourceFileName);
	const FString StagedManifest = FPaths::Combine(StagingDirectory, BindingArtifact::ManifestFileName);
	const bool bWroteAll = FFileHelper::SaveStringToFile(
		Descriptor,
		*StagedDescriptor,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		&& FFileHelper::SaveStringToFile(
			Source,
			*StagedSource,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		&& FFileHelper::SaveStringToFile(
			Manifest,
			*StagedManifest,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!bWroteAll)
	{
		PlatformFile.DeleteDirectoryRecursively(*StagingDirectory);
		SetFailure(
			OutResult,
			TEXT("publish_write_failed"),
			StagingDirectory,
			TEXT("Verify free space and write permissions, then publish again."));
		return false;
	}

	// Platform files expose directory renames through MoveFile on supported editor hosts.
	if (!PlatformFile.MoveFile(*FinalDirectory, *StagingDirectory))
	{
		PlatformFile.DeleteDirectoryRecursively(*StagingDirectory);
		if (PlatformFile.DirectoryExists(*FinalDirectory))
		{
			FAvidScriptCSharpBindingEmitResult ValidationResult;
			if (ExistingPackageMatches(FinalDirectory, Descriptor, Source, Manifest, ValidationResult))
			{
				SetPublishedPackageResult(EmitResult, FinalDirectory, true, OutResult);
				return true;
			}
			OutResult = MoveTemp(ValidationResult);
			return false;
		}
		SetFailure(
			OutResult,
			TEXT("publish_commit_failed"),
			FinalDirectory,
			TEXT("Close package readers and retry the content-addressed directory publish."));
		return false;
	}

	SetPublishedPackageResult(EmitResult, FinalDirectory, false, OutResult);
	return true;
}
} // namespace

bool FAvidScriptEditorCSharpBindingEmitter::Emit(
	const FString& DescriptorJson,
	FString& OutReferenceSource,
	FString& OutManifestJson,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	OutReferenceSource.Empty();
	OutManifestJson.Empty();
	OutResult = FAvidScriptCSharpBindingEmitResult();
	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!FAvidScriptEditorBindingDescriptorModelParser::Parse(
		DescriptorJson,
		Package,
		ErrorCategory,
		ErrorSource))
	{
		SetFailure(
			OutResult,
			ErrorCategory,
			ErrorSource,
			TEXT("Regenerate the descriptor with the matching Phase 42 generator."));
		return false;
	}
	if (!IsSafePackageName(Package.PackageName))
	{
		SetFailure(
			OutResult,
			TEXT("package_name_invalid"),
			Package.PackageName,
			TEXT("Use only letters, digits, dot, underscore, and dash in package names."));
		return false;
	}
	if (!ValidateCanonicalDescriptor(DescriptorJson, Package, ErrorCategory, ErrorSource))
	{
		SetFailure(
			OutResult,
			ErrorCategory,
			ErrorSource,
			TEXT("Regenerate the descriptor from the current UE reflection snapshot."));
		return false;
	}

	const FString DescriptorHash = FAvidScriptHash::Sha256HexUtf8(DescriptorJson);
	if (!FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(Package, DescriptorHash, OutReferenceSource, ErrorCategory, ErrorSource))
	{
		OutReferenceSource.Empty();
		SetFailure(
			OutResult,
			ErrorCategory,
			ErrorSource,
			TEXT("Resolve the descriptor projection or generated C# name collision."));
		return false;
	}
	const FString SourceHash = FAvidScriptHash::Sha256HexUtf8(OutReferenceSource);
	if (!FAvidScriptEditorCSharpBindingRenderer::EmitManifest(Package, DescriptorHash, SourceHash, OutManifestJson))
	{
		OutReferenceSource.Empty();
		OutManifestJson.Empty();
		SetFailure(
			OutResult,
			TEXT("manifest_serialize_failed"),
			Package.PackageName,
			TEXT("Inspect the generated binding manifest writer state."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.BindingCount = Package.Bindings.Num();
	OutResult.TypeCount = Package.Types.Num();
	OutResult.ClassReferenceCount = Package.ClassReferences.Num();
	OutResult.PackageName = Package.PackageName;
	OutResult.PackageHash = Package.PackageHash;
	OutResult.DescriptorHash = DescriptorHash;
	OutResult.SourceHash = SourceHash;
	OutResult.ManifestHash = FAvidScriptHash::Sha256HexUtf8(OutManifestJson);
	return true;
}

bool FAvidScriptEditorCSharpBindingEmitter::EmitDefault(
	FString& OutDescriptorJson,
	FString& OutReferenceSource,
	FString& OutManifestJson,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(OutDescriptorJson, DescriptorResult))
	{
		OutDescriptorJson.Empty();
		OutReferenceSource.Empty();
		OutManifestJson.Empty();
		SetFailure(
			OutResult,
			DescriptorResult.ErrorCategory,
			DescriptorResult.ErrorSource,
			DescriptorResult.NextAction);
		return false;
	}
	return Emit(OutDescriptorJson, OutReferenceSource, OutManifestJson, OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::EmitEngineGameplay(
	FString& OutDescriptorJson,
	FString& OutReferenceSource,
	FString& OutManifestJson,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	return EmitProfile(
		FAvidScriptEditorBindingDescriptorGenerator::MakeEngineGameplayProfile(),
		OutDescriptorJson,
		OutReferenceSource,
		OutManifestJson,
		OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::EmitProfile(
	const FAvidScriptBindingSelectionProfile& Profile,
	FString& OutDescriptorJson,
	FString& OutReferenceSource,
	FString& OutManifestJson,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	return EmitProfile(
		Profile,
		{},
		OutDescriptorJson,
		OutReferenceSource,
		OutManifestJson,
		OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::EmitProfile(
	const FAvidScriptBindingSelectionProfile& Profile,
	const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
	FString& OutDescriptorJson,
	FString& OutReferenceSource,
	FString& OutManifestJson,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
		Profile,
		ClassReferences,
		OutDescriptorJson,
		SelectionResult,
		DescriptorResult))
	{
		OutDescriptorJson.Empty();
		OutReferenceSource.Empty();
		OutManifestJson.Empty();
		SetFailure(
			OutResult,
			DescriptorResult.ErrorCategory,
			DescriptorResult.ErrorSource,
			DescriptorResult.NextAction);
		return false;
	}
	return Emit(OutDescriptorJson, OutReferenceSource, OutManifestJson, OutResult);
}

FString FAvidScriptEditorCSharpBindingEmitter::GetDefaultOutputRoot()
{
	FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptGeneratedBindings")));
	FPaths::NormalizeDirectoryName(OutputRoot);
	return OutputRoot;
}

bool FAvidScriptEditorCSharpBindingEmitter::PublishDescriptor(
	const FString& DescriptorJson,
	const FString& OutputRoot,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	FString Source;
	FString Manifest;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	if (!Emit(DescriptorJson, Source, Manifest, EmitResult))
	{
		OutResult = MoveTemp(EmitResult);
		return false;
	}
	return PublishGeneratedPackage(
		OutputRoot,
		DescriptorJson,
		Source,
		Manifest,
		EmitResult,
		OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::PublishDefault(
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	return PublishDefault(GetDefaultOutputRoot(), OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::PublishDefault(
	const FString& OutputRoot,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	FString Descriptor;
	FString Source;
	FString Manifest;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	if (!EmitDefault(Descriptor, Source, Manifest, EmitResult))
	{
		OutResult = MoveTemp(EmitResult);
		return false;
	}
	return PublishGeneratedPackage(OutputRoot, Descriptor, Source, Manifest, EmitResult, OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	return PublishEngineGameplay(GetDefaultOutputRoot(), OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(
	const FString& OutputRoot,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	return PublishProfile(
		FAvidScriptEditorBindingDescriptorGenerator::MakeEngineGameplayProfile(),
		OutputRoot,
		OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::PublishProfile(
	const FAvidScriptBindingSelectionProfile& Profile,
	const FString& OutputRoot,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	return PublishProfile(Profile, {}, OutputRoot, OutResult);
}

bool FAvidScriptEditorCSharpBindingEmitter::PublishProfile(
	const FAvidScriptBindingSelectionProfile& Profile,
	const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
	const FString& OutputRoot,
	FAvidScriptCSharpBindingEmitResult& OutResult)
{
	FString Descriptor;
	FString Source;
	FString Manifest;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	if (!EmitProfile(Profile, ClassReferences, Descriptor, Source, Manifest, EmitResult))
	{
		OutResult = MoveTemp(EmitResult);
		return false;
	}
	return PublishGeneratedPackage(OutputRoot, Descriptor, Source, Manifest, EmitResult, OutResult);
}
