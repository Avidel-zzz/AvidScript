#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"
#include "AvidScriptVmExportTable.h"

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
class FAvidScriptNonBorrowingGuestMemory final : public IAvidScriptVmGuestMemory
{
public:
	bool ReadBytes(uint32 GuestAddress, TArrayView<uint8> OutBytes, FString& OutError) override
	{
		return false;
	}

	bool WriteBytes(uint32 GuestAddress, TConstArrayView<uint8> Bytes, FString& OutError) override
	{
		return false;
	}
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmGuestMemoryBorrowContractTest,
	"AvidScript.Architecture.VM.GuestMemoryBorrowContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmGuestMemoryBorrowContractTest::RunTest(const FString& Parameters)
{
	FAvidScriptNonBorrowingGuestMemory Memory;
	uint8 Sentinel[] = { 1, 2, 3, 4 };
	TConstArrayView<uint8> ReadOnlyView = MakeArrayView(Sentinel);
	TArrayView<uint8> MutableView = MakeArrayView(Sentinel);
	FString Error;

	TestFalse(TEXT("default read-only borrow fails closed"), Memory.BorrowReadOnlyBytes(4, 4, 4, ReadOnlyView, Error));
	TestTrue(TEXT("default read-only borrow clears its view"), ReadOnlyView.IsEmpty());
	TestTrue(TEXT("default read-only borrow reports unavailability"), Error.StartsWith(TEXT("guest_memory_borrow_unavailable:")));

	Error.Reset();
	TestFalse(TEXT("default mutable borrow fails closed"), Memory.BorrowMutableBytes(4, 4, 4, MutableView, Error));
	TestTrue(TEXT("default mutable borrow clears its view"), MutableView.IsEmpty());
	TestTrue(TEXT("default mutable borrow reports unavailability"), Error.StartsWith(TEXT("guest_memory_borrow_unavailable:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmExportCacheTest,
	"AvidScript.Architecture.VM.ExportCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmExportCacheTest::RunTest(const FString& Parameters)
{
	FAvidScriptVmExportTable Table;
	int32 ResolveCount = 0;
	void* const Function = reinterpret_cast<void*>(static_cast<UPTRINT>(0x1234));

	FAvidScriptVmExportHandle FirstHandle;
	TestTrue(TEXT("first resolve succeeds"), Table.ResolveOrCache(TEXT("avid_on_tick"), [&]()
	{
		++ResolveCount;
		return Function;
	}, FirstHandle));

	FAvidScriptVmExportHandle SecondHandle;
	TestTrue(TEXT("second resolve succeeds"), Table.ResolveOrCache(TEXT("avid_on_tick"), [&]()
	{
		++ResolveCount;
		return Function;
	}, SecondHandle));
	TestEqual(TEXT("resolver is called once"), ResolveCount, 1);
	TestEqual(TEXT("cached handle slot is stable"), SecondHandle.Slot, FirstHandle.Slot);
	TestEqual(TEXT("cached handle generation is stable"), SecondHandle.Generation, FirstHandle.Generation);

	void* ResolvedFunction = nullptr;
	FAvidScriptVmError Error;
	TestTrue(TEXT("live handle resolves"), Table.TryGet(FirstHandle, ResolvedFunction, Error));
	TestEqual(TEXT("resolved function is preserved"), ResolvedFunction, Function);

	Table.Reset();
	TestFalse(TEXT("pre-reset handle is stale"), Table.TryGet(FirstHandle, ResolvedFunction, Error));
	TestEqual(TEXT("stale category is structured"), Error.Category, FString(TEXT("stale_export")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmBackendInfoContractTest,
	"AvidScript.Architecture.VM.BackendInfoContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmBackendInfoContractTest::RunTest(const FString& Parameters)
{
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptWamrBackend();
	if (!TestNotNull(TEXT("WAMR compatibility factory returns a backend"), Backend.Get()))
	{
		return false;
	}

	const FAvidScriptVmBackendInfo& Info = Backend->GetBackendInfo();
	TestEqual(TEXT("backend kind is WAMR"), Info.Kind, EAvidScriptVmBackendKind::Wamr);
	TestEqual(
		TEXT("actual execution mode is interpreter"),
		Info.ExecutionMode,
		EAvidScriptVmExecutionMode::Interpreter);
	TestEqual(
		TEXT("artifact format is WASM bytecode"),
		Info.ArtifactFormat,
		EAvidScriptVmArtifactFormat::WasmBytecode);
	TestTrue(
		TEXT("guest memory capability is advertised"),
		EnumHasAnyFlags(Info.Capabilities, EAvidScriptVmCapability::GuestMemory));
	TestTrue(
		TEXT("interpreter capability is advertised"),
		EnumHasAnyFlags(Info.Capabilities, EAvidScriptVmCapability::Interpreter));
	TestTrue(
		TEXT("structured stack capability is advertised"),
		EnumHasAnyFlags(Info.Capabilities, EAvidScriptVmCapability::StructuredStack));
	TestFalse(TEXT("stable backend id is populated"), Info.StableBackendId.IsEmpty());
	TestFalse(TEXT("runtime version is populated"), Info.RuntimeVersion.IsEmpty());
	TestFalse(TEXT("runtime artifact identity is populated"), Info.RuntimeArtifactSha256.IsEmpty());
	TestTrue(
		TEXT("runtime build identity binds interpreter configuration"),
		Info.RuntimeBuildIdentity.Contains(
			TEXT("config=interp=1,fast_interp=1,aot=0,jit=0,fast_jit=0"),
			ESearchCase::CaseSensitive));
	TestTrue(
		TEXT("runtime build identity binds the linked static artifact"),
		Info.RuntimeBuildIdentity.EndsWith(
			TEXT("static_lib_sha256=") + Info.RuntimeArtifactSha256,
			ESearchCase::CaseSensitive));
	TestFalse(TEXT("target triple is populated"), Info.TargetTriple.IsEmpty());

	const uint8 WasmBytes[] = { 0x00, 0x61, 0x73, 0x6d };
	const FAvidScriptVmArtifactView Artifact = FAvidScriptVmArtifactView::FromWasmBytecode(
		MakeArrayView(WasmBytes),
		TEXT("wasm-sha256"));
	TestEqual(
		TEXT("WASM convenience execution bytes"),
		Artifact.ExecutionBytes.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(WasmBytes)));
	TestEqual(
		TEXT("WASM convenience canonical bytes"),
		Artifact.CanonicalWasmBytes.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(WasmBytes)));
	TestEqual(
		TEXT("WASM convenience artifact format"),
		Artifact.ArtifactFormat,
		EAvidScriptVmArtifactFormat::WasmBytecode);
	TestEqual(TEXT("WASM convenience execution identity"), Artifact.ExecutionIdentity, FString(TEXT("wasm-sha256")));
	TestEqual(TEXT("WASM convenience canonical identity"), Artifact.CanonicalWasmIdentity, FString(TEXT("wasm-sha256")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmBackendFactorySelectionTest,
	"AvidScript.Architecture.VM.BackendFactorySelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmBackendFactorySelectionTest::RunTest(const FString& Parameters)
{
	FAvidScriptVmError Error;
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptVmBackend(Selection, Error);
#if AVIDSCRIPT_WITH_WASMTIME
	if (!TestNotNull(TEXT("Wasmtime JIT selection returns a backend"), Backend.Get()))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	TestEqual(TEXT("Wasmtime backend kind"), Backend->GetBackendInfo().Kind, EAvidScriptVmBackendKind::Wasmtime);
	TestEqual(TEXT("Wasmtime execution mode"), Backend->GetBackendInfo().ExecutionMode, EAvidScriptVmExecutionMode::Jit);
	TestEqual(TEXT("Wasmtime artifact format"), Backend->GetBackendInfo().ArtifactFormat, EAvidScriptVmArtifactFormat::WasmBytecode);
	TestFalse(TEXT("Wasmtime factory does not report fallback"), Selection.bAllowFallback);

	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Auto;
	Backend = CreateAvidScriptVmBackend(Selection, Error);
	TestNotNull(TEXT("Wasmtime Auto explicitly selects its JIT"), Backend.Get());
	if (Backend)
	{
		TestEqual(TEXT("Wasmtime Auto reports actual JIT"), Backend->GetBackendInfo().ExecutionMode, EAvidScriptVmExecutionMode::Jit);
	}

	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Interpreter;
	Backend = CreateAvidScriptVmBackend(Selection, Error);
	TestNull(TEXT("Wasmtime interpreter mode is rejected"), Backend.Get());
	TestEqual(TEXT("Wasmtime interpreter category"), Error.Category, FString(TEXT("execution_mode_unavailable")));

	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	Backend = CreateAvidScriptVmBackend(Selection, Error);
	TestNull(TEXT("Wasmtime serialized artifact is not accepted by the JIT core"), Backend.Get());
	TestEqual(TEXT("Wasmtime serialized category"), Error.Category, FString(TEXT("artifact_format_unavailable")));
#else
	TestNull(TEXT("unavailable Wasmtime backend is rejected"), Backend.Get());
	TestEqual(TEXT("Wasmtime error category"), Error.Category, FString(TEXT("backend_unavailable")));
#endif

	Selection.BackendKind = EAvidScriptVmBackendKind::Wamr;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Aot;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WamrAot;
	Backend = CreateAvidScriptVmBackend(Selection, Error);
	TestNull(TEXT("unavailable WAMR AOT mode is rejected"), Backend.Get());
	TestEqual(TEXT("WAMR AOT error category"), Error.Category, FString(TEXT("execution_mode_unavailable")));

	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	Backend = CreateAvidScriptVmBackend(Selection, Error);
	TestNull(TEXT("unavailable WAMR JIT mode is rejected"), Backend.Get());
	TestEqual(TEXT("WAMR JIT error category"), Error.Category, FString(TEXT("execution_mode_unavailable")));

	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Auto;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Selection.bAllowFallback = true;
	Backend = CreateAvidScriptVmBackend(Selection, Error);
	if (!TestNotNull(TEXT("WAMR Auto selection can fall back to interpreter"), Backend.Get()))
	{
		return false;
	}
	TestTrue(TEXT("successful factory clears the error category"), Error.Category.IsEmpty());
	TestEqual(
		TEXT("fallback reports actual interpreter mode"),
		Backend->GetBackendInfo().ExecutionMode,
		EAvidScriptVmExecutionMode::Interpreter);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmAbiSignatureContractTest,
	"AvidScript.Architecture.VM.AbiSignatureContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmAbiSignatureContractTest::RunTest(const FString& Parameters)
{
	FAvidScriptVmAbiSignature Signature;
	FString Error;
	TestTrue(TEXT("all compact WASM value kinds parse"), ParseAvidScriptVmAbiSignature(TEXT("(iIfF)I"), Signature, Error));
	TestEqual(TEXT("four parameters are retained"), Signature.Parameters.Num(), 4);
	if (Signature.Parameters.Num() == 4)
	{
		TestEqual(TEXT("i maps to i32"), Signature.Parameters[0], EAvidScriptVmValueKind::I32);
		TestEqual(TEXT("I maps to i64"), Signature.Parameters[1], EAvidScriptVmValueKind::I64);
		TestEqual(TEXT("f maps to f32"), Signature.Parameters[2], EAvidScriptVmValueKind::F32);
		TestEqual(TEXT("F maps to f64"), Signature.Parameters[3], EAvidScriptVmValueKind::F64);
	}
	TestTrue(TEXT("one result is retained"), Signature.bHasResult);
	TestEqual(TEXT("result maps to i64"), Signature.Result, EAvidScriptVmValueKind::I64);

	TestTrue(TEXT("void result parses"), ParseAvidScriptVmAbiSignature(TEXT("(f)"), Signature, Error));
	TestFalse(TEXT("void result is explicit"), Signature.bHasResult);
	TestEqual(TEXT("void signature retains its parameter"), Signature.Parameters.Num(), 1);

	TestFalse(TEXT("missing open parenthesis is rejected"), ParseAvidScriptVmAbiSignature(TEXT("i)i"), Signature, Error));
	TestFalse(TEXT("multiple results are rejected"), ParseAvidScriptVmAbiSignature(TEXT("(i)ii"), Signature, Error));
	TestFalse(TEXT("unknown value kind is rejected"), ParseAvidScriptVmAbiSignature(TEXT("(v)i"), Signature, Error));
	TestFalse(TEXT("more than 64 parameters are rejected"), ParseAvidScriptVmAbiSignature(
		TEXT("(") + FString::ChrN(65, TEXT('i')) + TEXT(")i"),
		Signature,
		Error));
	TestFalse(TEXT("parse failures retain details"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmBackendInstanceHandleContractTest,
	"AvidScript.Architecture.VM.BackendInstanceHandleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmBackendInstanceHandleContractTest::RunTest(const FString& Parameters)
{
	const uint8 MinimalModule[] = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02,
		0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 0x70, 0x69,
		0x6e, 0x67, 0x00, 0x00, 0x0a, 0x04, 0x01, 0x02,
		0x00, 0x0b
	};

	TUniquePtr<IAvidScriptVmBackend> FirstBackend = CreateAvidScriptWamrBackend();
	TUniquePtr<IAvidScriptVmBackend> SecondBackend = CreateAvidScriptWamrBackend();
	FAvidScriptVmLoadConfig Config;
	FAvidScriptVmError Error;
	if (!TestTrue(
		TEXT("first backend loads"),
		FirstBackend->Load(MakeArrayView(MinimalModule), TEXT("instance_first"), Config, Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	if (!TestTrue(
		TEXT("second backend loads"),
		SecondBackend->Load(MakeArrayView(MinimalModule), TEXT("instance_second"), Config, Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}

	FAvidScriptVmExportHandle FirstHandle;
	FAvidScriptVmExportHandle SecondHandle;
	TestTrue(TEXT("first backend resolves export"), FirstBackend->ResolveExport(TEXT("ping"), FirstHandle, Error));
	TestTrue(TEXT("second backend resolves export"), SecondBackend->ResolveExport(TEXT("ping"), SecondHandle, Error));
	TestNotEqual(TEXT("backend instance identities differ"), FirstHandle.BackendInstanceIdentity, SecondHandle.BackendInstanceIdentity);

	FAvidScriptVmCallFrame EmptyFrame;
	TestFalse(TEXT("second backend rejects first backend handle"), SecondBackend->Call(FirstHandle, EmptyFrame, Error));
	TestEqual(TEXT("foreign handle error category"), Error.Category, FString(TEXT("foreign_export")));

	const uint64 PreviousIdentity = FirstHandle.BackendInstanceIdentity;
	FirstBackend->Unload();
	if (!TestTrue(
		TEXT("first backend reloads"),
		FirstBackend->Load(MakeArrayView(MinimalModule), TEXT("instance_reloaded"), Config, Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	FAvidScriptVmExportHandle ReloadedHandle;
	TestTrue(TEXT("reloaded backend resolves export"), FirstBackend->ResolveExport(TEXT("ping"), ReloadedHandle, Error));
	TestNotEqual(TEXT("reload advances backend instance identity"), ReloadedHandle.BackendInstanceIdentity, PreviousIdentity);
	TestFalse(TEXT("reloaded backend rejects old handle"), FirstBackend->Call(FirstHandle, EmptyFrame, Error));
	TestEqual(TEXT("old handle error category"), Error.Category, FString(TEXT("stale_export")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWamrBackendSmokeTest,
	"AvidScript.Architecture.VM.WamrBackendSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWamrBackendSmokeTest::RunTest(const FString& Parameters)
{
	const uint8 MinimalModule[] = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
		0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01,
		0x05, 0x03, 0x01, 0x00, 0x01, 0x07,
		0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
		0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
		0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
		0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
		0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x07,
		0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
	};

	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptWamrBackend();
	TestNotNull(TEXT("factory returns a backend"), Backend.Get());
	if (!Backend)
	{
		return false;
	}

	FAvidScriptVmError Error;
	FAvidScriptVmLoadConfig Config;
	TestTrue(TEXT("minimal module loads"), Backend->Load(MakeArrayView(MinimalModule), TEXT("vm_smoke"), Config, Error));
	TestTrue(TEXT("backend reports loaded"), Backend->IsLoaded());
	IAvidScriptVmGuestMemory* GuestMemory = Backend->GetGuestMemory();
	TestNotNull(TEXT("WAMR exposes language-neutral guest memory"), GuestMemory);
	if (GuestMemory != nullptr)
	{
		const TArray<uint8> WrittenBytes = { 0x11, 0x22, 0x33, 0x44 };
		TArray<uint8> ReadBytes;
		ReadBytes.SetNumZeroed(WrittenBytes.Num());
		FString MemoryError;
		TestTrue(TEXT("guest memory write succeeds"), GuestMemory->WriteBytes(16, WrittenBytes, MemoryError));
		TestTrue(TEXT("guest memory read succeeds"), GuestMemory->ReadBytes(16, ReadBytes, MemoryError));
		TestEqual(TEXT("guest memory bytes round trip"), ReadBytes, WrittenBytes);
		TestFalse(
			TEXT("guest memory rejects out of bounds read"),
			GuestMemory->ReadBytes(MAX_uint32 - 1, ReadBytes, MemoryError));
		TestFalse(TEXT("out of bounds read reports details"), MemoryError.IsEmpty());
	}

	FAvidScriptVmExportHandle BeginHandle;
	FAvidScriptVmExportHandle TickHandle;
	TestTrue(TEXT("begin export resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	TestTrue(TEXT("tick export resolves"), Backend->ResolveExport(TEXT("avid_on_tick"), TickHandle, Error));
	FAvidScriptVmExportHandle CachedTickHandle;
	TestTrue(TEXT("tick export resolves from cache"), Backend->ResolveExport(TEXT("avid_on_tick"), CachedTickHandle, Error));
	TestEqual(TEXT("two unique WAMR lookups"), Backend->GetExportLookupCount(), 2u);
	TestEqual(TEXT("cached tick slot"), CachedTickHandle.Slot, TickHandle.Slot);

	FAvidScriptVmCallFrame EmptyFrame;
	TestTrue(TEXT("begin export calls"), Backend->Call(BeginHandle, EmptyFrame, Error));
	float DeltaSeconds = 1.0f / 60.0f;
	FAvidScriptVmCallFrame TickFrame;
	TickFrame.CellCount = 1;
	FMemory::Memcpy(&TickFrame.Cells[0], &DeltaSeconds, sizeof(float));
	TestTrue(TEXT("tick export calls"), Backend->Call(TickHandle, TickFrame, Error));

	Backend->Unload();
	TestFalse(TEXT("backend reports unloaded"), Backend->IsLoaded());
	TestFalse(TEXT("old export handle is rejected"), Backend->Call(TickHandle, TickFrame, Error));
	TestEqual(TEXT("old handle category"), Error.Category, FString(TEXT("stale_export")));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedWasmBackendArtifactSmokeTest,
	"AvidScript.Architecture.VM.GeneratedWasmBackendArtifactSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedWasmBackendArtifactSmokeTest::RunTest(const FString& Parameters)
{
	const FString FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/WasmBackend/P41_5_WamrSmoke.wasm")));
	TArray<uint8> Bytecode;
	if (!TestTrue(TEXT("generated backend fixture loads from disk"), FFileHelper::LoadFileToArray(Bytecode, *FixturePath)))
	{
		AddError(FString::Printf(TEXT("Missing generated WASM backend fixture: %s"), *FixturePath));
		return false;
	}

	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptWamrBackend();
	FAvidScriptVmError Error;
	FAvidScriptVmLoadConfig Config;
	if (!TestTrue(TEXT("generated backend artifact loads in UE WAMR"), Backend->Load(
		MakeArrayView(Bytecode),
		TEXT("p41_5_generated_backend_smoke"),
		Config,
		Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}

	FAvidScriptVmExportHandle ValueHandle;
	TestTrue(TEXT("generated guest_value export resolves"), Backend->ResolveExport(
		TEXT("guest_value"),
		ValueHandle,
		Error));
	FAvidScriptVmCallFrame EmptyFrame;
	TestTrue(TEXT("generated guest_value export executes"), Backend->Call(
		ValueHandle,
		EmptyFrame,
		Error));
	Backend->Unload();
	return true;
}
namespace
{
class FAvidScriptVmTestHostDispatcher final : public IAvidScriptHostDispatcher
{
public:
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) override
	{
		++CallCount;
		LastBindingId = Call.BindingId;
		OutResult.bSucceeded = Call.BindingId == EAvidScriptHostBindingId::HostAddI32;
		OutResult.ReturnValue = Call.IntArgs[0] + 1;
		return OutResult.bSucceeded;
	}

	int32 CallCount = 0;
	EAvidScriptHostBindingId LastBindingId = EAvidScriptHostBindingId::Invalid;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWamrHostDispatcherSmokeTest,
	"AvidScript.Architecture.VM.WamrHostDispatcherSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWamrHostDispatcherSmokeTest::RunTest(const FString& Parameters)
{
	const uint8 HostImportModule[] = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x0d, 0x03, 0x60, 0x01, 0x7f, 0x01, 0x7f,
		0x60, 0x00, 0x00, 0x60, 0x01, 0x7d, 0x00,
		0x02, 0x1b, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64,
		0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x0c, 0x68,
		0x6f, 0x73, 0x74, 0x5f, 0x61, 0x64, 0x64, 0x5f,
		0x69, 0x33, 0x32, 0x00, 0x00, 0x03, 0x03, 0x02,
		0x01, 0x02, 0x07, 0x25, 0x02, 0x12, 0x61, 0x76,
		0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65,
		0x67, 0x69, 0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79,
		0x00, 0x01, 0x0c, 0x61, 0x76, 0x69, 0x64, 0x5f,
		0x6f, 0x6e, 0x5f, 0x74, 0x69, 0x63, 0x6b, 0x00,
		0x02, 0x0a, 0x0c, 0x02, 0x07, 0x00, 0x41, 0x29,
		0x10, 0x00, 0x1a, 0x0b, 0x02, 0x00, 0x0b
	};

	FAvidScriptVmTestHostDispatcher Dispatcher;
	FAvidScriptVmLoadConfig Config;
	Config.HostDispatcher = &Dispatcher;
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptWamrBackend();
	TestTrue(TEXT("host import module loads"), Backend->Load(MakeArrayView(HostImportModule), TEXT("vm_host_dispatch"), Config, Error));

	FAvidScriptVmExportHandle BeginHandle;
	TestTrue(TEXT("begin export resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	FAvidScriptVmCallFrame EmptyFrame;
	TestTrue(TEXT("begin export dispatches host call"), Backend->Call(BeginHandle, EmptyFrame, Error));
	TestEqual(TEXT("one host call was dispatched"), Dispatcher.CallCount, 1);
	TestEqual(TEXT("binding id is preserved"), Dispatcher.LastBindingId, EAvidScriptHostBindingId::HostAddI32);
	return true;
}
#endif
