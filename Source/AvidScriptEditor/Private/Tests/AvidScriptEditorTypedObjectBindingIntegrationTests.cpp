#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorTypedObjectBindingTestTypes.h"
#include "AvidScriptFrontendReport.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectTypeBinding.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
FString MakeAvidScriptTypedObjectBindingTestPath(const FString& RelativePath)
{
	FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptEditorTests"),
		TEXT("TypedObjectBinding"),
		RelativePath));
	FPaths::NormalizeFilename(Path);
	return Path;
}

const FAvidScriptBindingTypeModel* FindAvidScriptObjectType(
	const FAvidScriptBindingPackageModel& Package,
	const FString& ClassPath)
{
	return Package.Types.FindByPredicate(
		[&ClassPath](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.ClassPath == ClassPath;
		});
}

const FAvidScriptBindingFunctionModel* FindAvidScriptBindingForFunction(
	const FAvidScriptBindingPackageModel& Package,
	const FString& OwnerClassPath,
	const TCHAR* FunctionName)
{
	return Package.Bindings.FindByPredicate(
		[&OwnerClassPath, FunctionName](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.OwnerClass == OwnerClassPath
				&& Binding.UeFunction == FunctionName;
		});
}

FString MakeAvidScriptImportIdentity(
	const FString& StableId,
	const uint32 Ordinal,
	const FString& ModuleName,
	const FString& ImportName,
	const FString& Signature)
{
	return FString::Printf(
		TEXT("%s|%u|%s|%s|%s"),
		*StableId,
		Ordinal,
		*ModuleName,
		*ImportName,
		*Signature);
}

FString MakeAvidScriptImportIdentity(const FAvidScriptBindingFunctionModel& Binding)
{
	return MakeAvidScriptImportIdentity(
		Binding.StableId,
		static_cast<uint32>(Binding.Ordinal),
		Binding.HostImport.Module,
		Binding.HostImport.Name,
		Binding.HostImport.Signature);
}

FString MakeAvidScriptImportIdentity(const FAvidScriptVmDynamicImport& Import)
{
	return MakeAvidScriptImportIdentity(
		Import.StableId,
		Import.Ordinal,
		Import.ModuleName,
		Import.ImportName,
		Import.Signature);
}

void ReleaseAvidScriptTransientObject(UObject* Object)
{
	if (Object == nullptr)
	{
		return;
	}

	Object->SetFlags(RF_Transient);
	Object->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
	if (Object->IsRooted())
	{
		Object->RemoveFromRoot();
	}
	Object->MarkAsGarbage();
}

void CleanupAvidScriptTransientBlueprint(TStrongObjectPtr<UBlueprint>& Blueprint)
{
	UBlueprint* BlueprintObject = Blueprint.Get();
	if (BlueprintObject == nullptr)
	{
		return;
	}

	UClass* GeneratedClass = BlueprintObject->GeneratedClass;
	UClass* SkeletonGeneratedClass = BlueprintObject->SkeletonGeneratedClass;
	BlueprintObject->RemoveGeneratedClasses();
	ReleaseAvidScriptTransientObject(GeneratedClass);
	if (SkeletonGeneratedClass != GeneratedClass)
	{
		ReleaseAvidScriptTransientObject(SkeletonGeneratedClass);
	}
	ReleaseAvidScriptTransientObject(BlueprintObject);
	Blueprint.Reset();
}

FString GetAvidScriptTypedProjectApiPluginPath(const TCHAR* RelativePath)
{
	FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript"),
		RelativePath));
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString ResolveAvidScriptTypedProjectApiArtifactPath(const FString& ArtifactPath)
{
	if (ArtifactPath.IsEmpty())
	{
		return FString();
	}

	FString Path = ArtifactPath;
	FPaths::NormalizeFilename(Path);
	if (FPaths::IsRelative(Path))
	{
		Path = FPaths::Combine(FPaths::ProjectDir(), Path);
	}
	return FPaths::ConvertRelativePathToFull(Path);
}

bool LoadAvidScriptTypedProjectApiJson(
	const FString& Path,
	FString& OutJson,
	TSharedPtr<FJsonObject>& OutObject)
{
	OutJson.Empty();
	OutObject.Reset();
	if (!FFileHelper::LoadFileToString(OutJson, *Path))
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutJson);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

int32 CountAvidScriptJsonStringField(
	const TSharedPtr<FJsonObject>& Object,
	const FString& FieldName,
	const FString& ExpectedValue)
{
	if (!Object.IsValid())
	{
		return 0;
	}

	int32 Count = 0;
	FString Value;
	if (Object->TryGetStringField(FieldName, Value) && Value == ExpectedValue)
	{
		++Count;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}
		if (Pair.Value->Type == EJson::Object)
		{
			Count += CountAvidScriptJsonStringField(Pair.Value->AsObject(), FieldName, ExpectedValue);
		}
		else if (Pair.Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray())
			{
				if (!Item.IsValid())
				{
					continue;
				}
				if (Item->Type == EJson::Object)
				{
					Count += CountAvidScriptJsonStringField(Item->AsObject(), FieldName, ExpectedValue);
				}
			}
		}
	}
	return Count;
}

int32 CountAvidScriptJsonStringFieldPrefix(
	const TSharedPtr<FJsonObject>& Object,
	const FString& FieldName,
	const FString& Prefix)
{
	if (!Object.IsValid())
	{
		return 0;
	}

	int32 Count = 0;
	FString Value;
	if (Object->TryGetStringField(FieldName, Value)
		&& Value.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		++Count;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}
		if (Pair.Value->Type == EJson::Object)
		{
			Count += CountAvidScriptJsonStringFieldPrefix(Pair.Value->AsObject(), FieldName, Prefix);
		}
		else if (Pair.Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray())
			{
				if (Item.IsValid() && Item->Type == EJson::Object)
				{
					Count += CountAvidScriptJsonStringFieldPrefix(Item->AsObject(), FieldName, Prefix);
				}
			}
		}
	}
	return Count;
}

TSharedPtr<FJsonObject> FindAvidScriptJsonArrayObject(
	const TSharedPtr<FJsonObject>& Root,
	const FString& ArrayField,
	const FString& IdentityField,
	const FString& Identity)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Root.IsValid() || !Root->TryGetArrayField(ArrayField, Values) || Values == nullptr)
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Candidate;
		if (Object.IsValid()
			&& Object->TryGetStringField(IdentityField, Candidate)
			&& Candidate == Identity)
		{
			return Object;
		}
	}
	return nullptr;
}

bool CreateAvidScriptTypedProjectApiWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptTypedProjectApiWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	OutWorld->InitializeActorsForPlay(FURL());
	OutWorld->BeginPlay();
	OutWorld->SetBegunPlay(true);
	return true;
}

void DestroyAvidScriptTypedProjectApiWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}
	if (World->HasBegunPlay())
	{
		World->EndPlay(EEndPlayReason::Quit);
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

AAvidScriptTypedTestProjectile* FindAvidScriptTypedProjectApiProjectile(
	UWorld& World,
	const UClass& ProjectileClass)
{
	for (TActorIterator<AAvidScriptTypedTestProjectile> It(&World); It; ++It)
	{
		AAvidScriptTypedTestProjectile* Projectile = *It;
		if (IsValid(Projectile)
			&& !Projectile->IsActorBeingDestroyed()
			&& Projectile->GetClass() == &ProjectileClass)
		{
			return Projectile;
		}
	}
	return nullptr;
}

int32 CountAvidScriptRequiredImport(
	const FAvidScriptWasmReloadManifest& Manifest,
	const FString& ModuleName,
	const FString& ImportName)
{
	return Manifest.RequiredImports.FilterByPredicate(
		[&ModuleName, &ImportName](const FAvidScriptWasmRequiredImport& Import)
		{
			return Import.ModuleName == ModuleName && Import.ImportName == ImportName;
		}).Num();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorTypedObjectBindingIntegrationTest,
	"AvidScript.Editor.TypedObjectBinding.CustomActorDomain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorTypedObjectBindingIntegrationTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeAvidScriptTypedObjectBindingTestPath(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	};

	const FString SourcePath = FPaths::Combine(TestRoot, TEXT("TypedObjectBinding.cs"));
	if (!TestTrue(
			TEXT("Typed object binding profile source can be written"),
			FFileHelper::SaveStringToFile(
				TEXT("public static class TypedObjectBindingProfileSource {}\n"),
				*SourcePath)))
	{
		return false;
	}

	const FName BlueprintName(*FString::Printf(
		TEXT("AvidScriptTypedProjectile_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(
		AAvidScriptTypedTestProjectile::StaticClass(),
		GetTransientPackage(),
		BlueprintName,
		BPTYPE_Normal,
		TEXT("AvidScriptTypedObjectBinding")));
	if (!TestNotNull(TEXT("Transient Blueprint subclass is created"), Blueprint.Get()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		CleanupAvidScriptTransientBlueprint(Blueprint);
	};
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	UClass* BlueprintClass = Blueprint->GeneratedClass;
	if (!TestNotNull(TEXT("Transient Blueprint subclass compiles a generated class"), BlueprintClass))
	{
		return false;
	}
	TestTrue(TEXT("Transient Blueprint compilation is up to date"), Blueprint->IsUpToDate());
	TestTrue(
		TEXT("Transient Blueprint generated class derives from the native projectile"),
		BlueprintClass->IsChildOf(AAvidScriptTypedTestProjectile::StaticClass()));
	TestFalse(TEXT("Transient Blueprint generated class is concrete"), BlueprintClass->HasAnyClassFlags(CLASS_Abstract));

	const FString ActorClassPath = AAvidScriptTypedTestActor::StaticClass()->GetPathName();
	const FString ProjectileClassPath = AAvidScriptTypedTestProjectile::StaticClass()->GetPathName();
	const FString BlueprintClassPath = BlueprintClass->GetPathName();
	const FString ProfilePath = FPaths::Combine(TestRoot, TEXT("typed_object_binding.csharp-profile.json"));
	const FString ProfileText = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 3,\n")
		TEXT("  \"language\": \"csharp\",\n")
		TEXT("  \"source_path\": \"%s\",\n")
		TEXT("  \"binding_profile\": {\n")
		TEXT("    \"package_name\": \"avidscript.project.typed_object_binding\",\n")
		TEXT("    \"self_class_path\": \"%s\",\n")
		TEXT("    \"classes\": [\n")
		TEXT("      { \"class_path\": \"%s\", \"include_functions\": [\"ApplyGameplayValue\"] },\n")
		TEXT("      { \"class_path\": \"%s\", \"include_functions\": [\"ActivateProjectile\"] }\n")
		TEXT("    ],\n")
		TEXT("    \"class_references\": [\n")
		TEXT("      { \"script_name\": \"TypedProjectileClass\", \"class_path\": \"%s\", \"base_class_path\": \"%s\", \"load_policy\": \"EditorLoad\" }\n")
		TEXT("    ]\n")
		TEXT("  }\n")
		TEXT("}\n"),
		*SourcePath,
		*ActorClassPath,
		*ActorClassPath,
		*ProjectileClassPath,
		*BlueprintClassPath,
		*ProjectileClassPath);
	if (!TestTrue(
			TEXT("Schema v3 typed object binding profile can be written"),
			FFileHelper::SaveStringToFile(ProfileText, *ProfilePath)))
	{
		return false;
	}

	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!TestTrue(
			TEXT("Schema v3 profile resolves the transient Blueprint class reference"),
			FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, ProfileResult)))
	{
		AddError(ProfileResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Typed object profile uses schema v3"), ProfileResult.SchemaVersion, 3);
	TestEqual(TEXT("Typed object profile resolves one class reference"), ProfileResult.ResolvedClassReferences.Num(), 1);
	if (ProfileResult.ResolvedClassReferences.Num() != 1)
	{
		return false;
	}
	const FAvidScriptProjectBindingClassSpec& ClassReference = ProfileResult.ResolvedClassReferences[0];
	TestEqual(TEXT("Class reference keeps the transient generated class path"), ClassReference.ClassPath, BlueprintClassPath);
	TestEqual(TEXT("Class reference keeps the native projectile base path"), ClassReference.BaseClassPath, ProjectileClassPath);

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
			TEXT("Typed object descriptor generates from the resolved v3 profile"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				ProfileResult.ResolvedBindingSelection,
				ProfileResult.ResolvedClassReferences,
				DescriptorJson,
				SelectionResult,
				DescriptorResult)))
	{
		AddError(DescriptorResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("Typed object descriptor parses as schema v6"),
			FAvidScriptBindingDescriptorParser::Parse(DescriptorJson, Package, ParseCategory, ParseSource)))
	{
		AddError(ParseCategory + TEXT(":") + ParseSource);
		return false;
	}
	TestEqual(TEXT("Typed object descriptor publishes schema v6"), Package.SchemaVersion, 6);
	TestEqual(TEXT("Descriptor publishes exactly one class reference"), Package.ClassReferences.Num(), 1);
	if (Package.ClassReferences.Num() != 1)
	{
		return false;
	}
	const FAvidScriptBindingTypeModel* SelfType = Package.Types.FindByPredicate(
		[&Package](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.StableId == Package.SelfTypeId;
		});
	if (TestNotNull(TEXT("Descriptor publishes a typed Self node"), SelfType))
	{
		TestEqual(TEXT("Typed Self remains the native test Actor"), SelfType->ClassPath, ActorClassPath);
	}
	const FAvidScriptBindingTypeModel* ActorType = FindAvidScriptObjectType(Package, ActorClassPath);
	const FAvidScriptBindingTypeModel* ProjectileType = FindAvidScriptObjectType(Package, ProjectileClassPath);
	const FAvidScriptBindingTypeModel* EngineActorType = FindAvidScriptObjectType(Package, AActor::StaticClass()->GetPathName());
	const FAvidScriptBindingTypeModel* ObjectType = FindAvidScriptObjectType(Package, UObject::StaticClass()->GetPathName());
	if (TestNotNull(TEXT("Object graph includes the custom Actor"), ActorType)
		&& TestNotNull(TEXT("Object graph includes the custom projectile"), ProjectileType)
		&& TestNotNull(TEXT("Object graph includes the Engine Actor ancestor"), EngineActorType)
		&& TestNotNull(TEXT("Object graph includes the UObject root"), ObjectType))
	{
		TestEqual(TEXT("Projectile keeps the custom Actor base edge"), ProjectileType->BaseTypeId, ActorType->StableId);
		TestEqual(TEXT("Custom Actor keeps the Engine Actor base edge"), ActorType->BaseTypeId, EngineActorType->StableId);
		TestEqual(TEXT("Engine Actor remains inside the ancestor closure"), EngineActorType->BaseTypeId, ObjectType->StableId);
	}
	TestEqual(TEXT("Descriptor publishes exactly two custom reflected bindings"), Package.Bindings.Num(), 2);
	if (Package.Bindings.Num() != 2)
	{
		return false;
	}
	const FAvidScriptBindingFunctionModel* ActorBinding =
		FindAvidScriptBindingForFunction(Package, ActorClassPath, TEXT("ApplyGameplayValue"));
	const FAvidScriptBindingFunctionModel* ProjectileBinding =
		FindAvidScriptBindingForFunction(Package, ProjectileClassPath, TEXT("ActivateProjectile"));
	if (!TestNotNull(TEXT("Descriptor binds the custom Actor UFUNCTION"), ActorBinding)
		|| !TestNotNull(TEXT("Descriptor binds the custom projectile UFUNCTION"), ProjectileBinding))
	{
		return false;
	}
	const auto ValidateReflectedImport = [this](const FAvidScriptBindingFunctionModel& Binding)
	{
		TestEqual(TEXT("Custom UFUNCTION uses cached ProcessEvent dispatch"),
			Binding.DispatchMode, FString(TEXT("cached_process_event")));
		TestEqual(TEXT("Custom UFUNCTION uses the generated reflection module"),
			Binding.HostImport.Module, FString(TEXT("avidscript")));
		TestEqual(TEXT("Custom UFUNCTION import name derives only from stable identity"),
			Binding.HostImport.Name, TEXT("avid_ue_") + Binding.StableId.Left(16));
		TestFalse(TEXT("Custom UFUNCTION import does not expose a per-function native symbol"),
			Binding.HostImport.Name.Contains(Binding.UeFunction, ESearchCase::IgnoreCase));
	};
	ValidateReflectedImport(*ActorBinding);
	ValidateReflectedImport(*ProjectileBinding);
	TSet<FString> ReflectedImportWhitelist;
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		ReflectedImportWhitelist.Add(MakeAvidScriptImportIdentity(Binding));
	}
	TestEqual(TEXT("Descriptor reflected import whitelist has exact cardinality"),
		ReflectedImportWhitelist.Num(), 2);

	const FAvidScriptBindingClassReferenceModel& Reference = Package.ClassReferences[0];
	TestEqual(TEXT("Descriptor class reference stays Blueprint concrete"), Reference.ClassPath, BlueprintClassPath);
	TestEqual(TEXT("Typed Spawn result stays at the native projectile base"), Reference.BaseClassPath, ProjectileClassPath);
	TestEqual(TEXT("Typed Spawn result resolves to the projectile graph node"),
		Reference.ResultTypeId, ProjectileType == nullptr ? FString() : ProjectileType->StableId);

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult RuntimeLoadResult;
	if (!TestTrue(
			TEXT("Runtime package loads the typed custom reflection descriptor"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				RuntimePackage,
				RuntimeLoadResult))
		|| !RuntimePackage.IsValid())
	{
		AddError(RuntimeLoadResult.ErrorCategory + TEXT(": ") + RuntimeLoadResult.ErrorDetails);
		return false;
	}
	TSet<FString> CapabilityImportWhitelist = ReflectedImportWhitelist;
	uint32 CapabilityOrdinal = static_cast<uint32>(Package.Bindings.Num());
	for (const FAvidScriptObjectLifecycleBindingSpec& Spec :
		FAvidScriptObjectLifecycleBindings::GetSpecs())
	{
		CapabilityImportWhitelist.Add(MakeAvidScriptImportIdentity(
			Spec.StableId,
			CapabilityOrdinal++,
			Spec.ModuleName,
			Spec.ImportName,
			Spec.Signature));
	}
	for (const FAvidScriptObjectTypeBindingSpec& Spec :
		FAvidScriptObjectTypeBindings::GetSpecs())
	{
		CapabilityImportWhitelist.Add(MakeAvidScriptImportIdentity(
			Spec.StableId,
			CapabilityOrdinal++,
			Spec.ModuleName,
			Spec.ImportName,
			Spec.Signature));
	}
	const int32 ExpectedCapabilityImportCount =
		Package.Bindings.Num()
		+ FAvidScriptObjectLifecycleBindings::GetSpecs().Num()
		+ FAvidScriptObjectTypeBindings::GetSpecs().Num();
	TestEqual(TEXT("Capability import whitelist has exact cardinality"),
		CapabilityImportWhitelist.Num(), ExpectedCapabilityImportCount);
	const TArray<FAvidScriptVmDynamicImport>& RuntimeImports =
		RuntimePackage->GetVmPackage().Imports;
	TestEqual(TEXT("Runtime package contains only reflected and fixed capability imports"),
		RuntimeImports.Num(), CapabilityImportWhitelist.Num());
	TSet<FString> ActualRuntimeImports;
	for (const FAvidScriptVmDynamicImport& Import : RuntimeImports)
	{
		ActualRuntimeImports.Add(MakeAvidScriptImportIdentity(Import));
	}
	TestEqual(TEXT("Runtime package import identities contain no duplicates"),
		ActualRuntimeImports.Num(), RuntimeImports.Num());
	for (const FString& ExpectedImport : CapabilityImportWhitelist)
	{
		TestTrue(TEXT("Runtime package contains every exact whitelisted capability import"),
			ActualRuntimeImports.Contains(ExpectedImport));
	}

	FString ReferenceSource;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	if (!TestTrue(
			TEXT("Typed object facade emits from the descriptor"),
			FAvidScriptEditorCSharpBindingEmitter::Emit(DescriptorJson, ReferenceSource, ManifestJson, EmitResult)))
	{
		AddError(EmitResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Facade exposes typed Self and Blueprint Spawn result"),
		ReferenceSource.Contains(TEXT("public static AAvidScriptTypedTestActor Self"))
		&& ReferenceSource.Contains(TEXT("public readonly struct TSubclassOfAAvidScriptTypedTestProjectile"))
		&& ReferenceSource.Contains(TEXT("public static TSubclassOfAAvidScriptTypedTestProjectile TypedProjectileClass => new(0);"))
		&& ReferenceSource.Contains(TEXT("public static AAvidScriptTypedTestProjectile SpawnActor(TSubclassOfAAvidScriptTypedTestProjectile actorClass, FTransform transform)")));
	TestTrue(
		TEXT("Facade exposes both ordinary custom UFUNCTION bindings"),
		ReferenceSource.Contains(TEXT("ApplyGameplayValue(float Delta)"))
		&& ReferenceSource.Contains(TEXT("ActivateProjectile()")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpTypedProjectApiTest,
	"AvidScript.Editor.CSharp.TypedProjectApi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpTypedProjectApiTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeAvidScriptTypedObjectBindingTestPath(FPaths::Combine(
		TEXT("TypedProjectApi"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	};

	const FName BlueprintName(*FString::Printf(
		TEXT("AvidScriptTypedProjectApiProjectile_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(
		AAvidScriptTypedTestProjectile::StaticClass(),
		GetTransientPackage(),
		BlueprintName,
		BPTYPE_Normal,
		TEXT("AvidScriptTypedProjectApi")));
	if (!TestNotNull(TEXT("Typed project API transient Blueprint is created"), Blueprint.Get()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		CleanupAvidScriptTransientBlueprint(Blueprint);
	};
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	UClass* BlueprintClass = Blueprint->GeneratedClass;
	if (!TestNotNull(TEXT("Typed project API Blueprint compiles a generated class"), BlueprintClass))
	{
		return false;
	}
	TestTrue(TEXT("Typed project API Blueprint compilation is current"), Blueprint->IsUpToDate());
	TestTrue(
		TEXT("Typed project API Blueprint derives from the selected native projectile"),
		BlueprintClass->IsChildOf(AAvidScriptTypedTestProjectile::StaticClass()));
	TestFalse(TEXT("Typed project API Blueprint is concrete"), BlueprintClass->HasAnyClassFlags(CLASS_Abstract));

	const FString SampleProfilePath = GetAvidScriptTypedProjectApiPluginPath(
		TEXT("Samples/CSharp/TypedProjectApi/TypedProjectApi.csharp-profile.json"));
	FString DynamicProfileText;
	if (!TestTrue(
		TEXT("Typed project API sample profile is readable"),
		FFileHelper::LoadFileToString(DynamicProfileText, *SampleProfilePath)))
	{
		return false;
	}
	const FString NativeClassReference = FString::Printf(
		TEXT("        \"script_name\": \"Projectile\",\n")
		TEXT("        \"class_path\": \"%s\",\n")
		TEXT("        \"base_class_path\": \"%s\",\n"),
		*AAvidScriptTypedTestProjectile::StaticClass()->GetPathName(),
		*AAvidScriptTypedTestProjectile::StaticClass()->GetPathName());
	const FString BlueprintClassReference = FString::Printf(
		TEXT("        \"script_name\": \"Projectile\",\n")
		TEXT("        \"class_path\": \"%s\",\n")
		TEXT("        \"base_class_path\": \"%s\",\n"),
		*BlueprintClass->GetPathName(),
		*AAvidScriptTypedTestProjectile::StaticClass()->GetPathName());
	TestEqual(
		TEXT("Typed project API profile replaces exactly one concrete class reference"),
		DynamicProfileText.ReplaceInline(
			*NativeClassReference,
			*BlueprintClassReference,
			ESearchCase::CaseSensitive),
		1);
	const FString DynamicProfilePath = FPaths::Combine(
		TestRoot,
		TEXT("TypedProjectApi.csharp-profile.json"));
	if (!TestTrue(
		TEXT("Typed project API dynamic profile can be written"),
		FFileHelper::SaveStringToFile(
			DynamicProfileText,
			*DynamicProfilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
	{
		return false;
	}

	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!TestTrue(
		TEXT("Typed project API schema v3 profile parses through the production service"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(DynamicProfilePath, ProfileResult)))
	{
		AddError(ProfileResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Typed project API profile uses schema v3"), ProfileResult.SchemaVersion, 3);
	TestEqual(
		TEXT("Typed project API profile resolves the typed Self"),
		ProfileResult.ResolvedBindingSelection.SelfClassPath,
		AAvidScriptTypedTestActor::StaticClass()->GetPathName());
	TestEqual(TEXT("Typed project API profile resolves one class reference"), ProfileResult.ResolvedClassReferences.Num(), 1);
	if (ProfileResult.ResolvedClassReferences.Num() != 1)
	{
		return false;
	}
	TestEqual(
		TEXT("Typed project API profile resolves the concrete transient Blueprint"),
		ProfileResult.ResolvedClassReferences[0].ClassPath,
		BlueprintClass->GetPathName());

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
		TEXT("Typed project API reflection descriptor generates from the parsed profile"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			ProfileResult.ResolvedBindingSelection,
			ProfileResult.ResolvedClassReferences,
			DescriptorJson,
			SelectionResult,
			DescriptorResult)))
	{
		AddError(DescriptorResult.ErrorMessage);
		return false;
	}
	FAvidScriptBindingPackageModel DescriptorPackage;
	FString DescriptorParseCategory;
	FString DescriptorParseSource;
	if (!TestTrue(
		TEXT("Typed project API reflection descriptor parses as the production model"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			DescriptorPackage,
			DescriptorParseCategory,
			DescriptorParseSource)))
	{
		AddError(DescriptorParseCategory + TEXT(":") + DescriptorParseSource);
		return false;
	}
	const FString ActorClassPath = AAvidScriptTypedTestActor::StaticClass()->GetPathName();
	const FString ProjectileClassPath = AAvidScriptTypedTestProjectile::StaticClass()->GetPathName();
	const FAvidScriptBindingFunctionModel* ActorBinding =
		FindAvidScriptBindingForFunction(DescriptorPackage, ActorClassPath, TEXT("ApplyGameplayValue"));
	const FAvidScriptBindingFunctionModel* ProjectileBinding =
		FindAvidScriptBindingForFunction(DescriptorPackage, ProjectileClassPath, TEXT("ActivateProjectile"));
	if (!TestNotNull(TEXT("Typed project API descriptor contains the custom Actor function"), ActorBinding)
		|| !TestNotNull(TEXT("Typed project API descriptor contains the projectile function"), ProjectileBinding))
	{
		return false;
	}

	FString FacadeSource;
	FString FacadeManifest;
	FAvidScriptCSharpBindingEmitResult FacadeResult;
	if (!TestTrue(
		TEXT("Typed project API facade emits through the production renderer"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			FacadeSource,
			FacadeManifest,
			FacadeResult)))
	{
		AddError(FacadeResult.ErrorMessage);
		return false;
	}
	const FAvidScriptBindingTypeModel* ActorType =
		FindAvidScriptObjectType(DescriptorPackage, ActorClassPath);
	const FAvidScriptBindingTypeModel* ProjectileType =
		FindAvidScriptObjectType(DescriptorPackage, ProjectileClassPath);
	if (!TestNotNull(TEXT("Typed project API descriptor contains the Actor object type"), ActorType)
		|| !TestNotNull(TEXT("Typed project API descriptor contains the projectile object type"), ProjectileType))
	{
		return false;
	}
	TestTrue(
		TEXT("Typed project API facade emits target-derived static checked casts"),
		FacadeSource.Contains(TEXT("public static AAvidScriptTypedTestActor TryCast(AActor value)"))
		&& FacadeSource.Contains(FString::Printf(
			TEXT("ObjectTypeIsA(value.Slot, value.Generation, %d)"),
			ActorType->ObjectTypeOrdinal))
		&& FacadeSource.Contains(TEXT("public static AAvidScriptTypedTestProjectile TryCast(AAvidScriptTypedTestActor value)"))
		&& FacadeSource.Contains(FString::Printf(
			TEXT("ObjectTypeIsA(value.Slot, value.Generation, %d)"),
			ProjectileType->ObjectTypeOrdinal)));
	TestFalse(
		TEXT("Typed project API facade excludes the reversed instance checked cast"),
		FacadeSource.Contains(TEXT("public AAvidScriptTypedTestActor TryCast()"))
		|| FacadeSource.Contains(TEXT("public AActor TryCast()")));
	TestTrue(
		TEXT("Typed project API facade exposes typed Self, typed Spawn, and both custom functions"),
		FacadeSource.Contains(TEXT("public static AAvidScriptTypedTestActor Self"))
		&& FacadeSource.Contains(TEXT("public static AAvidScriptTypedTestProjectile SpawnActor(TSubclassOfAAvidScriptTypedTestProjectile actorClass, FTransform transform)"))
		&& FacadeSource.Contains(TEXT("ApplyGameplayValue(float Delta)"))
		&& FacadeSource.Contains(TEXT("ActivateProjectile()")));

	FAvidScriptEditorCSharpBuildRequest BuildRequest =
		FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ProfileResult);
	BuildRequest.Config.OutputRoot = FPaths::Combine(TestRoot, TEXT("Build"));
	BuildRequest.Config.ArtifactStem = TEXT("typed_project_api");
	BuildRequest.Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
		BuildRequest.Config.OutputRoot,
		BuildRequest.Config.ArtifactStem);
	BuildRequest.Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
		BuildRequest.Config.OutputRoot,
		BuildRequest.Config.ArtifactStem);
	BuildRequest.Config.SemanticCacheRoot = FPaths::Combine(TestRoot, TEXT("SemanticCache/v1"));
	BuildRequest.Config.bDisableSemanticCache = true;
	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
		TEXT("Typed project API source builds through facade, Roslyn, Guest IR, and WASM"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(BuildRequest, BuildResult)))
	{
		AddError(
			BuildResult.ErrorMessage
			+ TEXT("\nstdout:\n") + BuildResult.Stdout
			+ TEXT("\nstderr:\n") + BuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Typed project API build performs bootstrap and final passes"), BuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Typed project API build invokes Roslyn frontend once"), BuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Typed project API build emits one semantic model"), BuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Typed project API build lowers reachable Guest IR twice"), BuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Typed project API build emits WASM twice"), BuildResult.WasmBackendInvocationCount, 2);
	TestTrue(
		TEXT("Typed project API build publishes an authorization package"),
		!BuildResult.AuthorizationBindingPackagePath.IsEmpty()
		&& FPaths::FileExists(BuildResult.AuthorizationBindingPackagePath));

	FAvidScriptFrontendReport FrontendReport;
	FAvidScriptFrontendReportLoadResult FrontendReportResult;
	if (!TestTrue(
		TEXT("Typed project API production frontend report loads"),
		FAvidScriptFrontendReportReader::LoadFromFile(
			BuildResult.ReportPath,
			FrontendReport,
			FrontendReportResult)))
	{
		AddError(FrontendReportResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Typed project API report proves Roslyn semantic and Guest IR success"),
		FrontendReport.bSucceeded
		&& FrontendReport.bSemanticSucceeded
		&& FrontendReport.bGuestIrSucceeded
		&& FrontendReport.SemanticSchemaVersion >= 7
		&& FrontendReport.GuestIrSchemaVersion == 1);
	const FString SemanticPath =
		ResolveAvidScriptTypedProjectApiArtifactPath(FrontendReport.SemanticArtifact);
	const FString GuestIrPath =
		ResolveAvidScriptTypedProjectApiArtifactPath(FrontendReport.GuestIrArtifact);
	FString SemanticJson;
	TSharedPtr<FJsonObject> SemanticRoot;
	if (!TestTrue(
		TEXT("Typed project API Roslyn semantic artifact is real structured JSON"),
		LoadAvidScriptTypedProjectApiJson(SemanticPath, SemanticJson, SemanticRoot)))
	{
		return false;
	}
	FString GuestIrJson;
	TSharedPtr<FJsonObject> GuestIrRoot;
	if (!TestTrue(
		TEXT("Typed project API reachable Guest IR artifact is real structured JSON"),
		LoadAvidScriptTypedProjectApiJson(GuestIrPath, GuestIrJson, GuestIrRoot)))
	{
		return false;
	}
	TestTrue(
		TEXT("Typed project API semantic and Guest IR artifacts succeeded"),
		SemanticRoot->GetBoolField(TEXT("succeeded"))
		&& GuestIrRoot->GetBoolField(TEXT("succeeded")));

	const FString ActorTryCastSymbol =
		TEXT("symbol:method:global::AvidScript.AAvidScriptTypedTestActor.TryCast(global::AvidScript.AActor):global::AvidScript.AAvidScriptTypedTestActor");
	const FString ProjectileTryCastSymbol =
		TEXT("symbol:method:global::AvidScript.AAvidScriptTypedTestProjectile.TryCast(global::AvidScript.AAvidScriptTypedTestActor):global::AvidScript.AAvidScriptTypedTestProjectile");
	const FString ActorUpcastSymbol =
		TEXT("symbol:method:global::AvidScript.AAvidScriptTypedTestActor.op_Implicit(global::AvidScript.AAvidScriptTypedTestActor):global::AvidScript.AActor");
	const FString ProjectileUpcastSymbol =
		TEXT("symbol:method:global::AvidScript.AAvidScriptTypedTestProjectile.op_Implicit(global::AvidScript.AAvidScriptTypedTestProjectile):global::AvidScript.AAvidScriptTypedTestActor");
	TestEqual(
		TEXT("Roslyn operation tree and CFG bind two Actor checked-downcast call sites"),
		CountAvidScriptJsonStringField(SemanticRoot, TEXT("symbol_id"), ActorTryCastSymbol),
		4);
	TestEqual(
		TEXT("Roslyn operation tree and CFG bind two projectile checked-downcast call sites"),
		CountAvidScriptJsonStringField(SemanticRoot, TEXT("symbol_id"), ProjectileTryCastSymbol),
		4);
	TestTrue(
		TEXT("Roslyn semantic artifact binds both direct-base implicit upcasts"),
		SemanticJson.Contains(ActorUpcastSymbol)
		&& SemanticJson.Contains(ProjectileUpcastSymbol));

	const TSharedPtr<FJsonObject> ObjectTypeImport = FindAvidScriptJsonArrayObject(
		GuestIrRoot,
		TEXT("imports"),
		TEXT("name"),
		TEXT("avid_object_type_is_a"));
	if (!TestTrue(TEXT("Reachable Guest IR declares one object-type import"), ObjectTypeImport.IsValid()))
	{
		return false;
	}
	FString ObjectTypeImportId;
	if (!TestTrue(
		TEXT("Reachable Guest IR object-type import has an identity"),
		ObjectTypeImport->TryGetStringField(TEXT("id"), ObjectTypeImportId)))
	{
		return false;
	}
	TestEqual(
		TEXT("Reachable Guest IR declares the object-type import exactly once"),
		CountAvidScriptJsonStringField(GuestIrRoot, TEXT("name"), TEXT("avid_object_type_is_a")),
		1);
	const TSharedPtr<FJsonObject> ActorTryCastFunction = FindAvidScriptJsonArrayObject(
		GuestIrRoot,
		TEXT("functions"),
		TEXT("id"),
		TEXT("function:") + ActorTryCastSymbol);
	const TSharedPtr<FJsonObject> ProjectileTryCastFunction = FindAvidScriptJsonArrayObject(
		GuestIrRoot,
		TEXT("functions"),
		TEXT("id"),
		TEXT("function:") + ProjectileTryCastSymbol);
	const TSharedPtr<FJsonObject> ActorUpcastFunction = FindAvidScriptJsonArrayObject(
		GuestIrRoot,
		TEXT("functions"),
		TEXT("id"),
		TEXT("function:") + ActorUpcastSymbol);
	const TSharedPtr<FJsonObject> ProjectileUpcastFunction = FindAvidScriptJsonArrayObject(
		GuestIrRoot,
		TEXT("functions"),
		TEXT("id"),
		TEXT("function:") + ProjectileUpcastSymbol);
	if (!TestTrue(
		TEXT("Reachable Guest IR contains both checked-downcast wrappers"),
		ActorTryCastFunction.IsValid() && ProjectileTryCastFunction.IsValid())
		|| !TestTrue(
			TEXT("Reachable Guest IR contains both direct upcast wrappers"),
			ActorUpcastFunction.IsValid() && ProjectileUpcastFunction.IsValid()))
	{
		return false;
	}
	TestEqual(
		TEXT("Actor checked-downcast wrapper performs exactly one object-type crossing"),
		CountAvidScriptJsonStringField(
			ActorTryCastFunction,
			TEXT("target_id"),
			ObjectTypeImportId),
		1);
	TestEqual(
		TEXT("Projectile checked-downcast wrapper performs exactly one object-type crossing"),
		CountAvidScriptJsonStringField(
			ProjectileTryCastFunction,
			TEXT("target_id"),
			ObjectTypeImportId),
		1);
	TestEqual(
		TEXT("Actor direct upcast wrapper performs zero host imports"),
		CountAvidScriptJsonStringFieldPrefix(
			ActorUpcastFunction,
			TEXT("target_id"),
			TEXT("import:")),
		0);
	TestEqual(
		TEXT("Projectile direct upcast wrapper performs zero host imports"),
		CountAvidScriptJsonStringFieldPrefix(
			ProjectileUpcastFunction,
			TEXT("target_id"),
			TEXT("import:")),
		0);

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!TestTrue(
		TEXT("Typed project API manifest authorizes and loads the emitted WASM"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			BuildResult.ManifestPath,
			Manifest,
			Bytecode,
			ManifestLoadResult)))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("Typed project API emitted non-empty WASM bytecode"), Bytecode.Num() > 0);
	if (!TestTrue(TEXT("Typed project API manifest owns its runtime binding package"), Manifest.BindingPackage.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Typed project API runtime package caches one class reference"), Manifest.BindingPackage->GetClassReferenceCount(), 1);
	UClass* CachedProjectileClass = nullptr;
	UClass* CachedProjectileBaseClass = nullptr;
	if (!TestTrue(
		TEXT("Typed project API runtime package resolves the immutable Blueprint class"),
		Manifest.BindingPackage->TryResolveClassReference(
			0,
			CachedProjectileClass,
			CachedProjectileBaseClass)))
	{
		return false;
	}
	TestTrue(TEXT("Typed project API cached class remains Blueprint concrete"), CachedProjectileClass == BlueprintClass);
	TestTrue(
		TEXT("Typed project API cached base remains the native projectile"),
		CachedProjectileBaseClass == AAvidScriptTypedTestProjectile::StaticClass());

	const TSet<FString> ExpectedImportNames = {
		TEXT("avid_owner_get_handle"),
		TEXT("avid_object_spawn_actor"),
		TEXT("avid_object_destroy_actor"),
		TEXT("avid_object_type_is_a"),
		ActorBinding->HostImport.Name,
		ProjectileBinding->HostImport.Name
	};
	TestEqual(TEXT("Typed project API WASM reaches only six authorized imports"), Manifest.RequiredImports.Num(), ExpectedImportNames.Num());
	for (const FString& ImportName : ExpectedImportNames)
	{
		TestEqual(
			*FString::Printf(TEXT("Typed project API reaches %s exactly once"), *ImportName),
			CountAvidScriptRequiredImport(Manifest, TEXT("avidscript"), ImportName),
			1);
	}
	for (const FAvidScriptWasmRequiredImport& Import : Manifest.RequiredImports)
	{
		TestEqual(TEXT("Typed project API uses only the generated binding module"), Import.ModuleName, FString(TEXT("avidscript")));
		TestTrue(
			TEXT("Typed project API has no project-specific host registration"),
			ExpectedImportNames.Contains(Import.ImportName));
	}
	TestEqual(
		TEXT("Typed project API excludes the legacy class-reference IsA import"),
		CountAvidScriptRequiredImport(Manifest, TEXT("avidscript"), TEXT("avid_object_is_a")),
		0);

	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Typed project API integration World starts"), CreateAvidScriptTypedProjectApiWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptTypedProjectApiWorld(World);
	};
	AAvidScriptTypedTestActor* Owner = World->SpawnActor<AAvidScriptTypedTestActor>();
	AActor* Sentinel = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Typed project API owner spawns"), Owner)
		|| !TestNotNull(TEXT("Typed project API cleanup sentinel spawns"), Sentinel))
	{
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult OwnerRegisterResult;
	const FAvidScriptObjectHandle OwnerHandle = Registry.RegisterObject(Owner, OwnerRegisterResult);
	if (!TestTrue(TEXT("Typed project API owner registers"), OwnerRegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = OwnerHandle;
	HostContext.World = World;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptRuntimeSession Session;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	if (!TestTrue(
		TEXT("Typed project API enters BeginPlay through authorized WAMR"),
		Session.LoadInitialModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			Manifest,
			ReloadResult)))
	{
		AddError(ReloadResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Typed UE.Self custom function executes during BeginPlay"), Owner->GameplayValue, 4.0f);
	AAvidScriptTypedTestProjectile* Projectile =
		FindAvidScriptTypedProjectApiProjectile(*World, *BlueprintClass);
	if (!TestNotNull(TEXT("Typed SpawnActor creates the concrete Blueprint projectile"), Projectile))
	{
		return false;
	}
	TestEqual(TEXT("Typed projectile begins inactive"), Projectile->ActivationCount, 0);
	TestEqual(TEXT("Owner and typed projectile handles are live"), Registry.GetLiveHandleCount(), 2);

	FAvidScriptWasmSmokeResult TickResult;
	if (!TestTrue(TEXT("First typed project API Tick executes through WAMR"), Session.TickLive(0.016f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Successful checked cast executes the projectile function"), Projectile->ActivationCount, 1);
	TestEqual(
		TEXT("Mismatched projectile cast returns default and executes the invalid branch"),
		Owner->GameplayValue,
		6.0f);
	TestTrue(TEXT("First Tick preserves the projectile"), IsValid(Projectile) && !Projectile->IsActorBeingDestroyed());
	TestTrue(TEXT("First Tick preserves the cleanup sentinel"), IsValid(Sentinel));

	if (!TestTrue(TEXT("Second typed project API Tick executes DestroyActor"), Session.TickLive(0.016f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Custom projectile function executes before cleanup"), Projectile->ActivationCount, 2);
	TestEqual(TEXT("Mismatch behavior remains deterministic on the second Tick"), Owner->GameplayValue, 8.0f);
	TestTrue(
		TEXT("Typed cleanup destroys the target projectile"),
		!IsValid(Projectile) || Projectile->IsActorBeingDestroyed());
	TestEqual(TEXT("Typed cleanup releases only the projectile handle"), Registry.GetLiveHandleCount(), 1);
	TestTrue(TEXT("Typed cleanup preserves the owner"), IsValid(Owner));
	TestTrue(TEXT("Typed cleanup preserves the unrelated sentinel"), IsValid(Sentinel));

	FAvidScriptWasmSmokeResult StopResult;
	if (!TestTrue(TEXT("Typed project API runtime stops cleanly"), Session.StopAndUnload(StopResult)))
	{
		AddError(StopResult.ErrorMessage);
		return false;
	}

	FAvidScriptRuntimeSession StaleSession;
	StaleSession.SetHostContext(HostContext);
	if (!TestTrue(
		TEXT("Typed project API stale-handle run enters BeginPlay"),
		StaleSession.LoadInitialModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			Manifest,
			ReloadResult)))
	{
		AddError(ReloadResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Second BeginPlay still executes typed Self"), Owner->GameplayValue, 12.0f);
	AAvidScriptTypedTestProjectile* StaleProjectile =
		FindAvidScriptTypedProjectApiProjectile(*World, *BlueprintClass);
	if (!TestNotNull(TEXT("Stale-handle run spawns a fresh Blueprint projectile"), StaleProjectile))
	{
		return false;
	}
	FAvidScriptObjectHandleResult ProjectileRegisterResult;
	const FAvidScriptObjectHandle ProjectileHandle =
		Registry.RegisterObject(StaleProjectile, ProjectileRegisterResult);
	if (!TestTrue(TEXT("Stale-handle run observes the spawned projectile handle"), ProjectileRegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptObjectHandleResult ReleaseResult;
	if (!TestTrue(
		TEXT("Stale-handle run releases the guest-visible projectile generation"),
		Registry.ReleaseHandle(ProjectileHandle, ReleaseResult)))
	{
		return false;
	}
	TestFalse(
		TEXT("Checked downcast traps a stale generation in WAMR"),
		StaleSession.TickLive(0.016f, TickResult));
	TestEqual(
		TEXT("Stale generation traps at the object-type import"),
		TickResult.ImportName,
		FString(TEXT("avid_object_type_is_a")));
	TestTrue(
		TEXT("Stale generation retains the registry failure detail"),
		TickResult.ErrorMessage.Contains(TEXT("generation_mismatch"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("Stale checked cast cannot execute the projectile function"), StaleProjectile->ActivationCount, 0);
	TestEqual(TEXT("Stale checked cast fails before later gameplay mutation"), Owner->GameplayValue, 12.0f);
	TestTrue(TEXT("Stale checked cast does not destroy the projectile"), IsValid(StaleProjectile));
	TestTrue(TEXT("Stale checked cast preserves the unrelated sentinel"), IsValid(Sentinel));
	StaleSession.UnloadLive();
	StaleProjectile->Destroy();
	TestEqual(TEXT("Manual stale-case cleanup leaves only the owner handle"), Registry.GetLiveHandleCount(), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
