#if WITH_DEV_AUTOMATION_TESTS

#include "Demos/AvidScriptUiSaveDemoAssets.h"

#include "AvidScriptHash.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptUiSaveDemoAssetsTest,
	"AvidScript.Editor.Demos.UiSaveAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptUiSaveDemoAssetsTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	TestNotNull(TEXT("PrepareUiSaveDemo console command is registered"),
		IConsoleManager::Get().FindConsoleObject(TEXT("AvidScript.PrepareUiSaveDemo")));
	const FString Root = TEXT("/AvidScript/Automation/UiSave_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PartialRoot = Root + TEXT("_Partial");
	const TCHAR* Names[] = { TEXT("WBP_UiSave"), TEXT("BP_UiSaveHost"), TEXT("BP_PlayerSave"), TEXT("L_UiSave") };
	ON_SCOPE_EXIT
	{
		for (const FString& OwnedRoot : { Root, PartialRoot })
		{
			for (const TCHAR* Name : Names)
			{
				const FString PackagePath = OwnedRoot / Name;
				UObject* Asset = FindObject<UObject>(nullptr, *(PackagePath + TEXT(".") + Name));
				if (Asset)
				{
					FAssetRegistryModule::AssetDeleted(Asset);
					if (UWorld* World = Cast<UWorld>(Asset)) { World->DestroyWorld(false); }
					Asset->GetOutermost()->SetDirtyFlag(false);
					Asset->ClearFlags(RF_Public | RF_Standalone);
				}
				// The unique fixture root owns only these four exact package files.
				const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath,
					FString(Name) == TEXT("L_UiSave") ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
				IFileManager::Get().Delete(*Filename, false, true);
			}
		}
	};
	FString Error;
	bool bReused = false;
	if (!TestTrue(TEXT("Factories create, compile, validate and save all four assets"),
		AvidScript::UiSaveDemo::Prepare(Error, bReused, Root)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("First preparation creates assets"), bReused);
	TArray<FString> Hashes;
	for (const TCHAR* Name : Names)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(Root / Name,
			FString(Name) == TEXT("L_UiSave") ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
		TArray<uint8> Bytes;
		TestTrue(TEXT("Generated package exists on disk"), FFileHelper::LoadFileToArray(Bytes, *Filename));
		Hashes.Add(FAvidScriptHash::Sha256Hex(Bytes));
	}
	TestTrue(TEXT("Second preparation validates the existing set"), AvidScript::UiSaveDemo::Prepare(Error, bReused, Root));
	TestTrue(TEXT("Second preparation is idempotent"), bReused);
	UWidgetBlueprint* Widget = LoadObject<UWidgetBlueprint>(nullptr, *(Root / TEXT("WBP_UiSave.WBP_UiSave")));
	if (!TestNotNull(TEXT("Widget Blueprint can be loaded"), Widget)) { return false; }
	UTextBlock* Status = Cast<UTextBlock>(Widget->WidgetTree->FindWidget(TEXT("StatusText")));
	if (!TestNotNull(TEXT("StatusText is a real TextBlock"), Status)) { return false; }
	Status->SetText(FText::FromString(TEXT("User-edited presentation")));
	Widget->MarkPackageDirty();
	TestTrue(TEXT("Valid user presentation edits are preserved"), AvidScript::UiSaveDemo::Prepare(Error, bReused, Root));
	TestEqual(TEXT("Preparation did not overwrite user text"), Status->GetText().ToString(), FString(TEXT("User-edited presentation")));
	Status->Rename(TEXT("UserRenamedStatus"), Widget->WidgetTree, REN_DontCreateRedirectors | REN_NonTransactional);
	TestFalse(TEXT("Incompatible user edits are rejected"), AvidScript::UiSaveDemo::Prepare(Error, bReused, Root));
	TestTrue(TEXT("Rejection names the broken widget contract"), Error.Contains(TEXT("StatusText")));
	TestEqual(TEXT("Rejection did not repair the user's asset"), Status->GetFName(), FName(TEXT("UserRenamedStatus")));
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(Root / Names[Index],
			Index == 3 ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
		TArray<uint8> Bytes;
		TestTrue(TEXT("Original saved package remains readable"), FFileHelper::LoadFileToArray(Bytes, *Filename));
		TestEqual(TEXT("Reuse and rejection never rewrite saved assets"), FAvidScriptHash::Sha256Hex(Bytes), Hashes[Index]);
	}
	CreatePackage(*(PartialRoot / TEXT("BP_PlayerSave")));
	TestFalse(TEXT("Partial preexisting asset set is rejected before creation"),
		AvidScript::UiSaveDemo::Prepare(Error, bReused, PartialRoot));
	TestTrue(TEXT("Partial rejection is explicit"), Error.Contains(TEXT("Partial asset set")));
	TestNull(TEXT("Partial rejection did not create a Widget package"), FindPackage(nullptr, *(PartialRoot / TEXT("WBP_UiSave"))));
	TestFalse(TEXT("Generator rejects roots outside the plugin"), AvidScript::UiSaveDemo::Prepare(Error, bReused, TEXT("/Game/UiSave")));
	return true;
}

#endif
