#include "AvidScriptEditorCompileService.h"

#include "Misc/Paths.h"

namespace
{
bool IsAvidScriptCompileFieldIdentifierStart(const TCHAR Character)
{
	return (Character >= TEXT('A') && Character <= TEXT('Z')) ||
		(Character >= TEXT('a') && Character <= TEXT('z')) ||
		Character == TEXT('_');
}

bool IsAvidScriptCompileFieldIdentifier(const TCHAR Character)
{
	return IsAvidScriptCompileFieldIdentifierStart(Character) ||
		(Character >= TEXT('0') && Character <= TEXT('9'));
}

bool TryExtractAvidScriptCompileField(
	const FString& Text,
	const FString& FieldName,
	FString& OutValue)
{
	const FString Prefix = FieldName + TEXT("=");
	int32 PrefixIndex = INDEX_NONE;
	if (!Text.FindChar(Prefix[0], PrefixIndex))
	{
		return false;
	}

	PrefixIndex = Text.Find(Prefix, ESearchCase::CaseSensitive, ESearchDir::FromStart);
	if (PrefixIndex == INDEX_NONE)
	{
		return false;
	}

	const int32 ValueStart = PrefixIndex + Prefix.Len();
	int32 ValueEnd = Text.Len();
	for (int32 Index = ValueStart; Index < Text.Len(); ++Index)
	{
		if (Text[Index] != TEXT(' '))
		{
			continue;
		}

		int32 FieldProbe = Index + 1;
		if (FieldProbe >= Text.Len() || !IsAvidScriptCompileFieldIdentifierStart(Text[FieldProbe]))
		{
			continue;
		}

		while (FieldProbe < Text.Len() && IsAvidScriptCompileFieldIdentifier(Text[FieldProbe]))
		{
			++FieldProbe;
		}

		if (FieldProbe < Text.Len() && Text[FieldProbe] == TEXT('='))
		{
			ValueEnd = Index;
			break;
		}
	}

	OutValue = Text.Mid(ValueStart, ValueEnd - ValueStart).TrimStartAndEnd();
	return !OutValue.IsEmpty();
}

FString NormalizeAvidScriptCompilePath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}

	return Path;
}

const FAvidScriptFrontendDiagnostic* GetFirstAvidScriptCompileErrorDiagnostic(
	const FAvidScriptFrontendReport& Report)
{
	for (const FAvidScriptFrontendDiagnostic& Diagnostic : Report.Diagnostics)
	{
		if (Diagnostic.IsError())
		{
			return &Diagnostic;
		}
	}

	return nullptr;
}

FString GetAvidScriptCompileManifestPathFromReport(
	const FAvidScriptFrontendReport& Report,
	const FString& ManifestPathOverride)
{
	if (!ManifestPathOverride.IsEmpty())
	{
		return NormalizeAvidScriptCompilePath(ManifestPathOverride);
	}

	const FAvidScriptFrontendBuildEvent* LastEvent = Report.GetLastBuildEvent();
	if (LastEvent != nullptr)
	{
		if (const FString* ManifestField = LastEvent->Fields.Find(TEXT("manifest")))
		{
			return NormalizeAvidScriptCompilePath(*ManifestField);
		}
	}

	for (const FString& Line : Report.RawOutput)
	{
		FString ManifestPath;
		if (TryExtractAvidScriptCompileField(Line, TEXT("manifest"), ManifestPath))
		{
			return NormalizeAvidScriptCompilePath(ManifestPath);
		}
	}

	return FString();
}

void SetAvidScriptCompileFailure(
	const EAvidScriptEditorCompileStatus Status,
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	FAvidScriptEditorCompileResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.bReloadable = false;
	OutResult.Status = Status;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
}

void SetAvidScriptCompileDiagnosticsFailure(
	const FAvidScriptFrontendInvocationResult& InvocationResult,
	FAvidScriptEditorCompileResult& OutResult)
{
	const FAvidScriptFrontendDiagnostic* Diagnostic = GetFirstAvidScriptCompileErrorDiagnostic(InvocationResult.Report);
	FString ErrorMessage = TEXT("AvidScript frontend reported diagnostics.");
	if (Diagnostic != nullptr)
	{
		ErrorMessage = Diagnostic->Message;
	}

	SetAvidScriptCompileFailure(
		EAvidScriptEditorCompileStatus::FailedDiagnostics,
		TEXT("frontend_diagnostics"),
		ErrorMessage,
		OutResult);
}
} // namespace

bool FAvidScriptEditorCompileService::Compile(
	const FAvidScriptEditorCompileConfig& Config,
	FAvidScriptEditorCompileResult& OutResult)
{
	FAvidScriptFrontendInvocationResult InvocationResult;
	FAvidScriptFrontendInvoker::Invoke(Config.InvocationConfig, InvocationResult);
	return EvaluateInvocationResult(InvocationResult, Config.ManifestPathOverride, OutResult);
}

bool FAvidScriptEditorCompileService::EvaluateInvocationResult(
	const FAvidScriptFrontendInvocationResult& InvocationResult,
	FAvidScriptEditorCompileResult& OutResult)
{
	return EvaluateInvocationResult(InvocationResult, FString(), OutResult);
}

bool FAvidScriptEditorCompileService::EvaluateInvocationResult(
	const FAvidScriptFrontendInvocationResult& InvocationResult,
	const FString& ManifestPathOverride,
	FAvidScriptEditorCompileResult& OutResult)
{
	OutResult = FAvidScriptEditorCompileResult();
	OutResult.InvocationResult = InvocationResult;

	if (!InvocationResult.ReportLoadResult.bSucceeded)
	{
		SetAvidScriptCompileFailure(
			EAvidScriptEditorCompileStatus::FailedInvocation,
			InvocationResult.ErrorCategory.IsEmpty() ? TEXT("frontend_invocation_failed") : InvocationResult.ErrorCategory,
			InvocationResult.ErrorMessage,
			OutResult);
		return false;
	}

	if (!InvocationResult.Report.bSucceeded || InvocationResult.Report.HasErrorDiagnostics())
	{
		SetAvidScriptCompileDiagnosticsFailure(InvocationResult, OutResult);
		return false;
	}

	if (!InvocationResult.bSucceeded)
	{
		SetAvidScriptCompileFailure(
			EAvidScriptEditorCompileStatus::FailedInvocation,
			InvocationResult.ErrorCategory.IsEmpty() ? TEXT("frontend_invocation_failed") : InvocationResult.ErrorCategory,
			InvocationResult.ErrorMessage,
			OutResult);
		return false;
	}

	const FAvidScriptFrontendBuildEvent* LastEvent = InvocationResult.Report.GetLastBuildEvent();
	if (LastEvent == nullptr)
	{
		SetAvidScriptCompileFailure(
			EAvidScriptEditorCompileStatus::FailedInvocation,
			TEXT("frontend_report_missing_build_event"),
			TEXT("AvidScript frontend report did not include a build event."),
			OutResult);
		return false;
	}

	if (LastEvent->Result.Equals(TEXT("generated"), ESearchCase::IgnoreCase))
	{
		OutResult.bSucceeded = true;
		OutResult.bReloadable = false;
		OutResult.Status = EAvidScriptEditorCompileStatus::SucceededGeneratedOnly;
		return true;
	}

	if (!LastEvent->Result.Equals(TEXT("built"), ESearchCase::IgnoreCase))
	{
		SetAvidScriptCompileFailure(
			EAvidScriptEditorCompileStatus::FailedDiagnostics,
			LastEvent->Result.IsEmpty() ? TEXT("frontend_build_not_reloadable") : LastEvent->Result,
			TEXT("AvidScript frontend report did not produce a reloadable build result."),
			OutResult);
		return false;
	}

	OutResult.ManifestPath = GetAvidScriptCompileManifestPathFromReport(InvocationResult.Report, ManifestPathOverride);
	if (OutResult.ManifestPath.IsEmpty())
	{
		SetAvidScriptCompileFailure(
			EAvidScriptEditorCompileStatus::FailedManifest,
			TEXT("manifest_path_missing"),
			TEXT("AvidScript frontend report did not include a manifest path for the built artifact."),
			OutResult);
		return false;
	}

	OutResult.bManifestLoadAttempted = true;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			OutResult.ManifestPath,
			OutResult.Manifest,
			OutResult.Bytecode,
			OutResult.ManifestLoadResult))
	{
		SetAvidScriptCompileFailure(
			EAvidScriptEditorCompileStatus::FailedManifest,
			OutResult.ManifestLoadResult.ErrorCategory,
			OutResult.ManifestLoadResult.ErrorMessage,
			OutResult);
		OutResult.NextAction = OutResult.ManifestLoadResult.NextAction;
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.bReloadable = true;
	OutResult.Status = EAvidScriptEditorCompileStatus::SucceededReloadable;
	return true;
}
