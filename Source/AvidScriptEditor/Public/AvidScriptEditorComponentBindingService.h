#pragma once

#include "CoreMinimal.h"
#include "AvidScriptWasmReloadTypes.h"

class AActor;
class UAvidScriptComponent;

enum class EAvidScriptEditorComponentBindingStatus : uint8
{
	Unknown,
	Bound,
	ActorMissing,
	SelectionUnavailable,
	ManifestPathMissing,
	ManifestMissing,
	ReportMissing,
	ReportInvalid,
	ComponentMissing,
	ComponentCreateFailed,
	ReloadRejected
};

struct FAvidScriptEditorComponentBindingRequest
{
	AActor* Actor = nullptr;
	FString ManifestPath;
	bool bCreateComponentIfMissing = true;
	bool bRequireManifestFileExists = true;
};

struct FAvidScriptEditorComponentBindingResult
{
	bool bSucceeded = false;
	bool bCreatedComponent = false;
	bool bReloadAttempted = false;
	bool bReloadApplied = false;
	EAvidScriptEditorComponentBindingStatus Status = EAvidScriptEditorComponentBindingStatus::Unknown;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString NormalizedManifestPath;
	FString ReportPath;
	FString ActorPath;
	UAvidScriptComponent* Component = nullptr;
	FAvidScriptWasmReloadResult RuntimeResult;
};

class FAvidScriptEditorComponentBindingService
{
public:
	static bool ApplyManifestToActor(
		const FAvidScriptEditorComponentBindingRequest& Request,
		FAvidScriptEditorComponentBindingResult& OutResult);

	static bool ApplyManifestToSelectedActor(
		const FString& ManifestPath,
		FAvidScriptEditorComponentBindingResult& OutResult);

	static bool ApplyCSharpReportToActor(
		const FString& ReportPath,
		AActor* Actor,
		FAvidScriptEditorComponentBindingResult& OutResult);

	static bool ApplyCSharpReportToSelectedActor(
		const FString& ReportPath,
		FAvidScriptEditorComponentBindingResult& OutResult);
};
