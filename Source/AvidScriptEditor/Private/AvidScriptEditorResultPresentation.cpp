#include "AvidScriptEditorResultPresentation.h"

#include "Misc/Paths.h"

namespace
{
FString GetAvidScriptPresentationSourceLabel(const FString& SourcePath)
{
	if (SourcePath.IsEmpty())
	{
		return TEXT("<unknown source>");
	}

	return FPaths::GetCleanFilename(SourcePath);
}

FString GetAvidScriptPresentationModuleLabel(const FAvidScriptEditorCommandLaunchResult& Result)
{
	const FString& ModuleId = Result.CommandResult.CompileResult.Manifest.ModuleId;
	if (!ModuleId.IsEmpty())
	{
		return ModuleId;
	}

	return GetAvidScriptPresentationSourceLabel(Result.SourcePath);
}

FString MakeAvidScriptPresentationDetails(const FAvidScriptEditorCommandLaunchResult& Result)
{
	TArray<FString> Lines;

	if (!Result.Summary.IsEmpty())
	{
		Lines.Add(Result.Summary);
	}

	if (!Result.CommandResult.NextAction.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Next action: %s"), *Result.CommandResult.NextAction));
	}

	if (!Result.ReportPath.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Report: %s"), *Result.ReportPath));
	}

	if (!Result.ManifestPath.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Manifest: %s"), *Result.ManifestPath));
	}

	if (!Result.SourcePath.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Source: %s"), *Result.SourcePath));
	}

	return FString::Join(Lines, TEXT("\n"));
}
} // namespace

FAvidScriptEditorCommandPresentation FAvidScriptEditorResultPresenter::MakePresentation(
	const FAvidScriptEditorCommandLaunchResult& Result)
{
	FAvidScriptEditorCommandPresentation Presentation;
	Presentation.SourcePath = Result.SourcePath;
	Presentation.ManifestPath = Result.ManifestPath;
	Presentation.Details = MakeAvidScriptPresentationDetails(Result);

	if (!Result.bSucceeded)
	{
		Presentation.Severity = EAvidScriptEditorPresentationSeverity::Error;
		Presentation.Title = TEXT("AvidScript command failed");

		if (!Result.CommandResult.ErrorCategory.IsEmpty() || !Result.CommandResult.ErrorMessage.IsEmpty())
		{
			Presentation.Body = FString::Printf(
				TEXT("%s: %s"),
				*Result.CommandResult.ErrorCategory,
				*Result.CommandResult.ErrorMessage);
		}
		else
		{
			Presentation.Body = TEXT("AvidScript command failed before a detailed diagnostic was produced.");
		}

		return Presentation;
	}

	if (!Result.bReloadApplied || Result.CommandResult.Status == EAvidScriptEditorCommandStatus::GeneratedOnly)
	{
		Presentation.Severity = EAvidScriptEditorPresentationSeverity::Warning;
		Presentation.Title = TEXT("AvidScript Generated");
		Presentation.Body = FString::Printf(
			TEXT("No live reload was applied for %s."),
			*GetAvidScriptPresentationSourceLabel(Result.SourcePath));
		return Presentation;
	}

	Presentation.Severity = EAvidScriptEditorPresentationSeverity::Info;
	Presentation.Title = TEXT("AvidScript Reload applied");
	Presentation.Body = FString::Printf(
		TEXT("Module %s reloaded from %s."),
		*GetAvidScriptPresentationModuleLabel(Result),
		*GetAvidScriptPresentationSourceLabel(Result.SourcePath));
	return Presentation;
}