#include "AvidScriptEditorComponentBindingService.h"

#include "AvidScriptComponent.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Selection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FString NormalizeAvidScriptEditorComponentBindingPath(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return FString();
	}

	FString ResolvedPath = Path;
	if (FPaths::IsRelative(ResolvedPath))
	{
		ResolvedPath = FPaths::Combine(FPaths::ProjectDir(), ResolvedPath);
	}

	ResolvedPath = FPaths::ConvertRelativePathToFull(ResolvedPath);
	FPaths::NormalizeFilename(ResolvedPath);
	return ResolvedPath;
}

void SetAvidScriptEditorComponentBindingFailure(
	const EAvidScriptEditorComponentBindingStatus Status,
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorComponentBindingResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.Status = Status;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}

AActor* GetAvidScriptEditorSelectedActor()
{
	if (GEditor == nullptr)
	{
		return nullptr;
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (SelectedActors == nullptr)
	{
		return nullptr;
	}

	for (FSelectionIterator It(*SelectedActors); It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(*It))
		{
			return Actor;
		}
	}

	return nullptr;
}

bool TryGetAvidScriptCSharpReportManifestPath(
	const FString& ReportPath,
	FString& OutManifestPath,
	FString& OutErrorCategory,
	FString& OutErrorMessage,
	FString& OutNextAction)
{
	OutManifestPath.Reset();
	OutErrorCategory.Reset();
	OutErrorMessage.Reset();
	OutNextAction.Reset();

	const FString NormalizedReportPath = NormalizeAvidScriptEditorComponentBindingPath(ReportPath);
	if (NormalizedReportPath.IsEmpty())
	{
		OutErrorCategory = TEXT("report_path_missing");
		OutErrorMessage = TEXT("C# build report path is empty.");
		OutNextAction = TEXT("run the C# build step or provide actor_lifecycle.csharp.report.json");
		return false;
	}

	if (!FPaths::FileExists(NormalizedReportPath))
	{
		OutErrorCategory = TEXT("report_missing");
		OutErrorMessage = FString::Printf(TEXT("C# build report does not exist: %s"), *NormalizedReportPath);
		OutNextAction = TEXT("run BuildCSharpActorLifecycle.ps1 before applying the C# manifest");
		return false;
	}

	FString ReportJson;
	if (!FFileHelper::LoadFileToString(ReportJson, *NormalizedReportPath))
	{
		OutErrorCategory = TEXT("report_read_failed");
		OutErrorMessage = FString::Printf(TEXT("C# build report could not be read: %s"), *NormalizedReportPath);
		OutNextAction = TEXT("verify the report file is readable and rerun the C# build step");
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReportJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		OutErrorCategory = TEXT("report_invalid_json");
		OutErrorMessage = FString::Printf(TEXT("C# build report is not valid JSON: %s"), *NormalizedReportPath);
		OutNextAction = TEXT("rerun BuildCSharpActorLifecycle.ps1 to regenerate the report");
		return false;
	}

	FString Language;
	RootObject->TryGetStringField(TEXT("language"), Language);
	if (!Language.Equals(TEXT("csharp"), ESearchCase::IgnoreCase))
	{
		OutErrorCategory = TEXT("report_not_csharp");
		OutErrorMessage = FString::Printf(TEXT("Build report language is not csharp: %s"), *Language);
		OutNextAction = TEXT("choose a C# AvidScript build report");
		return false;
	}

	FString Result;
	RootObject->TryGetStringField(TEXT("result"), Result);
	const bool bDirectAbiSupported = RootObject->GetBoolField(TEXT("direct_abi_supported"));
	if (!Result.Equals(TEXT("direct_abi_built"), ESearchCase::IgnoreCase) || !bDirectAbiSupported)
	{
		OutErrorCategory = TEXT("report_not_direct_abi");
		OutErrorMessage = FString::Printf(
			TEXT("C# build report is not a direct ABI success: result=%s direct_abi_supported=%s"),
			*Result,
			bDirectAbiSupported ? TEXT("true") : TEXT("false"));
		OutNextAction = TEXT("fix the C# source/toolchain and rebuild until the report says direct_abi_built");
		return false;
	}

	const TSharedPtr<FJsonObject>* ArtifactsObject = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("artifacts"), ArtifactsObject) || ArtifactsObject == nullptr || !ArtifactsObject->IsValid())
	{
		OutErrorCategory = TEXT("report_artifacts_missing");
		OutErrorMessage = TEXT("C# build report does not contain an artifacts object.");
		OutNextAction = TEXT("rerun BuildCSharpActorLifecycle.ps1 with the current script version");
		return false;
	}

	FString ManifestPath;
	(*ArtifactsObject)->TryGetStringField(TEXT("manifest_file"), ManifestPath);
	if (ManifestPath.IsEmpty())
	{
		OutErrorCategory = TEXT("report_manifest_missing");
		OutErrorMessage = TEXT("C# build report does not contain artifacts.manifest_file.");
		OutNextAction = TEXT("rebuild the C# script and check that direct ABI output was generated");
		return false;
	}

	OutManifestPath = NormalizeAvidScriptEditorComponentBindingPath(ManifestPath);
	return true;
}
} // namespace

bool FAvidScriptEditorComponentBindingService::ApplyManifestToActor(
	const FAvidScriptEditorComponentBindingRequest& Request,
	FAvidScriptEditorComponentBindingResult& OutResult)
{
	OutResult = FAvidScriptEditorComponentBindingResult();
	OutResult.Component = nullptr;

	AActor* Actor = Request.Actor;
	if (Actor == nullptr)
	{
		SetAvidScriptEditorComponentBindingFailure(
			EAvidScriptEditorComponentBindingStatus::ActorMissing,
			TEXT("actor_missing"),
			TEXT("No target actor was provided."),
			TEXT("select an Actor or pass a valid AActor pointer"),
			OutResult);
		return false;
	}

	OutResult.ActorPath = Actor->GetPathName();
	const FString NormalizedManifestPath = NormalizeAvidScriptEditorComponentBindingPath(Request.ManifestPath);
	OutResult.NormalizedManifestPath = NormalizedManifestPath;
	if (NormalizedManifestPath.IsEmpty())
	{
		SetAvidScriptEditorComponentBindingFailure(
			EAvidScriptEditorComponentBindingStatus::ManifestPathMissing,
			TEXT("manifest_path_missing"),
			TEXT("AvidScript manifest path is empty."),
			TEXT("build a C# script and apply the generated .avidscript.json manifest"),
			OutResult);
		return false;
	}

	if (Request.bRequireManifestFileExists && !FPaths::FileExists(NormalizedManifestPath))
	{
		SetAvidScriptEditorComponentBindingFailure(
			EAvidScriptEditorComponentBindingStatus::ManifestMissing,
			TEXT("manifest_missing"),
			FString::Printf(TEXT("AvidScript manifest does not exist: %s"), *NormalizedManifestPath),
			TEXT("rerun the script build step or choose an existing manifest"),
			OutResult);
		return false;
	}

	UAvidScriptComponent* Component = Actor->FindComponentByClass<UAvidScriptComponent>();
	bool bRegisterNewComponent = false;
	if (Component == nullptr)
	{
		if (!Request.bCreateComponentIfMissing)
		{
			SetAvidScriptEditorComponentBindingFailure(
				EAvidScriptEditorComponentBindingStatus::ComponentMissing,
				TEXT("component_missing"),
				TEXT("Target actor does not have a UAvidScriptComponent."),
				TEXT("add an AvidScript component or allow the binding service to create one"),
				OutResult);
			return false;
		}

		Actor->Modify();
		Component = NewObject<UAvidScriptComponent>(Actor, UAvidScriptComponent::StaticClass(), NAME_None, RF_Transactional);
		if (Component == nullptr)
		{
			SetAvidScriptEditorComponentBindingFailure(
				EAvidScriptEditorComponentBindingStatus::ComponentCreateFailed,
				TEXT("component_create_failed"),
				TEXT("Could not create UAvidScriptComponent on the target actor."),
				TEXT("check actor validity and retry"),
				OutResult);
			return false;
		}

		bRegisterNewComponent = true;
		OutResult.bCreatedComponent = true;
	}

	Component->Modify();
	Component->SetScriptManifestPath(NormalizedManifestPath);
	if (bRegisterNewComponent)
	{
		Actor->AddInstanceComponent(Component);
		Component->OnComponentCreated();
		Component->RegisterComponent();
	}
	Actor->MarkPackageDirty();

	OutResult.bSucceeded = true;
	OutResult.Status = EAvidScriptEditorComponentBindingStatus::Bound;
	OutResult.Component = Component;
	return true;
}

bool FAvidScriptEditorComponentBindingService::ApplyManifestToSelectedActor(
	const FString& ManifestPath,
	FAvidScriptEditorComponentBindingResult& OutResult)
{
	OutResult = FAvidScriptEditorComponentBindingResult();

	AActor* Actor = GetAvidScriptEditorSelectedActor();
	if (Actor == nullptr)
	{
		SetAvidScriptEditorComponentBindingFailure(
			GEditor == nullptr
				? EAvidScriptEditorComponentBindingStatus::SelectionUnavailable
				: EAvidScriptEditorComponentBindingStatus::ActorMissing,
			GEditor == nullptr ? TEXT("selection_unavailable") : TEXT("selected_actor_missing"),
			GEditor == nullptr
				? TEXT("GEditor selection is not available.")
				: TEXT("No Actor is currently selected."),
			TEXT("select a target Actor in the level and retry"),
			OutResult);
		return false;
	}

	FAvidScriptEditorComponentBindingRequest Request;
	Request.Actor = Actor;
	Request.ManifestPath = ManifestPath;
	return ApplyManifestToActor(Request, OutResult);
}

bool FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(
	const FString& ReportPath,
	AActor* Actor,
	FAvidScriptEditorComponentBindingResult& OutResult)
{
	OutResult = FAvidScriptEditorComponentBindingResult();
	OutResult.ReportPath = NormalizeAvidScriptEditorComponentBindingPath(ReportPath);

	FString ManifestPath;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	if (!TryGetAvidScriptCSharpReportManifestPath(
			ReportPath,
			ManifestPath,
			ErrorCategory,
			ErrorMessage,
			NextAction))
	{
		SetAvidScriptEditorComponentBindingFailure(
			ErrorCategory == TEXT("report_missing")
				? EAvidScriptEditorComponentBindingStatus::ReportMissing
				: EAvidScriptEditorComponentBindingStatus::ReportInvalid,
			ErrorCategory,
			ErrorMessage,
			NextAction,
			OutResult);
		return false;
	}

	FAvidScriptEditorComponentBindingRequest Request;
	Request.Actor = Actor;
	Request.ManifestPath = ManifestPath;
	const bool bApplied = ApplyManifestToActor(Request, OutResult);
	OutResult.ReportPath = NormalizeAvidScriptEditorComponentBindingPath(ReportPath);
	return bApplied;
}

bool FAvidScriptEditorComponentBindingService::ApplyCSharpReportToSelectedActor(
	const FString& ReportPath,
	FAvidScriptEditorComponentBindingResult& OutResult)
{
	OutResult = FAvidScriptEditorComponentBindingResult();
	OutResult.ReportPath = NormalizeAvidScriptEditorComponentBindingPath(ReportPath);

	AActor* Actor = GetAvidScriptEditorSelectedActor();
	if (Actor == nullptr)
	{
		SetAvidScriptEditorComponentBindingFailure(
			GEditor == nullptr
				? EAvidScriptEditorComponentBindingStatus::SelectionUnavailable
				: EAvidScriptEditorComponentBindingStatus::ActorMissing,
			GEditor == nullptr ? TEXT("selection_unavailable") : TEXT("selected_actor_missing"),
			GEditor == nullptr
				? TEXT("GEditor selection is not available.")
				: TEXT("No Actor is currently selected."),
			TEXT("select a target Actor in the level and retry"),
			OutResult);
		return false;
	}

	return ApplyCSharpReportToActor(ReportPath, Actor, OutResult);
}
