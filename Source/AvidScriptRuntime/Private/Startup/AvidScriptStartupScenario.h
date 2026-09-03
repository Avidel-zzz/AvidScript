#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptStartupTargetMode : uint8
{
	WorldHost,
	ExistingActor,
	SpawnActor
};

struct FAvidScriptStartupTarget
{
	EAvidScriptStartupTargetMode Mode = EAvidScriptStartupTargetMode::WorldHost;
	FString ClassPath;
	FName RequiredTag;
	int32 MaxInstances = 1;
	TArray<FTransform> SpawnTransforms;
};

struct FAvidScriptStartupBinding
{
	FString ModuleId;
	FAvidScriptStartupTarget Target;
};

struct FAvidScriptStartupScenario
{
	FString ScenarioId;
	TArray<FString> Worlds;
	TArray<FAvidScriptStartupBinding> Bindings;
};

struct FAvidScriptStartupDocument
{
	int32 SchemaVersion = 0;
	TArray<FAvidScriptStartupScenario> Scenarios;
};

struct FAvidScriptStartupLoadResult
{
	FString ErrorCategory;
	FString ErrorMessage;

	bool IsSuccess() const
	{
		return ErrorCategory.IsEmpty();
	}
};

namespace AvidScript::Startup
{
bool ParseDocument(
	const FString& Json,
	FAvidScriptStartupDocument& OutDocument,
	FAvidScriptStartupLoadResult& OutResult);

bool LoadDocumentFile(
	const FString& Path,
	FAvidScriptStartupDocument& OutDocument,
	FAvidScriptStartupLoadResult& OutResult);

const FAvidScriptStartupScenario* FindScenario(
	const FAvidScriptStartupDocument& Document,
	const FString& ScenarioId);

bool IsWorldAllowed(
	const FAvidScriptStartupScenario& Scenario,
	const FString& WorldPackageName);
}

