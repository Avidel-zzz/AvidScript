#include "AvidScriptEditorResultPresentation.h"

#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorCSharpWorkspaceService.h"

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

void AppendAvidScriptRuntimeDiagnosticFrames(
	TArray<FString>& Lines,
	TConstArrayView<FAvidScriptWasmDiagnosticFrame> Frames)
{
	for (const FAvidScriptWasmDiagnosticFrame& Frame : Frames)
	{
		if (Frame.bSourceMapped
			&& !Frame.FunctionName.IsEmpty()
			&& !Frame.SourceFile.IsEmpty()
			&& Frame.Line > 0
			&& Frame.Column > 0)
		{
			Lines.Add(FString::Printf(
				TEXT("at %s (%s:%d:%d)"),
				*Frame.FunctionName,
				*Frame.SourceFile,
				Frame.Line,
				Frame.Column));
		}

		const FString FunctionIdentity = Frame.FunctionIndex == MAX_uint32
			? FString(TEXT("<unknown>"))
			: FString::Printf(TEXT("%u"), Frame.FunctionIndex);
		const FString TokenSuffix = Frame.RawFunctionToken.IsEmpty()
			? FString()
			: FString::Printf(TEXT(" token=%s"), *Frame.RawFunctionToken);
		Lines.Add(FString::Printf(
			TEXT("wasm frame: function=%s offset=0x%08x%s"),
			*FunctionIdentity,
			Frame.FunctionOffset,
			*TokenSuffix));
	}
}

FString MakeAvidScriptPresentationDetails(const FAvidScriptEditorCommandLaunchResult& Result)
{
	TArray<FString> Lines;

	if (!Result.Summary.IsEmpty())
	{
		Lines.Add(Result.Summary);
	}

	AppendAvidScriptRuntimeDiagnosticFrames(
		Lines,
		Result.CommandResult.ReloadApplyResult.RuntimeResult.RuntimeResult.DiagnosticFrames);

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

void AddAvidScriptPresentationDetailLine(TArray<FString>& Lines, const TCHAR* Label, const FString& Value)
{
	if (!Value.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("%s: %s"), Label, *Value));
	}
}

FString MakeAvidScriptPresentationErrorBody(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& Fallback)
{
	if (!ErrorCategory.IsEmpty() || !ErrorMessage.IsEmpty())
	{
		return FString::Printf(TEXT("%s: %s"), *ErrorCategory, *ErrorMessage);
	}

	return Fallback;
}

FString MakeAvidScriptCSharpProfileTemplateDetails(const FAvidScriptEditorCSharpProfileTemplateResult& Result)
{
	TArray<FString> Lines;
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Next action"), Result.NextAction);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Profile"), Result.NormalizedProfilePath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Source"), Result.SourcePath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Report"), Result.ReportPath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Manifest"), Result.ManifestPath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Module"), Result.ModuleId);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Artifact"), Result.ArtifactStem);
	return FString::Join(Lines, TEXT("\n"));
}

FString MakeAvidScriptCSharpWorkspaceDetails(const FAvidScriptEditorCSharpWorkspaceResult& Result)
{
	TArray<FString> Lines;
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Next action"), Result.NextAction);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Workspace"), Result.WorkspaceRoot);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Source"), Result.SourcePath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Project"), Result.ProjectPath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Profile"), Result.ProfilePath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Generated facade"), Result.FacadePath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("IDE binding package"), Result.BindingPackageManifestPath);
	Lines.Add(FString::Printf(
		TEXT("User files: created=%d updated=%d preserved=%d facade_refreshed=%s"),
		Result.CreatedUserFileCount,
		Result.UpdatedUserFileCount,
		Result.PreservedUserFileCount,
		Result.bFacadeRefreshed ? TEXT("true") : TEXT("false")));
	return FString::Join(Lines, TEXT("\n"));
}

FString MakeAvidScriptCSharpProfileBuildAndBindDetails(
	const FString& ProfilePath,
	const FAvidScriptEditorCSharpBuildResult& BuildResult,
	const FAvidScriptEditorComponentBindingResult& BindingResult)
{
	TArray<FString> Lines;
	const FString ManifestPath = !BuildResult.ManifestPath.IsEmpty()
		? BuildResult.ManifestPath
		: BindingResult.NormalizedManifestPath;
	const FString ReportPath = !BuildResult.ReportPath.IsEmpty()
		? BuildResult.ReportPath
		: BindingResult.ReportPath;
	const FString NextAction = !BuildResult.bSucceeded
		? BuildResult.NextAction
		: BindingResult.NextAction;

	AddAvidScriptPresentationDetailLine(Lines, TEXT("Next action"), NextAction);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Profile"), ProfilePath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Source"), BuildResult.SourcePath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Module"), BuildResult.ModuleId);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Report"), ReportPath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Manifest"), ManifestPath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Actor"), BindingResult.ActorPath);
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Stdout"), BuildResult.Stdout);
	Lines.Add(FString::Printf(
		TEXT("Pipeline: cache=%s build=%d frontend=%d semantic=%d guest_ir=%d wasm=%d"),
		BuildResult.SemanticCacheLookup.IsEmpty() ? TEXT("not-reported") : *BuildResult.SemanticCacheLookup,
		BuildResult.BuildInvocationCount,
		BuildResult.FrontendInvocationCount,
		BuildResult.SemanticInvocationCount,
		BuildResult.GuestIrInvocationCount,
		BuildResult.WasmBackendInvocationCount));
	AddAvidScriptPresentationDetailLine(Lines, TEXT("Stderr"), BuildResult.Stderr);
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

FAvidScriptEditorCommandPresentation FAvidScriptEditorResultPresenter::MakeCSharpProfileTemplatePresentation(
	const FAvidScriptEditorCSharpProfileTemplateResult& Result)
{
	FAvidScriptEditorCommandPresentation Presentation;
	Presentation.SourcePath = Result.SourcePath;
	Presentation.ManifestPath = Result.ManifestPath;
	Presentation.Details = MakeAvidScriptCSharpProfileTemplateDetails(Result);

	if (!Result.bSucceeded)
	{
		Presentation.Severity = EAvidScriptEditorPresentationSeverity::Error;
		Presentation.Title = TEXT("AvidScript C# profile template failed");
		Presentation.Body = MakeAvidScriptPresentationErrorBody(
			Result.ErrorCategory,
			Result.ErrorMessage,
			TEXT("C# profile template failed before a detailed diagnostic was produced."));
		return Presentation;
	}

	Presentation.Severity = EAvidScriptEditorPresentationSeverity::Info;
	Presentation.Title = TEXT("AvidScript C# profile ready");
	Presentation.Body = Result.bCreated
		? FString::Printf(TEXT("C# profile template created: %s"), *Result.NormalizedProfilePath)
		: FString::Printf(TEXT("C# profile already exists and was not overwritten: %s"), *Result.NormalizedProfilePath);
	return Presentation;
}

FAvidScriptEditorCommandPresentation FAvidScriptEditorResultPresenter::MakeCSharpWorkspacePresentation(
	const FAvidScriptEditorCSharpWorkspaceResult& Result)
{
	FAvidScriptEditorCommandPresentation Presentation;
	Presentation.SourcePath = Result.SourcePath;
	Presentation.ManifestPath = Result.ManifestPath;
	Presentation.Details = MakeAvidScriptCSharpWorkspaceDetails(Result);

	if (!Result.bSucceeded)
	{
		Presentation.Severity = EAvidScriptEditorPresentationSeverity::Error;
		Presentation.Title = TEXT("AvidScript C# project workspace failed");
		Presentation.Body = MakeAvidScriptPresentationErrorBody(
			Result.ErrorCategory,
			Result.ErrorMessage,
			TEXT("Project C# gameplay workspace failed before a detailed diagnostic was produced."));
		return Presentation;
	}

	Presentation.Severity = EAvidScriptEditorPresentationSeverity::Info;
	Presentation.Title = TEXT("AvidScript C# project workspace ready");
	Presentation.Body = FString::Printf(
		TEXT("Project C# gameplay workspace is ready: created=%d updated=%d preserved=%d."),
		Result.CreatedUserFileCount,
		Result.UpdatedUserFileCount,
		Result.PreservedUserFileCount);
	return Presentation;
}

FAvidScriptEditorCommandPresentation FAvidScriptEditorResultPresenter::MakeCSharpProfileBuildAndBindPresentation(
	const FString& ProfilePath,
	const FAvidScriptEditorCSharpBuildResult& BuildResult,
	const FAvidScriptEditorComponentBindingResult& BindingResult)
{
	FAvidScriptEditorCommandPresentation Presentation;
	Presentation.SourcePath = BuildResult.SourcePath;
	Presentation.ManifestPath = !BuildResult.ManifestPath.IsEmpty()
		? BuildResult.ManifestPath
		: BindingResult.NormalizedManifestPath;
	Presentation.Details = MakeAvidScriptCSharpProfileBuildAndBindDetails(ProfilePath, BuildResult, BindingResult);

	if (!BuildResult.bSucceeded)
	{
		Presentation.Severity = EAvidScriptEditorPresentationSeverity::Error;
		Presentation.Title = TEXT("AvidScript C# profile build failed");
		Presentation.Body = MakeAvidScriptPresentationErrorBody(
			BuildResult.ErrorCategory,
			BuildResult.ErrorMessage,
			TEXT("C# profile build failed before a detailed diagnostic was produced."));
		return Presentation;
	}

	if (!BindingResult.bSucceeded)
	{
		Presentation.Severity = EAvidScriptEditorPresentationSeverity::Warning;
		Presentation.Title = TEXT("AvidScript C# profile binding failed");
		Presentation.Body = MakeAvidScriptPresentationErrorBody(
			BindingResult.ErrorCategory,
			BindingResult.ErrorMessage,
			TEXT("C# profile build succeeded, but binding to the selected Actor failed."));
		return Presentation;
	}

	Presentation.Severity = EAvidScriptEditorPresentationSeverity::Info;
	Presentation.Title = TEXT("AvidScript C# profile bound");
	Presentation.Body = FString::Printf(
		TEXT("Module %s bound to %s."),
		*BuildResult.ModuleId,
		*BindingResult.ActorPath);
	return Presentation;
}
