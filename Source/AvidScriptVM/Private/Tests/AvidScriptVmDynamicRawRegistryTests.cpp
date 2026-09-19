#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"
#include "AvidScriptVmResultFixtureBuilder.h"

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
		if (bReject)
		{
			OutResult = FAvidScriptDynamicHostCallResult();
			OutResult.ErrorCategory = RejectCategory;
			OutResult.Details = RejectDetails;
			return false;
		}
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
	bool bReject = false;
	FString RejectCategory = TEXT("binding_test_rejected");
	FString RejectDetails = TEXT("The test dispatcher rejected the dynamic binding.");
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
	SecondDispatcher.bReject = true;
	TestFalse(TEXT("WAMR surfaces a structured dynamic host rejection"), CallBeginPlay(*SecondBackend, Error));
	TestEqual(TEXT("WAMR preserves the dynamic failure category"), Error.Category, SecondDispatcher.RejectCategory);
	TestEqual(TEXT("WAMR preserves the dynamic failure details"), Error.Details, SecondDispatcher.RejectDetails);
	TestEqual(TEXT("WAMR preserves the dynamic import module"), Error.ImportModuleName, FString(TEXT("avidscript")));
	TestEqual(TEXT("WAMR preserves the dynamic import name"), Error.ImportName, FString(DynamicImportName));
	SecondDispatcher.bReject = false;
	TestTrue(TEXT("WAMR clears the previous failure before the next call"), CallBeginPlay(*SecondBackend, Error));
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

namespace
{
struct FSupplementalScalarTestState
{
	uint64 Bits = 0;
	int32 Calls = 0;
	bool bReject = false;
};

template <typename ValueType>
EAvidScriptVmTypedHostStatus ReadSupplementalScalar(void* Context, int64 Self, ValueType& OutValue)
{
	auto& State = *static_cast<FSupplementalScalarTestState*>(Context);
	++State.Calls;
	if (State.bReject || Self != 0x1234567887654321LL) return EAvidScriptVmTypedHostStatus::Rejected;
	using BitsType = std::conditional_t<sizeof(ValueType) == 4, uint32, uint64>;
	const BitsType Bits = static_cast<BitsType>(State.Bits);
	FMemory::Memcpy(&OutValue, &Bits, sizeof(OutValue));
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

template <typename ValueType>
EAvidScriptVmTypedHostStatus WriteSupplementalScalar(void* Context, int64 Self, ValueType Value)
{
	auto& State = *static_cast<FSupplementalScalarTestState*>(Context);
	++State.Calls;
	if (State.bReject || Self != 0x1234567887654321LL) return EAvidScriptVmTypedHostStatus::Rejected;
	using BitsType = std::conditional_t<sizeof(ValueType) == 4, uint32, uint64>;
	BitsType Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Value));
	State.Bits = Bits;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

TArray<uint8> BuildSupplementalScalarFixture(AvidScriptVmResultFixture::EValueKind Kind)
{
	using namespace AvidScriptVmResultFixture;
	const uint8 Type = static_cast<uint8>(Kind);
	TArray<uint8> Module = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	// Imports get(self)->scalar, set(self,scalar)->void; exported wrappers preserve bits.
	AppendSection(Module, 1, {2, 0x60, 1, 0x7e, 1, Type, 0x60, 2, 0x7e, Type, 0});
	TArray<uint8> Imports = {2};
	AppendString(Imports, "avidscript"); AppendString(Imports, "avid_test_scalar_get"); Imports.Append({0, 0});
	AppendString(Imports, "avidscript"); AppendString(Imports, "avid_test_scalar_set"); Imports.Append({0, 1});
	AppendSection(Module, 2, Imports);
	AppendSection(Module, 3, {2, 0, 1});
	TArray<uint8> Exports = {2};
	AppendString(Exports, "read"); Exports.Append({0, 2});
	AppendString(Exports, "write"); Exports.Append({0, 3});
	AppendSection(Module, 7, Exports);
	AppendSection(Module, 10, {2, 6, 0, 0x20, 0, 0x10, 0, 0x0b, 8, 0, 0x20, 0, 0x20, 1, 0x10, 1, 0x0b});
	return Module;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmSupplementalScalarsTest,
	"AvidScript.Architecture.VM.DynamicRawRegistrySupplementalScalars",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmSupplementalScalarsTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptVmResultFixture;
	for (const EValueKind Kind : {EValueKind::I32, EValueKind::I64, EValueKind::F32, EValueKind::F64})
	{
		AddInfo(FString::Printf(TEXT("WAMR supplemental scalar kind=0x%x"), static_cast<uint8>(Kind)));
		FSupplementalScalarTestState State, PeerState;
		TArray<FAvidScriptVmTypedHostImport> Imports;
		Imports.SetNum(2);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			auto& Import = Imports[Index];
			Import.StableId = FString::Printf(TEXT("supplemental-test-%d"), Index);
			Import.ModuleName = TEXT("avidscript");
			Import.ImportName = Index == 0 ? TEXT("avid_test_scalar_get") : TEXT("avid_test_scalar_set");
			Import.bSupplementalRuntimeAuthority = true;
			Import.PreparedTarget.Context = &State;
		}
		bool bWide = false;
		switch (Kind)
		{
		case EValueKind::I32:
			Imports[0].Signature = TEXT("(I)i"); Imports[1].Signature = TEXT("(Ii)");
			Imports[0].Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get; Imports[1].Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Set;
			Imports[0].PreparedTarget.PackedSelfPropertyI32Get = &ReadSupplementalScalar<int32>; Imports[1].PreparedTarget.PackedSelfPropertyI32Set = &WriteSupplementalScalar<int32>; break;
		case EValueKind::I64:
			bWide = true;
			Imports[0].Signature = TEXT("(I)I"); Imports[1].Signature = TEXT("(II)");
			Imports[0].Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Get; Imports[1].Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Set;
			Imports[0].PreparedTarget.PackedSelfPropertyI64Get = &ReadSupplementalScalar<int64>; Imports[1].PreparedTarget.PackedSelfPropertyI64Set = &WriteSupplementalScalar<int64>; break;
		case EValueKind::F32:
			Imports[0].Signature = TEXT("(I)f"); Imports[1].Signature = TEXT("(If)");
			Imports[0].Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Get; Imports[1].Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Set;
			Imports[0].PreparedTarget.PackedSelfPropertyF32Get = &ReadSupplementalScalar<float>; Imports[1].PreparedTarget.PackedSelfPropertyF32Set = &WriteSupplementalScalar<float>; break;
		case EValueKind::F64:
			bWide = true;
			Imports[0].Signature = TEXT("(I)d"); Imports[1].Signature = TEXT("(Id)");
			Imports[0].Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Get; Imports[1].Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Set;
			Imports[0].PreparedTarget.PackedSelfPropertyF64Get = &ReadSupplementalScalar<double>; Imports[1].PreparedTarget.PackedSelfPropertyF64Set = &WriteSupplementalScalar<double>; break;
		default: return false;
		}
		FAvidScriptVmLoadConfig Config;
		Config.TypedHostImports = Imports;
		const auto Wasm = BuildSupplementalScalarFixture(Kind);
		auto Backend = CreateAvidScriptWamrBackend();
		FAvidScriptVmError Error;
		if (!TestTrue(TEXT("scalar imports load"), Backend->Load(Wasm, TEXT("supplemental_scalars"), Config, Error))) { AddError(Error.Details); return false; }
		FAvidScriptVmExportHandle Read, Write;
		if (!Backend->ResolveExport(TEXT("read"), Read, Error) || !Backend->ResolveExport(TEXT("write"), Write, Error)) return false;
		for (uint64 Bits : {0ULL, 0x8000000080000000ULL, 0xFEDCBA98DEADBEEFULL, 0x7ff812347fc12345ULL})
		{
			if (!bWide) Bits = static_cast<uint32>(Bits);
			FAvidScriptVmCallFrame Frame;
			Frame.Cells[0] = 0x87654321; Frame.Cells[1] = 0x12345678;
			Frame.Cells[2] = static_cast<uint32>(Bits); Frame.Cells[3] = static_cast<uint32>(Bits >> 32);
			Frame.CellCount = bWide ? 4 : 3;
			if (!TestTrue(TEXT("scalar setter executes void import"), Backend->Call(Write, Frame, Error))) { AddError(Error.Category + TEXT(": ") + Error.Details); return false; }
			TestEqual(TEXT("setter preserves scalar bit pattern"), State.Bits, Bits);
			Frame.CellCount = 2;
			FAvidScriptVmCallResult Result;
			if (!TestTrue(TEXT("scalar getter executes"), Backend->Call(Read, Frame, Error, &Result))) { AddError(Error.Category + TEXT(": ") + Error.Details); return false; }
			const uint64 Actual = static_cast<uint64>(Result.Cells[0]) | (bWide ? static_cast<uint64>(Result.Cells[1]) << 32 : 0);
			TestEqual(TEXT("getter preserves scalar bit pattern"), Actual, Bits);
		}
		// Global stubs never confer a capability on another VM, nor retain its context.
		auto PeerImports = Imports;
		for (auto& Import : PeerImports) Import.PreparedTarget.Context = &PeerState;
		FAvidScriptVmLoadConfig PeerConfig;
		PeerConfig.TypedHostImports = PeerImports;
		auto Peer = CreateAvidScriptWamrBackend();
		TestTrue(TEXT("peer shares stubs with independent context"), Peer->Load(Wasm, TEXT("supplemental_peer"), PeerConfig, Error));
		FAvidScriptVmExportHandle PeerRead;
		TestTrue(TEXT("peer export resolves"), Peer->ResolveExport(TEXT("read"), PeerRead, Error));
		FAvidScriptVmCallFrame Frame;
		Frame.CellCount = 2; Frame.Cells[0] = 0x87654321; Frame.Cells[1] = 0x12345678;
		State.bReject = true;
		TestFalse(TEXT("getter rejection becomes a VM failure"), Backend->Call(Read, Frame, Error));
		TestEqual(TEXT("rejection category"), Error.Category, FString(TEXT("host_import_failed")));
		Frame.CellCount = bWide ? 4 : 3;
		TestFalse(TEXT("setter rejection becomes a VM failure"), Backend->Call(Write, Frame, Error));
		Backend->Unload();
		Frame.CellCount = 2;
		TestTrue(TEXT("peer survives originating VM unload"), Peer->Call(PeerRead, Frame, Error));
		TestEqual(TEXT("peer has its own target"), PeerState.Calls, 1);
		auto Unauthorized = CreateAvidScriptWamrBackend();
		TestFalse(TEXT("warmed scalar stubs cannot authorize missing imports"), Unauthorized->Load(Wasm, TEXT("supplemental_missing"), FAvidScriptVmLoadConfig(), Error));
		auto WrongImports = Imports;
		WrongImports[1].Signature = TEXT("(I)i");
		FAvidScriptVmLoadConfig WrongConfig;
		WrongConfig.TypedHostImports = WrongImports;
		TestFalse(TEXT("shape and signature mismatch rejected before stub reuse"), Unauthorized->Load(Wasm, TEXT("supplemental_wrong_shape"), WrongConfig, Error));
		TestEqual(TEXT("shape rejection category"), Error.Category, FString(TEXT("supplemental_import_unsupported")));
	}
	return true;
}

#endif
