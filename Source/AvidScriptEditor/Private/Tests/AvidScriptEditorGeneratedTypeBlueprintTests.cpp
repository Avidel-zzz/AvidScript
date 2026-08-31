#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallParentFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
bool CreateBlueprintScriptWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptBlueprintSubclassWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyBlueprintScriptWorld(UWorld*& World)
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

bool UnloadBlueprintPackage(UPackage*& Package, FString& OutError)
{
	OutError.Reset();
	if (Package == nullptr)
	{
		return true;
	}
	Package->SetDirtyFlag(false);
	TArray<UPackage*> Packages{ Package };
	FText ErrorText;
	if (!UPackageTools::UnloadPackages(Packages, ErrorText, true))
	{
		OutError = ErrorText.ToString();
		return false;
	}
	Package = nullptr;
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedTypeBlueprintSubclassTest,
	"AvidScript.Editor.GeneratedTypes.BlueprintSubclass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedTypeBlueprintSubclassTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	UClass* const ScriptParent = FindObject<UClass>(
		nullptr,
		TEXT("/Script/AvidScriptGenerated.Projectile"));
	if (!TestNotNull(TEXT("C# generated Projectile UClass is loaded"), ScriptParent))
	{
		return true;
	}
	TestTrue(
		TEXT("C# generated Projectile is Blueprintable"),
		FKismetEditorUtilities::CanCreateBlueprintOfClass(ScriptParent));

	const FString AssetName = FString::Printf(
		TEXT("BP_AvidScriptP59D1_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackageName = TEXT("/Game/") + AssetName;
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	UPackage* ActivePackage = nullptr;
	UWorld* World = nullptr;
	ON_SCOPE_EXIT
	{
		DestroyBlueprintScriptWorld(World);
		FString UnloadError;
		UnloadBlueprintPackage(ActivePackage, UnloadError);
		IFileManager::Get().Delete(*PackageFilename, false, true, true);
	};

	ActivePackage = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("Blueprint package is created"), ActivePackage))
	{
		return true;
	}
	{
		TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(
			ScriptParent,
			ActivePackage,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			TEXT("AvidScriptP59D1")));
		if (!TestNotNull(TEXT("Blueprint subclass is created"), Blueprint.Get()))
		{
			return true;
		}
		UEdGraph* const ActivateOverrideGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint.Get(),
			TEXT("Activate"),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph(
			Blueprint.Get(),
			ActivateOverrideGraph,
			false,
			ScriptParent);
		TArray<UK2Node_CallParentFunction*> ParentCalls;
		ActivateOverrideGraph->GetNodesOfClass(ParentCalls);
		TestEqual(
			TEXT("Blueprint override graph contains one Parent Call"),
			ParentCalls.Num(),
			1);
		FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
		UClass* const BlueprintClass = Blueprint->GeneratedClass;
		if (!TestNotNull(TEXT("Blueprint subclass compiles"), BlueprintClass))
		{
			return true;
		}
		TestTrue(
			TEXT("Blueprint generated class derives from the C# generated UClass"),
			BlueprintClass->IsChildOf(ScriptParent));

		FFloatProperty* const DamageProperty =
			FindFProperty<FFloatProperty>(BlueprintClass, TEXT("Damage"));
		UFunction* const ActivateFunction = BlueprintClass->FindFunctionByName(TEXT("Activate"));
		UFunction* const ActivateOverride = BlueprintClass->FindFunctionByName(
			TEXT("Activate"),
			EIncludeSuperFlag::ExcludeSuper);
		if (!TestNotNull(TEXT("Blueprint sees the C# Damage property"), DamageProperty)
			|| !TestNotNull(TEXT("Blueprint sees the C# Activate function"), ActivateFunction)
			|| !TestNotNull(TEXT("Blueprint owns the Activate override"), ActivateOverride))
		{
			return true;
		}
		TestTrue(
			TEXT("Damage is Blueprint read/write"),
			DamageProperty->HasAnyPropertyFlags(CPF_BlueprintVisible)
				&& !DamageProperty->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
		TestTrue(
			TEXT("Activate is Blueprint callable"),
			ActivateFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		TestFalse(
			TEXT("Blueprint Activate override is bytecode rather than native"),
			ActivateOverride->HasAnyFunctionFlags(FUNC_Native));
		TestTrue(
			TEXT("Blueprint Activate override owns bytecode"),
			!ActivateOverride->Script.IsEmpty());

		UObject* const BlueprintDefaultObject = BlueprintClass->GetDefaultObject();
		TestEqual(
			TEXT("Blueprint CDO inherits the C# native default"),
			DamageProperty->GetPropertyValue_InContainer(BlueprintDefaultObject),
			25.0f);
		DamageProperty->SetPropertyValue_InContainer(BlueprintDefaultObject, 40.0f);
		Blueprint->MarkPackageDirty();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!TestTrue(
				TEXT("Blueprint package saves"),
				UPackage::SavePackage(
					ActivePackage,
					Blueprint.Get(),
					*PackageFilename,
					SaveArgs)))
		{
			return true;
		}
	}

	FString UnloadError;
	if (!TestTrue(
			TEXT("Saved Blueprint package unloads"),
			UnloadBlueprintPackage(ActivePackage, UnloadError)))
	{
		AddError(UnloadError);
		return true;
	}
	ActivePackage = LoadPackage(nullptr, *PackageName, LOAD_None);
	if (!TestNotNull(TEXT("Saved Blueprint package reloads"), ActivePackage))
	{
		return true;
	}
	UBlueprint* const ReloadedBlueprint = FindObject<UBlueprint>(
		ActivePackage,
		*AssetName);
	if (!TestNotNull(TEXT("Reloaded package contains the Blueprint"), ReloadedBlueprint))
	{
		return true;
	}
	UClass* const ReloadedClass = ReloadedBlueprint->GeneratedClass.Get();
	if (!TestNotNull(TEXT("Reloaded Blueprint has a generated class"), ReloadedClass))
	{
		return true;
	}
	TestTrue(
		TEXT("Reloaded Blueprint remains a child of the C# generated UClass"),
		ReloadedClass->IsChildOf(ScriptParent));
	UFunction* const ReloadedActivateOverride = ReloadedClass->FindFunctionByName(
		TEXT("Activate"),
		EIncludeSuperFlag::ExcludeSuper);
	if (!TestNotNull(
			TEXT("Reloaded Blueprint retains the Activate override"),
			ReloadedActivateOverride))
	{
		return true;
	}
	TestTrue(
		TEXT("Reloaded Blueprint retains Activate bytecode"),
		!ReloadedActivateOverride->Script.IsEmpty());
	FFloatProperty* const ReloadedDamage =
		FindFProperty<FFloatProperty>(ReloadedClass, TEXT("Damage"));
	FIntProperty* const ReloadedActivationCount =
		FindFProperty<FIntProperty>(ReloadedClass, TEXT("ActivationCount"));
	FBoolProperty* const ReloadedHasBegunPlay =
		FindFProperty<FBoolProperty>(ReloadedClass, TEXT("HasBegunPlay"));
	if (!TestNotNull(TEXT("Reloaded Damage property resolves"), ReloadedDamage)
		|| !TestNotNull(TEXT("Reloaded ActivationCount property resolves"), ReloadedActivationCount)
		|| !TestNotNull(TEXT("Reloaded HasBegunPlay property resolves"), ReloadedHasBegunPlay))
	{
		return true;
	}
	TestEqual(
		TEXT("Blueprint CDO override survives save and reload"),
		ReloadedDamage->GetPropertyValue_InContainer(ReloadedClass->GetDefaultObject()),
		40.0f);

	if (!CreateBlueprintScriptWorld(World))
	{
		AddError(TEXT("Failed to create the Blueprint subclass test world."));
		return true;
	}
	AActor* const Actor = World->SpawnActor<AActor>(ReloadedClass);
	if (!TestNotNull(TEXT("Reloaded Blueprint subclass spawns"), Actor))
	{
		return true;
	}
	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	if (!Actor->HasActorBegunPlay())
	{
		Actor->DispatchBeginPlay();
	}
	TestTrue(
		TEXT("Blueprint subclass reaches the inherited C# BeginPlay"),
		ReloadedHasBegunPlay->GetPropertyValue_InContainer(Actor));

	UFunction* const ReloadedActivate = Actor->FindFunction(TEXT("Activate"));
	if (!TestNotNull(TEXT("Reloaded Blueprint instance exposes Activate"), ReloadedActivate))
	{
		return true;
	}
	struct FActivateParameters
	{
		float DamageScale = 2.0f;
	} ActivateParameters;
	Actor->ProcessEvent(ReloadedActivate, &ActivateParameters);
	TestEqual(
		TEXT("Blueprint ProcessEvent reaches C# gameplay logic"),
		ReloadedDamage->GetPropertyValue_InContainer(Actor),
		80.0f);
	TestEqual(
		TEXT("Blueprint subclass preserves C# state interaction"),
		ReloadedActivationCount->GetPropertyValue_InContainer(Actor),
		2);

	return true;
}

#endif
