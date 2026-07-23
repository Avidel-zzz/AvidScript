#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorTypedObjectBindingTestTypes.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptObjectTypeBinding.h"

#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
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

#endif // WITH_DEV_AUTOMATION_TESTS
