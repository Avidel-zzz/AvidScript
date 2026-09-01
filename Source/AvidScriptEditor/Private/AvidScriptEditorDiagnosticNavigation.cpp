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
} // namespace

bool FAvidScriptEditorDiagnosticNavigation::Resolve(
	const FAvidScriptFrontendDiagnostic& Diagnostic,
	const FString& ProjectDirectory,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	OutResult = FAvidScriptEditorDiagnosticNavigationResult();
	if (!Diagnostic.HasSourceLocation() || ProjectDirectory.IsEmpty())
	{
		SetAvidScriptDiagnosticNavigationFailure(
			TEXT("diagnostic_location_missing"),
			TEXT("The diagnostic does not contain a navigable source location."),
			OutResult);
		return false;
	}
	if (!FPaths::IsRelative(Diagnostic.File))
	{
		SetAvidScriptDiagnosticNavigationFailure(
			TEXT("diagnostic_source_not_relative"),
			TEXT("The diagnostic source id must be project-relative."),
			OutResult);
		return false;
	}

	FString ProjectRoot = FPaths::ConvertRelativePathToFull(ProjectDirectory);
	FString SourcePath = FPaths::ConvertRelativePathToFull(ProjectRoot, Diagnostic.File);
	FPaths::NormalizeDirectoryName(ProjectRoot);
	FPaths::NormalizeFilename(SourcePath);
	if (!FPaths::IsUnderDirectory(SourcePath, ProjectRoot))
	{
		SetAvidScriptDiagnosticNavigationFailure(
			TEXT("diagnostic_source_outside_project"),
			TEXT("The diagnostic source id escapes the project directory."),
			OutResult);
		return false;
	}
	if (!FPaths::FileExists(SourcePath))
	{
		SetAvidScriptDiagnosticNavigationFailure(
			TEXT("diagnostic_source_missing"),
			TEXT("The diagnostic source file no longer exists."),
			OutResult);
		return false;
	}
	if (!Diagnostic.SourceSha256.IsEmpty())
	{
		FString CurrentSha256;
		if (!TryGetAvidScriptSourceSha256(SourcePath, CurrentSha256)
			|| !CurrentSha256.Equals(Diagnostic.SourceSha256, ESearchCase::IgnoreCase))
		{
			SetAvidScriptDiagnosticNavigationFailure(
				TEXT("diagnostic_source_changed"),
				TEXT("The source changed after this diagnostic was produced; rebuild before navigating."),
				OutResult);
			return false;
		}
	}

	OutResult.bSucceeded = true;
	OutResult.AbsoluteSourcePath = SourcePath;
	OutResult.Line = Diagnostic.GetDisplayLine();
	OutResult.Column = Diagnostic.GetDisplayColumn();
	return true;
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
