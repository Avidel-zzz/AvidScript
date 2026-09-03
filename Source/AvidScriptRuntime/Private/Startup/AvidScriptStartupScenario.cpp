#include "Startup/AvidScriptStartupScenario.h"

#include "Packages/AvidScriptModulePackageSchema.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"

namespace AvidScript::Startup
{
namespace
{
constexpr int64 MaximumDocumentBytes = 1024 * 1024;
constexpr int32 MaximumScenarios = 16;
constexpr int32 MaximumWorlds = 32;
constexpr int32 MaximumBindings = 64;
constexpr int32 MaximumSpawnTransforms = 64;
constexpr int32 MaximumIdentifierLength = 64;
constexpr int32 MaximumPathLength = 1024;

void SetFailure(
	FAvidScriptStartupLoadResult& OutResult,
	const TCHAR* Category,
	const FString& Details)
{
	OutResult.ErrorCategory = Category;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript startup scenario rejected | category=%s | details=%s"),
		Category,
		*Details);
}

bool HasExactFields(
	const FJsonObject& Object,
	std::initializer_list<const TCHAR*> Required,
	std::initializer_list<const TCHAR*> Optional = {})
{
	TSet<FString> Allowed;
	for (const TCHAR* Field : Required)
	{
		Allowed.Add(Field);
		if (!Object.HasField(Field))
		{
			return false;
		}
	}
	for (const TCHAR* Field : Optional)
	{
		Allowed.Add(Field);
	}
	if (Object.Values.Num() < static_cast<int32>(Required.size())
		|| Object.Values.Num() > Allowed.Num())
	{
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
	{
		if (!Allowed.Contains(Pair.Key))
		{
			return false;
		}
	}
	return true;
}

bool HasNoDuplicateObjectKeys(
	const FString& Json,
	FString& OutDuplicateKey)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TArray<TSet<FString>> ObjectScopes;
	EJsonNotation Notation = EJsonNotation::Error;
	while (Reader->ReadNext(Notation))
	{
		if (Notation == EJsonNotation::Error)
		{
			return false;
		}
		const FString& Identifier = Reader->GetIdentifier();
		if (!Identifier.IsEmpty() && !ObjectScopes.IsEmpty())
		{
			if (ObjectScopes.Last().Contains(Identifier))
			{
				OutDuplicateKey = Identifier;
				return false;
			}
			ObjectScopes.Last().Add(Identifier);
		}
		if (Notation == EJsonNotation::ObjectStart)
		{
			ObjectScopes.Emplace();
		}
		else if (Notation == EJsonNotation::ObjectEnd)
		{
			if (ObjectScopes.IsEmpty())
			{
				return false;
			}
			ObjectScopes.Pop(EAllowShrinking::No);
		}
	}
	return ObjectScopes.IsEmpty();
}

bool IsNormalizedIdentifier(const FString& Value)
{
	if (Value.IsEmpty() || Value.Len() > MaximumIdentifierLength
		|| Value[0] < TEXT('a') || Value[0] > TEXT('z'))
	{
		return false;
	}
	for (int32 Index = 1; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if ((Character < TEXT('a') || Character > TEXT('z'))
			&& !FChar::IsDigit(Character)
			&& Character != TEXT('_')
			&& Character != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

bool TryReadFiniteNumber(
	const TSharedPtr<FJsonValue>& Value,
	double& OutNumber)
{
	return Value.IsValid()
		&& Value->Type == EJson::Number
		&& Value->TryGetNumber(OutNumber)
		&& FMath::IsFinite(OutNumber);
}

bool TryReadVector(
	const FJsonObject& Object,
	const TCHAR* Field,
	FVector& OutVector)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.TryGetArrayField(Field, Values)
		|| Values == nullptr
		|| Values->Num() != 3)
	{
		return false;
	}
	double Components[3]{};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (!TryReadFiniteNumber((*Values)[Index], Components[Index])
			|| FMath::Abs(Components[Index]) > 10000000.0)
		{
			return false;
		}
	}
	OutVector = FVector(Components[0], Components[1], Components[2]);
	return true;
}

bool TryReadTransform(
	const TSharedPtr<FJsonObject>& Object,
	FTransform& OutTransform)
{
	if (!Object.IsValid()
		|| !HasExactFields(
			*Object,
			{ TEXT("location"), TEXT("rotation"), TEXT("scale") }))
	{
		return false;
	}
	FVector Location;
	FVector RotationVector;
	FVector Scale;
	if (!TryReadVector(*Object, TEXT("location"), Location)
		|| !TryReadVector(*Object, TEXT("rotation"), RotationVector)
		|| !TryReadVector(*Object, TEXT("scale"), Scale)
		|| Scale.GetAbsMin() < UE_SMALL_NUMBER)
	{
		return false;
	}
	OutTransform = FTransform(
		FRotator(RotationVector.X, RotationVector.Y, RotationVector.Z),
		Location,
		Scale);
	return true;
}

bool TryReadClassPath(const FJsonObject& Object, FString& OutClassPath)
{
	if (!Object.TryGetStringField(TEXT("class_path"), OutClassPath)
		|| OutClassPath.IsEmpty()
		|| OutClassPath.Len() > MaximumPathLength)
	{
		return false;
	}
	const FSoftClassPath ClassPath(OutClassPath);
	return ClassPath.IsValid() && ClassPath.GetSubPathString().IsEmpty();
}

bool TryReadExactInteger(
	const FJsonObject& Object,
	const TCHAR* Field,
	int32& OutValue)
{
	const TSharedPtr<FJsonValue> Value = Object.TryGetField(Field);
	double Number = 0.0;
	if (!Value.IsValid()
		|| Value->Type != EJson::Number
		|| !Value->TryGetNumber(Number)
		|| !FMath::IsFinite(Number)
		|| Number < static_cast<double>(MIN_int32)
		|| Number > static_cast<double>(MAX_int32))
	{
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return Number == static_cast<double>(OutValue);
}

bool ParseTarget(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptStartupTarget& OutTarget,
	FAvidScriptStartupLoadResult& OutResult)
{
	FString Mode;
	if (!Object.IsValid()
		|| !Object->TryGetStringField(TEXT("mode"), Mode))
	{
		SetFailure(OutResult, TEXT("target_invalid"), TEXT("target mode is missing"));
		return false;
	}

	if (Mode == TEXT("world_host"))
	{
		if (!HasExactFields(*Object, { TEXT("mode") }))
		{
			SetFailure(OutResult, TEXT("target_invalid"), TEXT("world_host fields are invalid"));
			return false;
		}
		OutTarget.Mode = EAvidScriptStartupTargetMode::WorldHost;
		return true;
	}

	if (Mode == TEXT("existing_actor"))
	{
		if (!HasExactFields(
				*Object,
				{ TEXT("mode"), TEXT("class_path"), TEXT("max_instances") },
				{ TEXT("required_tag") })
			|| !TryReadClassPath(*Object, OutTarget.ClassPath)
			|| !TryReadExactInteger(*Object, TEXT("max_instances"), OutTarget.MaxInstances)
			|| OutTarget.MaxInstances < 1
			|| OutTarget.MaxInstances > MaximumSpawnTransforms)
		{
			SetFailure(OutResult, TEXT("target_invalid"), TEXT("existing_actor target is invalid"));
			return false;
		}
		FString RequiredTag;
		if (Object->HasField(TEXT("required_tag")))
		{
			if (!Object->TryGetStringField(TEXT("required_tag"), RequiredTag)
				|| !IsNormalizedIdentifier(RequiredTag))
			{
				SetFailure(OutResult, TEXT("target_invalid"), TEXT("required_tag is invalid"));
				return false;
			}
			OutTarget.RequiredTag = FName(*RequiredTag);
		}
		OutTarget.Mode = EAvidScriptStartupTargetMode::ExistingActor;
		return true;
	}

	if (Mode == TEXT("spawn_actor"))
	{
		const TArray<TSharedPtr<FJsonValue>>* TransformValues = nullptr;
		if (!HasExactFields(
				*Object,
				{ TEXT("mode"), TEXT("class_path"), TEXT("transforms") })
			|| !TryReadClassPath(*Object, OutTarget.ClassPath)
			|| !Object->TryGetArrayField(TEXT("transforms"), TransformValues)
			|| TransformValues == nullptr
			|| TransformValues->IsEmpty()
			|| TransformValues->Num() > MaximumSpawnTransforms)
		{
			SetFailure(OutResult, TEXT("target_invalid"), TEXT("spawn_actor target is invalid"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *TransformValues)
		{
			const TSharedPtr<FJsonObject>* TransformObject = nullptr;
			FTransform Transform;
			if (!Value.IsValid()
				|| !Value->TryGetObject(TransformObject)
				|| TransformObject == nullptr
				|| !TryReadTransform(*TransformObject, Transform))
			{
				SetFailure(OutResult, TEXT("target_invalid"), TEXT("spawn transform is invalid"));
				return false;
			}
			OutTarget.SpawnTransforms.Add(Transform);
		}
		OutTarget.Mode = EAvidScriptStartupTargetMode::SpawnActor;
		OutTarget.MaxInstances = OutTarget.SpawnTransforms.Num();
		return true;
	}

	SetFailure(OutResult, TEXT("target_mode_unsupported"), Mode);
	return false;
}

bool ParseBinding(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptStartupBinding& OutBinding,
	FAvidScriptStartupLoadResult& OutResult)
{
	const TSharedPtr<FJsonObject>* TargetObject = nullptr;
	if (!Object.IsValid()
		|| !HasExactFields(*Object, { TEXT("module_id"), TEXT("target") })
		|| !Object->TryGetStringField(TEXT("module_id"), OutBinding.ModuleId)
		|| !AvidScript::ModulePackage::IsNormalizedModuleId(OutBinding.ModuleId)
		|| !Object->TryGetObjectField(TEXT("target"), TargetObject)
		|| TargetObject == nullptr)
	{
		SetFailure(OutResult, TEXT("binding_invalid"), TEXT("binding identity or target is invalid"));
		return false;
	}
	return ParseTarget(*TargetObject, OutBinding.Target, OutResult);
}

bool ParseScenario(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptStartupScenario& OutScenario,
	FAvidScriptStartupLoadResult& OutResult)
{
	FString Activation;
	const TArray<TSharedPtr<FJsonValue>>* WorldValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* BindingValues = nullptr;
	if (!Object.IsValid()
		|| !HasExactFields(
			*Object,
			{ TEXT("scenario_id"), TEXT("activation"), TEXT("worlds"), TEXT("bindings") })
		|| !Object->TryGetStringField(TEXT("scenario_id"), OutScenario.ScenarioId)
		|| !IsNormalizedIdentifier(OutScenario.ScenarioId)
		|| !Object->TryGetStringField(TEXT("activation"), Activation)
		|| Activation != TEXT("explicit")
		|| !Object->TryGetArrayField(TEXT("worlds"), WorldValues)
		|| WorldValues == nullptr
		|| WorldValues->IsEmpty()
		|| WorldValues->Num() > MaximumWorlds
		|| !Object->TryGetArrayField(TEXT("bindings"), BindingValues)
		|| BindingValues == nullptr
		|| BindingValues->IsEmpty()
		|| BindingValues->Num() > MaximumBindings)
	{
		SetFailure(OutResult, TEXT("scenario_invalid"), TEXT("scenario fields are invalid"));
		return false;
	}

	TSet<FString> UniqueWorlds;
	for (const TSharedPtr<FJsonValue>& Value : *WorldValues)
	{
		FString World;
		if (!Value.IsValid()
			|| !Value->TryGetString(World)
			|| World.Len() > MaximumPathLength
			|| !FPackageName::IsValidLongPackageName(World)
			|| UniqueWorlds.Contains(World))
		{
			SetFailure(OutResult, TEXT("world_filter_invalid"), TEXT("world package filter is invalid or duplicated"));
			return false;
		}
		UniqueWorlds.Add(World);
		OutScenario.Worlds.Add(MoveTemp(World));
	}

	TSet<FString> BindingKeys;
	for (const TSharedPtr<FJsonValue>& Value : *BindingValues)
	{
		const TSharedPtr<FJsonObject>* BindingObject = nullptr;
		FAvidScriptStartupBinding Binding;
		if (!Value.IsValid()
			|| !Value->TryGetObject(BindingObject)
			|| BindingObject == nullptr
			|| !ParseBinding(*BindingObject, Binding, OutResult))
		{
			return false;
		}
		const FString BindingKey = FString::Printf(
			TEXT("%s|%d|%s|%s"),
			*Binding.ModuleId,
			static_cast<int32>(Binding.Target.Mode),
			*Binding.Target.ClassPath,
			*Binding.Target.RequiredTag.ToString());
		if (BindingKeys.Contains(BindingKey))
		{
			SetFailure(OutResult, TEXT("binding_duplicate"), BindingKey);
			return false;
		}
		BindingKeys.Add(BindingKey);
		OutScenario.Bindings.Add(MoveTemp(Binding));
	}
	return true;
}
} // namespace

bool ParseDocument(
	const FString& Json,
	FAvidScriptStartupDocument& OutDocument,
	FAvidScriptStartupLoadResult& OutResult)
{
	OutDocument = FAvidScriptStartupDocument();
	OutResult = FAvidScriptStartupLoadResult();
	if (Json.IsEmpty()
		|| FTCHARToUTF8(*Json).Length() > MaximumDocumentBytes)
	{
		SetFailure(OutResult, TEXT("document_size_invalid"), TEXT("document is empty or exceeds 1 MiB"));
		return false;
	}

	FString DuplicateKey;
	if (!HasNoDuplicateObjectKeys(Json, DuplicateKey))
	{
		SetFailure(
			OutResult,
			DuplicateKey.IsEmpty() ? TEXT("json_invalid") : TEXT("json_duplicate_key"),
			DuplicateKey.IsEmpty() ? TEXT("JSON token stream is invalid") : DuplicateKey);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root)
		|| !Root.IsValid()
		|| !HasExactFields(*Root, { TEXT("schema_version"), TEXT("scenarios") }))
	{
		SetFailure(OutResult, TEXT("document_invalid"), TEXT("root object fields are invalid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ScenarioValues = nullptr;
	if (!TryReadExactInteger(*Root, TEXT("schema_version"), OutDocument.SchemaVersion)
		|| OutDocument.SchemaVersion != 1
		|| !Root->TryGetArrayField(TEXT("scenarios"), ScenarioValues)
		|| ScenarioValues == nullptr
		|| ScenarioValues->IsEmpty()
		|| ScenarioValues->Num() > MaximumScenarios)
	{
		SetFailure(OutResult, TEXT("document_invalid"), TEXT("schema version or scenario count is invalid"));
		return false;
	}

	TSet<FString> ScenarioIds;
	for (const TSharedPtr<FJsonValue>& Value : *ScenarioValues)
	{
		const TSharedPtr<FJsonObject>* ScenarioObject = nullptr;
		FAvidScriptStartupScenario Scenario;
		if (!Value.IsValid()
			|| !Value->TryGetObject(ScenarioObject)
			|| ScenarioObject == nullptr
			|| !ParseScenario(*ScenarioObject, Scenario, OutResult))
		{
			return false;
		}
		if (ScenarioIds.Contains(Scenario.ScenarioId))
		{
			SetFailure(OutResult, TEXT("scenario_duplicate"), Scenario.ScenarioId);
			return false;
		}
		ScenarioIds.Add(Scenario.ScenarioId);
		OutDocument.Scenarios.Add(MoveTemp(Scenario));
	}
	return true;
}

bool LoadDocumentFile(
	const FString& Path,
	FAvidScriptStartupDocument& OutDocument,
	FAvidScriptStartupLoadResult& OutResult)
{
	OutDocument = FAvidScriptStartupDocument();
	OutResult = FAvidScriptStartupLoadResult();
	const int64 FileSize = IFileManager::Get().FileSize(*Path);
	if (FileSize <= 0 || FileSize > MaximumDocumentBytes)
	{
		SetFailure(OutResult, TEXT("file_size_invalid"), Path);
		return false;
	}
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		SetFailure(OutResult, TEXT("file_read_failed"), Path);
		return false;
	}
	return ParseDocument(Json, OutDocument, OutResult);
}

const FAvidScriptStartupScenario* FindScenario(
	const FAvidScriptStartupDocument& Document,
	const FString& ScenarioId)
{
	return Document.Scenarios.FindByPredicate(
		[&ScenarioId](const FAvidScriptStartupScenario& Scenario)
		{
			return Scenario.ScenarioId == ScenarioId;
		});
}

bool IsWorldAllowed(
	const FAvidScriptStartupScenario& Scenario,
	const FString& WorldPackageName)
{
	return Scenario.Worlds.Contains(WorldPackageName);
}
} // namespace AvidScript::Startup
