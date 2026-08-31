#if WITH_DEV_AUTOMATION_TESTS

#include "GeneratedTypes/AvidScriptEditorGeneratedTypeReloadService.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
class FFakeGeneratedTypeReloadWatchHost final
	: public IAvidScriptEditorCSharpLiveReloadWatchHost
{
public:
	virtual bool Start(
		const FString& WorkspaceRoot,
		FOnChangeBatch InOnChangeBatch,
		FString& OutErrorCategory,
		FString& OutErrorMessage) override
	{
		(void)OutErrorCategory;
		(void)OutErrorMessage;
		WatchedRoot = WorkspaceRoot;
		Callback = MoveTemp(InOnChangeBatch);
		bWatching = true;
		return true;
	}

	virtual void Stop() override
	{
		bWatching = false;
		Callback = FOnChangeBatch();
	}

	virtual bool IsWatching() const override
	{
		return bWatching;
	}

	void Emit(TArray<FString> FilePaths, const bool bRescanRequired = false)
	{
		if (!Callback)
		{
			return;
		}
		FAvidScriptEditorCSharpLiveReloadChangeBatch Batch;
		Batch.FilePaths = MoveTemp(FilePaths);
		Batch.bRescanRequired = bRescanRequired;
		Callback(MoveTemp(Batch));
	}

	bool bWatching = false;
	FString WatchedRoot;
	FOnChangeBatch Callback;
};

struct FFakeGeneratedTypeReloadApplyState
{
	bool Apply(
		const FString& DescriptorPath,
		const EAvidScriptEditorGeneratedTypeReloadClassification Classification,
		FAvidScriptEditorGeneratedTypeReloadServiceResult& OutResult)
	{
		++CallCount;
		LastDescriptorPath = DescriptorPath;
		LastClassification = Classification;
		OutResult.bSucceeded = true;
		if (Classification ==
			EAvidScriptEditorGeneratedTypeReloadClassification::NativeRebuildRequired)
		{
			OutResult.Status =
				EAvidScriptEditorGeneratedTypeReloadStatus::NativeRebuildRequired;
			OutResult.ErrorCategory = TEXT("generated_type_native_rebuild_required");
			OutResult.NextAction = TEXT("fixture rebuild");
			return true;
		}
		OutResult.bRuntimeMutationAttempted = true;
		OutResult.bRuntimeApplied = true;
		OutResult.Status = Classification ==
			EAvidScriptEditorGeneratedTypeReloadClassification::InitialInstall
			? EAvidScriptEditorGeneratedTypeReloadStatus::InitialInstallApplied
			: EAvidScriptEditorGeneratedTypeReloadStatus::BodyOnlyApplied;
		return true;
	}

	int32 CallCount = 0;
	FString LastDescriptorPath;
	EAvidScriptEditorGeneratedTypeReloadClassification LastClassification =
		EAvidScriptEditorGeneratedTypeReloadClassification::InitialInstall;
};

bool WriteGeneratedTypeReloadDescriptor(
	const FString& DescriptorPath,
	const FString& PackageId,
	const FString& PreviousPackageId,
	const FString& NativeStructure,
	const FString& PreviousNativeStructure,
	const FString& Classification)
{
	const TSharedRef<FJsonObject> Reload = MakeShared<FJsonObject>();
	Reload->SetNumberField(TEXT("schema_version"), 1);
	Reload->SetStringField(TEXT("classification"), Classification);
	Reload->SetStringField(TEXT("native_structure_sha256"), NativeStructure);
	Reload->SetStringField(
		TEXT("previous_native_structure_sha256"),
		PreviousNativeStructure);
	Reload->SetStringField(TEXT("previous_package_id"), PreviousPackageId);

	const TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
	Descriptor->SetNumberField(TEXT("schema_version"), 1);
	Descriptor->SetStringField(TEXT("package_id"), PackageId);
	Descriptor->SetObjectField(TEXT("reload"), Reload);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	return FJsonSerializer::Serialize(Descriptor, Writer)
		&& FFileHelper::SaveStringToFile(Json, *DescriptorPath);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedTypeReloadServiceTest,
	"AvidScript.Editor.GeneratedTypes.ReloadService",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedTypeReloadServiceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString FixtureRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P59/GeneratedTypeReloadService")));
	IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
	IFileManager::Get().MakeDirectory(*FixtureRoot, true);
	const FString DescriptorPath = FPaths::Combine(
		FixtureRoot,
		TEXT("AvidScriptGeneratedPackage.json"));
	const FString PackageA = FString::ChrN(64, TEXT('a'));
	const FString PackageB = FString::ChrN(64, TEXT('b'));
	const FString PackageC = FString::ChrN(64, TEXT('c'));
	const FString PackageD = FString::ChrN(64, TEXT('d'));
	const FString StructureA = FString::ChrN(64, TEXT('1'));
	const FString StructureB = FString::ChrN(64, TEXT('2'));

	TestTrue(
		TEXT("Initial descriptor writes"),
		WriteGeneratedTypeReloadDescriptor(
			DescriptorPath,
			PackageA,
			FString(),
			StructureA,
			FString(),
			TEXT("initial_install")));

	TUniquePtr<FFakeGeneratedTypeReloadWatchHost> FakeWatch =
		MakeUnique<FFakeGeneratedTypeReloadWatchHost>();
	FFakeGeneratedTypeReloadWatchHost* FakeWatchPtr = FakeWatch.Get();
	TSharedRef<FFakeGeneratedTypeReloadApplyState> ApplyState =
		MakeShared<FFakeGeneratedTypeReloadApplyState>();
	FAvidScriptEditorGeneratedTypeReloadService Service(
		MoveTemp(FakeWatch),
		[ApplyState](
			const FString& Path,
			const EAvidScriptEditorGeneratedTypeReloadClassification Classification,
			FAvidScriptEditorGeneratedTypeReloadServiceResult& OutResult)
		{
			return ApplyState->Apply(Path, Classification, OutResult);
		});
	FAvidScriptEditorGeneratedTypeReloadServiceResult StartResult;
	if (!TestTrue(
			TEXT("Generated type descriptor watcher starts"),
			Service.Start(DescriptorPath, StartResult)))
	{
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return false;
	}
	TestEqual(
		TEXT("Watcher baseline is the package installed at Editor startup"),
		StartResult.DescriptorIdentity.PackageId,
		PackageA);
	TestEqual(TEXT("Watcher root is descriptor parent"), FakeWatchPtr->WatchedRoot, FixtureRoot);

	FakeWatchPtr->Emit({FPaths::Combine(FixtureRoot, TEXT("unrelated.tmp"))});
	Service.Tick();
	TestEqual(TEXT("Unrelated changes do not apply"), ApplyState->CallCount, 0);

	TestTrue(
		TEXT("Body-only descriptor writes"),
		WriteGeneratedTypeReloadDescriptor(
			DescriptorPath,
			PackageB,
			PackageA,
			StructureA,
			StructureA,
			TEXT("body_only")));
	FakeWatchPtr->Emit({DescriptorPath});
	FakeWatchPtr->Emit({DescriptorPath});
	Service.Tick();
	TestEqual(TEXT("Coalesced body-only publish applies once"), ApplyState->CallCount, 1);
	TestEqual(
		TEXT("Body-only classification reaches the apply boundary"),
		ApplyState->LastClassification,
		EAvidScriptEditorGeneratedTypeReloadClassification::BodyOnly);
	TestEqual(
		TEXT("Body-only package becomes active"),
		Service.GetLastResult().DescriptorIdentity.PackageId,
		PackageB);
	TestTrue(TEXT("Body-only result reports runtime apply"), Service.GetLastResult().bRuntimeApplied);

	FakeWatchPtr->Emit({DescriptorPath});
	Service.Tick();
	TestEqual(TEXT("Duplicate package does not apply twice"), ApplyState->CallCount, 1);
	TestEqual(
		TEXT("Duplicate package is explicit"),
		Service.GetLastResult().Status,
		EAvidScriptEditorGeneratedTypeReloadStatus::DuplicateIgnored);

	TestTrue(
		TEXT("Structural descriptor writes"),
		WriteGeneratedTypeReloadDescriptor(
			DescriptorPath,
			PackageC,
			PackageB,
			StructureB,
			StructureA,
			TEXT("native_rebuild_required")));
	FakeWatchPtr->Emit({}, true);
	Service.Tick();
	TestEqual(TEXT("Rescan applies structural classification"), ApplyState->CallCount, 2);
	TestEqual(
		TEXT("Structural update stops at native rebuild"),
		Service.GetLastResult().Status,
		EAvidScriptEditorGeneratedTypeReloadStatus::NativeRebuildRequired);
	TestFalse(
		TEXT("Structural update does not mutate Runtime"),
		Service.GetLastResult().bRuntimeMutationAttempted);

	TestTrue(
		TEXT("Stale-chain descriptor writes"),
		WriteGeneratedTypeReloadDescriptor(
			DescriptorPath,
			PackageD,
			PackageA,
			StructureB,
			StructureB,
			TEXT("body_only")));
	FakeWatchPtr->Emit({DescriptorPath});
	Service.Tick();
	TestEqual(TEXT("Stale package chain never reaches Runtime"), ApplyState->CallCount, 2);
	TestEqual(
		TEXT("Stale package chain is rejected"),
		Service.GetLastResult().ErrorCategory,
		FString(TEXT("generated_type_reload_chain_mismatch")));
	TestEqual(TEXT("Rejected count is exact"), Service.GetLastResult().Stats.RejectedCount, uint64(1));

	Service.Stop();
	TestFalse(TEXT("Watcher stops"), Service.IsRunning());
	IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
	return true;
}

#endif
