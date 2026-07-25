#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
constexpr const TCHAR* DynamicImportName = TEXT("avid_ue_1111111111111111");
constexpr const TCHAR* DynamicStableId = TEXT("1111111111111111111111111111111111111111111111111111111111111111");

FAvidScriptVmDynamicImport MakeDynamicImport(uint32 Ordinal)
{
	FAvidScriptVmDynamicImport Import;
	Import.StableId = DynamicStableId;
	Import.Ordinal = Ordinal;
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = DynamicImportName;
	Import.Signature = TEXT("(i)i");
	return Import;
}

FAvidScriptVmBindingPackage MakeDynamicPackage(const TCHAR* HashCharacter, uint32 TargetOrdinal)
{
	FAvidScriptVmBindingPackage Package;
	Package.PackageName = TEXT("avidscript.phase42.dynamic");
	Package.PackageHash = FString::ChrN(64, HashCharacter[0]);
	if (TargetOrdinal > 0)
	{
		FAvidScriptVmDynamicImport Padding;
		Padding.StableId = TEXT("2222222222222222222222222222222222222222222222222222222222222222");
		Padding.Ordinal = 0;
		Padding.ModuleName = TEXT("avidscript");
		Padding.ImportName = TEXT("avid_ue_2222222222222222");
		Padding.Signature = TEXT("(i)i");
		Package.Imports.Add(MoveTemp(Padding));
	}
	Package.Imports.Add(MakeDynamicImport(TargetOrdinal));
	return Package;
}

bool LoadDynamicFixture(TArray<uint8>& OutBytecode)
{
	const FString FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/WasmBackend/P42_3_DynamicRawImport.wasm")));
	return FFileHelper::LoadFileToArray(OutBytecode, *FixturePath);
}

class FAvidScriptDynamicRawTestDispatcher final : public IAvidScriptHostDispatcher
{
public:
	bool DispatchHostCall(const FAvidScriptHostCall&, FAvidScriptHostCallResult&) override
	{
		return false;
	}

	bool DispatchDynamicHostCall(
		const FAvidScriptDynamicHostCall& Call,
		FAvidScriptDynamicHostCallResult& OutResult) override
	{
		++CallCount;
		LastOrdinal = Call.BindingOrdinal;
		LastArgumentCount = Call.Arguments.Num();
		LastInput = Call.Arguments.IsEmpty() ? 0 : static_cast<int32>(Call.Arguments[0]);
		bSawGuestMemory = Call.GuestMemory != nullptr;
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = LastInput + 1;
		if (BackendToUnload != nullptr)
		{
			IAvidScriptVmBackend* RequestedBackend = BackendToUnload;
			BackendToUnload = nullptr;
			bRequestedUnload = true;
			RequestedBackend->Unload();
		}
		return true;
	}

	IAvidScriptVmBackend* BackendToUnload = nullptr;
	int32 CallCount = 0;
	uint32 LastOrdinal = MAX_uint32;
	int32 LastArgumentCount = 0;
	int32 LastInput = 0;
	bool bSawGuestMemory = false;
	bool bRequestedUnload = false;
};

bool CallBeginPlay(
	IAvidScriptVmBackend& Backend,
	FAvidScriptVmError& OutError)
{
	FAvidScriptVmExportHandle BeginPlay;
	if (!Backend.ResolveExport(TEXT("avid_on_begin_play"), BeginPlay, OutError))
	{
		return false;
	}
	return Backend.Call(BeginPlay, FAvidScriptVmCallFrame(), OutError);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmDynamicRawRegistrySmokeTest,
	"AvidScript.Architecture.VM.DynamicRawRegistrySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmDynamicRawRegistrySmokeTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Bytecode;
	if (!TestTrue(TEXT("Generated dynamic raw fixture loads"), LoadDynamicFixture(Bytecode)))
	{
		return false;
	}

	FAvidScriptVmBindingPackage FirstPackage = MakeDynamicPackage(TEXT("a"), 0);
	FAvidScriptVmBindingPackage SecondPackage = MakeDynamicPackage(TEXT("b"), 1);
	FAvidScriptDynamicRawTestDispatcher FirstDispatcher;
	FAvidScriptDynamicRawTestDispatcher SecondDispatcher;
	FAvidScriptVmLoadConfig FirstConfig;
	FirstConfig.HostDispatcher = &FirstDispatcher;
	FirstConfig.BindingPackage = &FirstPackage;
	FAvidScriptVmLoadConfig SecondConfig;
	SecondConfig.HostDispatcher = &SecondDispatcher;
	SecondConfig.BindingPackage = &SecondPackage;

	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> FirstBackend = CreateAvidScriptWamrBackend();
	TUniquePtr<IAvidScriptVmBackend> SecondBackend = CreateAvidScriptWamrBackend();
	TestTrue(TEXT("First package attaches"), FirstBackend->Load(Bytecode, TEXT("dynamic_raw_first"), FirstConfig, Error));
	TestTrue(TEXT("Second package reuses the global raw symbol"), SecondBackend->Load(Bytecode, TEXT("dynamic_raw_second"), SecondConfig, Error));
	TestTrue(TEXT("First package dynamic import executes"), CallBeginPlay(*FirstBackend, Error));
	TestTrue(TEXT("Second package dynamic import executes"), CallBeginPlay(*SecondBackend, Error));
	TestEqual(TEXT("First package receives local ordinal zero"), FirstDispatcher.LastOrdinal, 0u);
	TestEqual(TEXT("Second package receives local ordinal one"), SecondDispatcher.LastOrdinal, 1u);
	TestEqual(TEXT("Raw callback receives one argument"), SecondDispatcher.LastArgumentCount, 1);
	TestEqual(TEXT("Raw callback preserves the i32 argument"), SecondDispatcher.LastInput, 41);
	TestTrue(TEXT("Dynamic call exposes language-neutral guest memory"), SecondDispatcher.bSawGuestMemory);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmDynamicRawRegistryReentrantUnloadTest,
	"AvidScript.Architecture.VM.DynamicRawRegistryReentrantUnload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmDynamicRawRegistryReentrantUnloadTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Bytecode;
	if (!TestTrue(TEXT("Generated dynamic raw fixture loads"), LoadDynamicFixture(Bytecode)))
	{
		return false;
	}

	FAvidScriptVmBindingPackage Package = MakeDynamicPackage(TEXT("f"), 0);
	FAvidScriptDynamicRawTestDispatcher Dispatcher;
	FAvidScriptVmLoadConfig Config;
	Config.HostDispatcher = &Dispatcher;
	Config.BindingPackage = &Package;

	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptWamrBackend();
	if (!TestTrue(TEXT("Reentrant unload fixture attaches"), Backend->Load(
		Bytecode,
		TEXT("dynamic_raw_reentrant_unload"),
		Config,
		Error)))
	{
		return false;
	}

	Dispatcher.BackendToUnload = Backend.Get();
	TestFalse(TEXT("Unload requested inside a host callback fails the active call safely"), CallBeginPlay(*Backend, Error));
	TestTrue(TEXT("Host callback requested unload"), Dispatcher.bRequestedUnload);
	TestEqual(TEXT("Reentrant unload has a stable category"), Error.Category, FString(TEXT("reentrant_unload")));
	TestFalse(TEXT("Deferred unload completes after the active WAMR call unwinds"), Backend->IsLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmDynamicRawRegistryFailureTest,
	"AvidScript.Architecture.VM.DynamicRawRegistryFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmDynamicRawRegistryFailureTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Bytecode;
	if (!TestTrue(TEXT("Generated dynamic raw fixture loads"), LoadDynamicFixture(Bytecode)))
	{
		return false;
	}

	FAvidScriptDynamicRawTestDispatcher Dispatcher;
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> MissingPackageBackend = CreateAvidScriptWamrBackend();
	FAvidScriptVmLoadConfig MissingPackageConfig;
	MissingPackageConfig.HostDispatcher = &Dispatcher;
	TestFalse(TEXT("Dynamic import without a package fails closed"), MissingPackageBackend->Load(
		Bytecode,
		TEXT("dynamic_raw_missing_package"),
		MissingPackageConfig,
		Error));
	TestEqual(TEXT("Missing package reports a stable category"), Error.Category, FString(TEXT("binding_package_missing")));

	FAvidScriptVmBindingPackage InvalidPackage = MakeDynamicPackage(TEXT("c"), 0);
	InvalidPackage.Imports.Add(MakeDynamicImport(0));
	FAvidScriptVmLoadConfig InvalidConfig;
	InvalidConfig.HostDispatcher = &Dispatcher;
	InvalidConfig.BindingPackage = &InvalidPackage;
	TUniquePtr<IAvidScriptVmBackend> InvalidBackend = CreateAvidScriptWamrBackend();
	TestFalse(TEXT("Duplicate dynamic ordinals fail before module load"), InvalidBackend->Load(
		Bytecode,
		TEXT("dynamic_raw_invalid_package"),
		InvalidConfig,
		Error));
	TestEqual(TEXT("Invalid package reports a stable category"), Error.Category, FString(TEXT("dynamic_package_invalid")));

	FAvidScriptVmBindingPackage FirstPackage = MakeDynamicPackage(TEXT("d"), 0);
	FAvidScriptVmLoadConfig FirstConfig;
	FirstConfig.HostDispatcher = &Dispatcher;
	FirstConfig.BindingPackage = &FirstPackage;
	TUniquePtr<IAvidScriptVmBackend> FirstBackend = CreateAvidScriptWamrBackend();
	TestTrue(TEXT("Conflict seed package attaches"), FirstBackend->Load(Bytecode, TEXT("dynamic_raw_conflict_seed"), FirstConfig, Error));

	TUniquePtr<IAvidScriptVmBackend> BorrowingBackend = CreateAvidScriptWamrBackend();
	TestFalse(TEXT("A warmed global registry cannot authorize a package-less VM"), BorrowingBackend->Load(
		Bytecode,
		TEXT("dynamic_raw_warmed_registry_borrow"),
		MissingPackageConfig,
		Error));
	TestEqual(TEXT("Warmed registry borrowing is rejected before WAMR load"), Error.Category, FString(TEXT("binding_package_missing")));

	FAvidScriptVmBindingPackage WrongPackage;
	WrongPackage.PackageName = TEXT("avidscript.phase50.wrong");
	WrongPackage.PackageHash = FString::ChrN(64, TEXT('f'));
	FAvidScriptVmDynamicImport WrongImport;
	WrongImport.StableId = TEXT("2222222222222222222222222222222222222222222222222222222222222222");
	WrongImport.Ordinal = 0;
	WrongImport.ModuleName = TEXT("avidscript");
	WrongImport.ImportName = TEXT("avid_ue_2222222222222222");
	WrongImport.Signature = TEXT("(i)i");
	WrongPackage.Imports.Add(MoveTemp(WrongImport));
	FAvidScriptVmLoadConfig WrongPackageConfig;
	WrongPackageConfig.HostDispatcher = &Dispatcher;
	WrongPackageConfig.BindingPackage = &WrongPackage;
	TUniquePtr<IAvidScriptVmBackend> WrongPackageBackend = CreateAvidScriptWamrBackend();
	TestFalse(TEXT("A warmed registry cannot authorize an import absent from the current non-empty package"), WrongPackageBackend->Load(
		Bytecode,
		TEXT("dynamic_raw_wrong_package_borrow"),
		WrongPackageConfig,
		Error));
	TestEqual(TEXT("Wrong current package borrowing is rejected before WAMR load"), Error.Category, FString(TEXT("binding_package_import_mismatch")));

	FAvidScriptVmBindingPackage ConflictPackage = MakeDynamicPackage(TEXT("e"), 0);
	ConflictPackage.Imports[0].StableId = TEXT("3333333333333333333333333333333333333333333333333333333333333333");
	FAvidScriptVmLoadConfig ConflictConfig;
	ConflictConfig.HostDispatcher = &Dispatcher;
	ConflictConfig.BindingPackage = &ConflictPackage;
	TUniquePtr<IAvidScriptVmBackend> ConflictBackend = CreateAvidScriptWamrBackend();
	TestFalse(TEXT("Same module and name cannot change stable identity"), ConflictBackend->Load(
		Bytecode,
		TEXT("dynamic_raw_conflict"),
		ConflictConfig,
		Error));
	TestEqual(TEXT("Registry conflict reports a stable category"), Error.Category, FString(TEXT("dynamic_import_conflict")));

	FirstBackend->Unload();
	TestTrue(TEXT("Released import can be registered with a new identity"), ConflictBackend->Load(
		Bytecode,
		TEXT("dynamic_raw_after_release"),
		ConflictConfig,
		Error));
	return true;
}

#endif
