#include "AvidScriptEditorDiagnosticNavigation.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SourceCodeNavigation.h"

#include <openssl/sha.h>

namespace
{
void SetAvidScriptDiagnosticNavigationFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	OutResult = FAvidScriptEditorDiagnosticNavigationResult();
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
}

bool TryGetAvidScriptSourceSha256(const FString& Path, FString& OutSha256)
{
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Path))
	{
		return false;
	}

	const FTCHARToUTF8 Utf8(*Source);
	uint8 Digest[SHA256_DIGEST_LENGTH];
	if (::SHA256(
			reinterpret_cast<const unsigned char*>(Utf8.Get()),
			static_cast<size_t>(Utf8.Length()),
			Digest) == nullptr)
	{
		return false;
	}
	OutSha256 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	return true;
}

bool ResolveAvidScriptSourceLocation(
	const FAvidScriptEditorSourceLocation& Location,
	const FString& ProjectDirectory,
	const FString& ErrorPrefix,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	OutResult = FAvidScriptEditorDiagnosticNavigationResult();
	if (!Location.IsValid() || ProjectDirectory.IsEmpty())
	{
		SetAvidScriptDiagnosticNavigationFailure(
			ErrorPrefix + TEXT("_location_missing"),
			TEXT("The source location is not navigable."),
			OutResult);
		return false;
	}
	if (!FPaths::IsRelative(Location.File))
	{
		SetAvidScriptDiagnosticNavigationFailure(
			ErrorPrefix + TEXT("_source_not_relative"),
			TEXT("The source id must be project-relative."),
			OutResult);
		return false;
	}

	FString ProjectRoot = FPaths::ConvertRelativePathToFull(ProjectDirectory);
	FString SourcePath = FPaths::ConvertRelativePathToFull(ProjectRoot, Location.File);
	FPaths::NormalizeDirectoryName(ProjectRoot);
	FPaths::NormalizeFilename(SourcePath);
	if (!FPaths::IsUnderDirectory(SourcePath, ProjectRoot))
	{
		SetAvidScriptDiagnosticNavigationFailure(
			ErrorPrefix + TEXT("_source_outside_project"),
			TEXT("The source id escapes the project directory."),
			OutResult);
		return false;
	}
	if (!FPaths::FileExists(SourcePath))
	{
		SetAvidScriptDiagnosticNavigationFailure(
			ErrorPrefix + TEXT("_source_missing"),
			TEXT("The source file no longer exists."),
			OutResult);
		return false;
	}
	if (!Location.SourceSha256.IsEmpty())
	{
		FString CurrentSha256;
		if (!TryGetAvidScriptSourceSha256(SourcePath, CurrentSha256)
			|| !CurrentSha256.Equals(Location.SourceSha256, ESearchCase::IgnoreCase))
		{
			SetAvidScriptDiagnosticNavigationFailure(
				ErrorPrefix + TEXT("_source_changed"),
				TEXT("The source changed after this location was produced; rebuild before navigating."),
				OutResult);
			return false;
		}
	}

	OutResult.bSucceeded = true;
	OutResult.AbsoluteSourcePath = SourcePath;
	OutResult.Line = Location.Line;
	OutResult.Column = Location.Column;
	return true;
}
} // namespace

bool FAvidScriptEditorDiagnosticNavigation::Resolve(
	const FAvidScriptEditorSourceLocation& Location,
	const FString& ProjectDirectory,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	return ResolveAvidScriptSourceLocation(
		Location,
		ProjectDirectory,
		TEXT("runtime_frame"),
		OutResult);
}

bool FAvidScriptEditorDiagnosticNavigation::Open(
	const FAvidScriptEditorSourceLocation& Location,
	const FString& ProjectDirectory,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	if (!Resolve(Location, ProjectDirectory, OutResult))
	{
		return false;
	}
	if (!FSourceCodeNavigation::OpenSourceFile(
		OutResult.AbsoluteSourcePath,
		OutResult.Line,
		OutResult.Column))
	{
		SetAvidScriptDiagnosticNavigationFailure(
			TEXT("source_accessor_unavailable"),
			TEXT("The configured source-code accessor could not open the source location."),
			OutResult);
		return false;
	}
	return true;
}

bool FAvidScriptEditorDiagnosticNavigation::Resolve(
	const FAvidScriptFrontendDiagnostic& Diagnostic,
	const FString& ProjectDirectory,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	FAvidScriptEditorSourceLocation Location;
	if (Diagnostic.HasSourceLocation())
	{
		Location.File = Diagnostic.File;
		Location.SourceSha256 = Diagnostic.SourceSha256;
		Location.Line = Diagnostic.GetDisplayLine();
		Location.Column = Diagnostic.GetDisplayColumn();
	}
	return ResolveAvidScriptSourceLocation(
		Location,
		ProjectDirectory,
		TEXT("diagnostic"),
		OutResult);
}

bool FAvidScriptEditorDiagnosticNavigation::Open(
	const FAvidScriptFrontendDiagnostic& Diagnostic,
	const FString& ProjectDirectory,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	if (!Resolve(Diagnostic, ProjectDirectory, OutResult))
	{
		return false;
	}
	if (!FSourceCodeNavigation::OpenSourceFile(
		OutResult.AbsoluteSourcePath,
		OutResult.Line,
		OutResult.Column))
	{
		SetAvidScriptDiagnosticNavigationFailure(
			TEXT("source_accessor_unavailable"),
			TEXT("The configured source-code accessor could not open the diagnostic location."),
			OutResult);
		return false;
	}
	return true;
}
