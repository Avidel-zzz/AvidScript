#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"
#include "AvidScriptVmExportTable.h"
#include "AvidScriptVmResultFixtureBuilder.h"
#include "AvidScriptVmStaticHostImports.h"

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
void AppendVmContractU32Leb(TArray<uint8>& Bytes, uint32 Value)
{
	do
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		if (Value != 0)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	} while (Value != 0);
}

void AppendVmContractString(TArray<uint8>& Bytes, const ANSICHAR* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendVmContractU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

void AppendVmContractSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendVmContractU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

TArray<uint8> BuildVmContinuationImportFixture()
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendVmContractU32Leb(Types, 3);
	const uint8 DelayType[] = { 0x60, 0x02, 0x7d, 0x7f, 0x01, 0x7e };
	const uint8 CancelType[] = { 0x60, 0x01, 0x7e, 0x01, 0x7f };
	const uint8 LoadObjectType[] = { 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7e };
	Types.Append(DelayType, UE_ARRAY_COUNT(DelayType));
	Types.Append(CancelType, UE_ARRAY_COUNT(CancelType));
	Types.Append(LoadObjectType, UE_ARRAY_COUNT(LoadObjectType));
	AppendVmContractSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendVmContractU32Leb(Imports, 3);
	AppendVmContractString(Imports, "env");
	AppendVmContractString(Imports, "continuation_delay");
	Imports.Add(0x00);
	AppendVmContractU32Leb(Imports, 0);
	AppendVmContractString(Imports, "env");
	AppendVmContractString(Imports, "continuation_cancel");
	Imports.Add(0x00);
	AppendVmContractU32Leb(Imports, 1);
	AppendVmContractString(Imports, "env");
	AppendVmContractString(Imports, "continuation_load_object");
	Imports.Add(0x00);
	AppendVmContractU32Leb(Imports, 2);
	AppendVmContractSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendVmContractU32Leb(Functions, 3);
	AppendVmContractU32Leb(Functions, 0);
	AppendVmContractU32Leb(Functions, 1);
	AppendVmContractU32Leb(Functions, 2);
	AppendVmContractSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendVmContractU32Leb(Exports, 3);
	AppendVmContractString(Exports, "call_delay");
	Exports.Add(0x00);
	AppendVmContractU32Leb(Exports, 3);
	AppendVmContractString(Exports, "call_cancel");
	Exports.Add(0x00);
	AppendVmContractU32Leb(Exports, 4);
	AppendVmContractString(Exports, "call_load_object");
	Exports.Add(0x00);
	AppendVmContractU32Leb(Exports, 5);
	AppendVmContractSection(Module, 7, Exports);

	const uint8 DelayBody[] = {
		0x00,
		0x20, 0x00,
		0x20, 0x01,
		0x10, 0x00,
		0x0b
	};
	const uint8 CancelBody[] = {
		0x00,
		0x20, 0x00,
		0x10, 0x01,
		0x0b
	};
	const uint8 LoadObjectBody[] = {
		0x00,
		0x20, 0x00,
		0x20, 0x01,
		0x10, 0x02,
		0x0b
	};
	TArray<uint8> Code;
	AppendVmContractU32Leb(Code, 3);
	AppendVmContractU32Leb(Code, UE_ARRAY_COUNT(DelayBody));
	Code.Append(DelayBody, UE_ARRAY_COUNT(DelayBody));
	AppendVmContractU32Leb(Code, UE_ARRAY_COUNT(CancelBody));
	Code.Append(CancelBody, UE_ARRAY_COUNT(CancelBody));
	AppendVmContractU32Leb(Code, UE_ARRAY_COUNT(LoadObjectBody));
	Code.Append(LoadObjectBody, UE_ARRAY_COUNT(LoadObjectBody));
	AppendVmContractSection(Module, 10, Code);
	return Module;
}

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
	TestTrue(
		TEXT("cooperative debug probe capability is advertised"),
		EnumHasAnyFlags(Info.Capabilities, EAvidScriptVmCapability::DebugProbe));
	TestFalse(TEXT("stable backend id is populated"), Info.StableBackendId.IsEmpty());
	TestFalse(TEXT("runtime version is populated"), Info.RuntimeVersion.IsEmpty());
	TestFalse(TEXT("runtime artifact identity is populated"), Info.RuntimeArtifactSha256.IsEmpty());
	TestTrue(
		TEXT("runtime build identity binds interpreter configuration"),
		Info.RuntimeBuildIdentity.Contains(
			TEXT("config=interp=1,fast_interp=1,aot=0,jit=0,fast_jit=0,simd=1,simde=1"),
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
	const uint8 SerializedBytes[] = { 0x7f, 0x45, 0x4c, 0x46 };
	const FAvidScriptVmArtifactView SerializedArtifact =
		FAvidScriptVmArtifactView::FromWasmtimeSerialized(
			MakeArrayView(SerializedBytes),
			MakeArrayView(WasmBytes),
			TEXT("serialized-sha256"),
			TEXT("wasm-sha256"),
			TEXT("compiler-identity"),
			TEXT("x86_64-pc-windows-msvc"),
			EAvidScriptVmArtifactTrust::VerifiedPackage);
	TestEqual(
		TEXT("serialized convenience artifact format"),
		SerializedArtifact.ArtifactFormat,
		EAvidScriptVmArtifactFormat::WasmtimeSerialized);
	TestEqual(
		TEXT("serialized convenience compiler identity"),
		SerializedArtifact.CompilerBuildIdentity,
		FString(TEXT("compiler-identity")));
	TestEqual(
		TEXT("serialized convenience trust"),
		SerializedArtifact.Trust,
		EAvidScriptVmArtifactTrust::VerifiedPackage);
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

	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Aot;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	Backend = CreateAvidScriptVmBackend(Selection, Error);
	TestNotNull(TEXT("Wasmtime precompiled selection returns a backend"), Backend.Get());
	if (Backend)
	{
		TestEqual(
			TEXT("Wasmtime precompiled execution mode"),
			Backend->GetBackendInfo().ExecutionMode,
			EAvidScriptVmExecutionMode::Aot);
		TestEqual(
			TEXT("Wasmtime precompiled artifact format"),
			Backend->GetBackendInfo().ArtifactFormat,
			EAvidScriptVmArtifactFormat::WasmtimeSerialized);
		TestTrue(
			TEXT("Wasmtime advertises precompiled artifacts"),
			EnumHasAnyFlags(
				Backend->GetBackendInfo().Capabilities,
				EAvidScriptVmCapability::PrecompiledArtifact));
	}

	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Backend = CreateAvidScriptVmBackend(Selection, Error);
	TestNull(TEXT("Wasmtime serialized artifact rejects JIT mode"), Backend.Get());
	TestEqual(TEXT("Wasmtime serialized JIT category"), Error.Category, FString(TEXT("execution_mode_unavailable")));
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
	FAvidScriptVmArrayRangeImportContractTest,
	"AvidScript.Architecture.VM.ArrayRangeImportContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmArrayRangeImportContractTest::RunTest(const FString& Parameters)
{
	const FAvidScriptVmStaticHostImport& ReadRange =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ValueArrayReadRange);
	const FAvidScriptVmStaticHostImport& WriteRange =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ValueArrayWriteRange);
	TestEqual(
		TEXT("Array range read uses the shared import name"),
		FString(UTF8_TO_TCHAR(ReadRange.ImportName)),
		FString(TEXT("avid_value_array_read_range")));
	TestEqual(
		TEXT("Array range write uses the shared import name"),
		FString(UTF8_TO_TCHAR(WriteRange.ImportName)),
		FString(TEXT("avid_value_array_write_range")));
	TestEqual(
		TEXT("Array range read carries five i32 parameters"),
		FString(UTF8_TO_TCHAR(ReadRange.Signature)),
		FString(TEXT("(iiiii)i")));
	TestEqual(
		TEXT("Array range write carries five i32 parameters"),
		FString(UTF8_TO_TCHAR(WriteRange.Signature)),
		FString(TEXT("(iiiii)i")));
	TestFalse(
		TEXT("Array range imports are not exposed through the legacy env module"),
		ReadRange.bSupportsEnvCompatibility || WriteRange.bSupportsEnvCompatibility);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmP58StaticImportContractTest,
	"AvidScript.Architecture.VM.P58StaticImportContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmP58StaticImportContractTest::RunTest(const FString& Parameters)
{
	const FAvidScriptVmStaticHostImport& ContinuationStateRead =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationStateRead);
	const FAvidScriptVmStaticHostImport& TextToString =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ValueTextToString);
	const FAvidScriptVmStaticHostImport& DelegateOutputWrite =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::DelegateOutputWrite);
	TestEqual(
		TEXT("P58 imports append after the frozen continuation catalog"),
		static_cast<uint16>(EAvidScriptHostBindingId::ValueTextToString),
		static_cast<uint16>(EAvidScriptHostBindingId::ContinuationStateRead) + 1);
	TestEqual(
		TEXT("Catalog lookup keeps the final frozen continuation import"),
		ContinuationStateRead.BindingId,
		EAvidScriptHostBindingId::ContinuationStateRead);
	TestEqual(
		TEXT("FText conversion uses the shared capability import"),
		FString(UTF8_TO_TCHAR(TextToString.ImportName)),
		FString(TEXT("avid_value_text_to_string")));
	TestEqual(
		TEXT("Delegate output write freezes token, ordinal and address"),
		FString(UTF8_TO_TCHAR(DelegateOutputWrite.Signature)),
		FString(TEXT("(iii)i")));
	TestFalse(
		TEXT("P58 capability imports are not exposed through legacy env"),
		TextToString.bSupportsEnvCompatibility
			|| DelegateOutputWrite.bSupportsEnvCompatibility);
	TestTrue(
		TEXT("Delegate output write is registered only in avidscript"),
		IsAvidScriptVmStaticHostImport(
			TEXT("avidscript"),
			TEXT("avid_delegate_output_write"))
			&& !IsAvidScriptVmStaticHostImport(
				TEXT("env"),
				TEXT("avid_delegate_output_write")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmDebugProbeImportContractTest,
	"AvidScript.Architecture.VM.DebugProbeImportContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmDebugProbeImportContractTest::RunTest(const FString& Parameters)
{
	const FAvidScriptVmStaticHostImport& Probe =
		GetAvidScriptVmStaticHostImport(EAvidScriptHostBindingId::DebugProbe);
	const FAvidScriptVmStaticHostImport& Suspend =
		GetAvidScriptVmStaticHostImport(EAvidScriptHostBindingId::DebugSuspend);
	const FAvidScriptVmStaticHostImport& FrameRead =
		GetAvidScriptVmStaticHostImport(EAvidScriptHostBindingId::DebugFrameRead);
	TestEqual(
		TEXT("Debug probe appends after the frozen P58 catalog"),
		static_cast<uint16>(EAvidScriptHostBindingId::DebugProbe),
		static_cast<uint16>(EAvidScriptHostBindingId::DelegateOutputWrite) + 1);
	TestEqual(
		TEXT("Debug probe uses the compiler ABI import name"),
		FString(UTF8_TO_TCHAR(Probe.ImportName)),
		FString(TEXT("avid_debug_probe")));
	TestEqual(
		TEXT("Debug probe consumes i64 and returns i32 action"),
		FString(UTF8_TO_TCHAR(Probe.Signature)),
		FString(TEXT("(I)i")));
	TestEqual(
		TEXT("Debug suspend follows the probe"),
		static_cast<uint16>(EAvidScriptHostBindingId::DebugSuspend),
		static_cast<uint16>(EAvidScriptHostBindingId::DebugProbe) + 1);
	TestEqual(
		TEXT("Debug suspend freezes probe, route and frame range"),
		FString(UTF8_TO_TCHAR(Suspend.Signature)),
		FString(TEXT("(Iiii)I")));
	TestEqual(
		TEXT("Debug frame read follows suspension"),
		static_cast<uint16>(EAvidScriptHostBindingId::DebugFrameRead),
		static_cast<uint16>(EAvidScriptHostBindingId::DebugSuspend) + 1);
	TestEqual(
		TEXT("Debug frame read freezes token and frame range"),
		FString(UTF8_TO_TCHAR(FrameRead.Signature)),
		FString(TEXT("(Iii)i")));
	TestFalse(
		TEXT("Debug probe is not exposed through legacy env"),
		Probe.bSupportsEnvCompatibility
			|| Suspend.bSupportsEnvCompatibility
			|| FrameRead.bSupportsEnvCompatibility);
	TestTrue(
		TEXT("Debug probe is registered only in avidscript"),
		IsAvidScriptVmStaticHostImport(TEXT("avidscript"), TEXT("avid_debug_probe"))
			&& IsAvidScriptVmStaticHostImport(TEXT("avidscript"), TEXT("avid_debug_suspend"))
			&& IsAvidScriptVmStaticHostImport(TEXT("avidscript"), TEXT("avid_debug_frame_read"))
			&& !IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("avid_debug_probe"))
			&& !IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("avid_debug_suspend"))
			&& !IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("avid_debug_frame_read")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmEventSubscriptionImportContractTest,
	"AvidScript.Architecture.VM.EventSubscriptionImportContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmEventSubscriptionImportContractTest::RunTest(
	const FString& Parameters)
{
	const FAvidScriptVmStaticHostImport& Subscribe =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::EventSubscribe);
	const FAvidScriptVmStaticHostImport& Unsubscribe =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::EventUnsubscribe);
	const FAvidScriptVmStaticHostImport& IsCurrentSource =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::EventIsCurrentSource);
	TestEqual(
		TEXT("Event subscribe uses the generated facade import name"),
		FString(UTF8_TO_TCHAR(Subscribe.ImportName)),
		FString(TEXT("event_subscribe")));
	TestEqual(
		TEXT("Event subscribe returns an opaque i64 token"),
		FString(UTF8_TO_TCHAR(Subscribe.Signature)),
		FString(TEXT("(iii)I")));
	TestEqual(
		TEXT("Event unsubscribe consumes the full i64 token"),
		FString(UTF8_TO_TCHAR(Unsubscribe.Signature)),
		FString(TEXT("(I)i")));
	TestTrue(
		TEXT("C#/LDC env compatibility remains available on both event imports"),
		Subscribe.bSupportsEnvCompatibility
			&& Unsubscribe.bSupportsEnvCompatibility);
	TestEqual(
		TEXT("Sender query appends after the frozen debug imports"),
		static_cast<uint16>(EAvidScriptHostBindingId::EventIsCurrentSource),
		static_cast<uint16>(EAvidScriptHostBindingId::DebugFrameRead) + 1);
	TestEqual(
		TEXT("Sender query uses the shared import name"),
		FString(UTF8_TO_TCHAR(IsCurrentSource.ImportName)),
		FString(TEXT("event_is_current_source")));
	TestEqual(
		TEXT("Sender query accepts slot and generation and returns i32"),
		FString(UTF8_TO_TCHAR(IsCurrentSource.Signature)),
		FString(TEXT("(ii)i")));
	TestTrue(
		TEXT("Sender query is available through generated facade env imports"),
		IsCurrentSource.bSupportsEnvCompatibility
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("event_is_current_source")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmContinuationImportContractTest,
	"AvidScript.Architecture.VM.ContinuationImportContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmContinuationImportContractTest::RunTest(
	const FString& Parameters)
{
	const FAvidScriptVmStaticHostImport& Delay =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationDelay);
	const FAvidScriptVmStaticHostImport& Cancel =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationCancel);
	const FAvidScriptVmStaticHostImport& LoadObject =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationLoadObject);
	const FAvidScriptVmStaticHostImport& CreateSource =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationCancelSourceCreate);
	const FAvidScriptVmStaticHostImport& CancelSource =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationCancelSourceCancel);
	const FAvidScriptVmStaticHostImport& ReleaseSource =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationCancelSourceRelease);
	const FAvidScriptVmStaticHostImport& BindSource =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationBindCancel);
	const FAvidScriptVmStaticHostImport& ResultRead =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationResultRead);
	const FAvidScriptVmStaticHostImport& StateStore =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationStateStore);
	const FAvidScriptVmStaticHostImport& StateRead =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationStateRead);
	TestEqual(
		TEXT("Continuation delay uses the frozen env import name"),
		FString(UTF8_TO_TCHAR(Delay.ImportName)),
		FString(TEXT("continuation_delay")));
	TestEqual(
		TEXT("Continuation delay consumes f32 and i32 and returns i64"),
		FString(UTF8_TO_TCHAR(Delay.Signature)),
		FString(TEXT("(fi)I")));
	TestEqual(
		TEXT("Continuation cancel uses the frozen env import name"),
		FString(UTF8_TO_TCHAR(Cancel.ImportName)),
		FString(TEXT("continuation_cancel")));
	TestEqual(
		TEXT("Continuation cancel consumes i64 and returns i32"),
		FString(UTF8_TO_TCHAR(Cancel.Signature)),
		FString(TEXT("(I)i")));
	TestEqual(
		TEXT("Continuation object load keeps the exact dense binding id"),
		static_cast<uint16>(EAvidScriptHostBindingId::ContinuationLoadObject),
		static_cast<uint16>(31));
	TestEqual(
		TEXT("Continuation object load catalog row keeps its binding id"),
		LoadObject.BindingId,
		EAvidScriptHostBindingId::ContinuationLoadObject);
	TestEqual(
		TEXT("Continuation object load uses the frozen env import name"),
		FString(UTF8_TO_TCHAR(LoadObject.ImportName)),
		FString(TEXT("continuation_load_object")));
	TestEqual(
		TEXT("Continuation object load consumes two i32 values and returns i64"),
		FString(UTF8_TO_TCHAR(LoadObject.Signature)),
		FString(TEXT("(ii)I")));
	TestEqual(
		TEXT("Cancellation source creation returns an opaque i64 token"),
		FString(UTF8_TO_TCHAR(CreateSource.Signature)),
		FString(TEXT("()I")));
	TestEqual(
		TEXT("Cancellation source cancel consumes i64 and returns i32"),
		FString(UTF8_TO_TCHAR(CancelSource.Signature)),
		FString(TEXT("(I)i")));
	TestEqual(
		TEXT("Cancellation source release consumes i64 and returns i32"),
		FString(UTF8_TO_TCHAR(ReleaseSource.Signature)),
		FString(TEXT("(I)i")));
	TestEqual(
		TEXT("Cancellation bind consumes source and continuation i64 tokens"),
		FString(UTF8_TO_TCHAR(BindSource.Signature)),
		FString(TEXT("(II)i")));
	TestEqual(
		TEXT("Continuation result read freezes ordinal, capability and one output range"),
		FString(UTF8_TO_TCHAR(ResultRead.Signature)),
		FString(TEXT("(iiiii)i")));
	TestEqual(
		TEXT("Continuation state store freezes token and one input range"),
		FString(UTF8_TO_TCHAR(StateStore.Signature)),
		FString(TEXT("(Iii)i")));
	TestEqual(
		TEXT("Continuation state read freezes token and one output range"),
		FString(UTF8_TO_TCHAR(StateRead.Signature)),
		FString(TEXT("(Iii)i")));
	TestTrue(
		TEXT("All continuation imports are available through env"),
		Delay.bSupportsEnvCompatibility
			&& Cancel.bSupportsEnvCompatibility
			&& LoadObject.bSupportsEnvCompatibility
			&& CreateSource.bSupportsEnvCompatibility
			&& CancelSource.bSupportsEnvCompatibility
			&& ReleaseSource.bSupportsEnvCompatibility
			&& BindSource.bSupportsEnvCompatibility
			&& ResultRead.bSupportsEnvCompatibility
			&& StateStore.bSupportsEnvCompatibility
			&& StateRead.bSupportsEnvCompatibility
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_delay"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_cancel"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_load_object"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_cancel_source_create"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_cancel_source_cancel"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_cancel_source_release"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_bind_cancel"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_result_read"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_state_store"))
			&& IsAvidScriptVmStaticHostImport(TEXT("env"), TEXT("continuation_state_read")));
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
	TestTrue(
		TEXT("WAMR declares host-call budgeting"),
		EnumHasAnyFlags(
			Backend->GetBackendInfo().Capabilities,
			EAvidScriptVmCapability::HostCallBudget));
	TestFalse(
		TEXT("WAMR does not claim fuel metering"),
		EnumHasAnyFlags(
			Backend->GetBackendInfo().Capabilities,
			EAvidScriptVmCapability::ExecutionFuel));
	FAvidScriptVmLoadConfig UnsupportedBudgetConfig;
	UnsupportedBudgetConfig.ExecutionBudget.FuelPerEntry = 1;
	TestFalse(
		TEXT("WAMR rejects unsupported required execution budget"),
		Backend->Load(
			MakeArrayView(MinimalModule),
			TEXT("vm_budget_unsupported"),
			UnsupportedBudgetConfig,
			Error));
	TestEqual(
		TEXT("WAMR budget rejection category"),
		Error.Category,
		FString(TEXT("execution_budget_unsupported")));
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
	FAvidScriptVmPreparedExportCall PreparedTick;
	TestFalse(
		TEXT("WAMR explicitly declines prepared export calls"),
		Backend->PrepareExportCall(TickHandle, PreparedTick, Error));
	TestFalse(
		TEXT("WAMR leaves no partial prepared target"),
		PreparedTick.IsValid());
	TestEqual(
		TEXT("WAMR prepared fallback category is stable"),
		Error.Category,
		FString(TEXT("prepared_export_unsupported")));

	FAvidScriptVmCallFrame EmptyFrame;
	FAvidScriptVmCallResult VoidResult;
	TestTrue(
		TEXT("begin export calls with result sink"),
		Backend->Call(BeginHandle, EmptyFrame, Error, &VoidResult));
	TestEqual(TEXT("void WAMR export has zero result cells"), VoidResult.CellCount, 0u);
	TestTrue(
		TEXT("legacy WAMR call keeps default null result compatibility"),
		Backend->Call(BeginHandle, EmptyFrame, Error));
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
	FAvidScriptWamrWideResultAbiTest,
	"AvidScript.Architecture.VM.WamrWideResultAbi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWamrWideResultAbiTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptVmResultFixture;

	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptWamrBackend();
	if (!TestNotNull(TEXT("WAMR wide result backend is created"), Backend.Get()))
	{
		return false;
	}
	FAvidScriptVmError Error;
	FAvidScriptVmLoadConfig Config;
	FAvidScriptVmExportHandle Handle;
	FAvidScriptVmCallResult Result;

	const uint64 I64Bits = 0x0123456789abcdefULL;
	const TArray<uint8> I64Fixture =
		BuildSingle(EValueKind::I64, I64Bits);
	if (!TestTrue(
			TEXT("WAMR i64 result fixture loads"),
			Backend->Load(
				MakeArrayView(I64Fixture),
				TEXT("wamr_i64_result"),
				Config,
				Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	TestTrue(
		TEXT("WAMR i64 result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestTrue(
		TEXT("WAMR i64 result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("WAMR i64 result has two cells"), Result.CellCount, 2u);
	TestEqual(
		TEXT("WAMR i64 low cell is first"),
		Result.Cells[0],
		static_cast<uint32>(I64Bits));
	TestEqual(
		TEXT("WAMR i64 high cell is second"),
		Result.Cells[1],
		static_cast<uint32>(I64Bits >> 32));

	const double F64Value = -123.5;
	uint64 F64Bits = 0;
	FMemory::Memcpy(&F64Bits, &F64Value, sizeof(F64Bits));
	const TArray<uint8> F64Fixture =
		BuildSingle(EValueKind::F64, F64Bits);
	if (!TestTrue(
			TEXT("WAMR f64 result fixture loads"),
			Backend->Load(
				MakeArrayView(F64Fixture),
				TEXT("wamr_f64_result"),
				Config,
				Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	TestTrue(
		TEXT("WAMR f64 result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestTrue(
		TEXT("WAMR f64 result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("WAMR f64 result has two cells"), Result.CellCount, 2u);
	TestEqual(
		TEXT("WAMR f64 low bits cell is first"),
		Result.Cells[0],
		static_cast<uint32>(F64Bits));
	TestEqual(
		TEXT("WAMR f64 high bits cell is second"),
		Result.Cells[1],
		static_cast<uint32>(F64Bits >> 32));

	const float F32Value = -3.25f;
	uint32 F32Bits = 0;
	FMemory::Memcpy(&F32Bits, &F32Value, sizeof(F32Bits));
	const TArray<uint8> F32Fixture =
		BuildSingle(EValueKind::F32, F32Bits);
	if (!TestTrue(
			TEXT("WAMR f32 result fixture loads"),
			Backend->Load(
				MakeArrayView(F32Fixture),
				TEXT("wamr_f32_result"),
				Config,
				Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	TestTrue(
		TEXT("WAMR f32 result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestTrue(
		TEXT("WAMR f32 result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("WAMR f32 result has one cell"), Result.CellCount, 1u);
	TestEqual(
		TEXT("WAMR f32 result bits are preserved"),
		Result.Cells[0],
		F32Bits);
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
	FAvidScriptVmCallResult Result;
	TestTrue(TEXT("generated guest_value export executes"), Backend->Call(
		ValueHandle,
		EmptyFrame,
		Error,
		&Result));
	TestEqual(TEXT("generated i32 export has one result cell"), Result.CellCount, 1u);
	TestEqual(TEXT("generated i32 export preserves value"), Result.Cells[0], 7u);
	TestTrue(
		TEXT("generated export keeps legacy null result compatibility"),
		Backend->Call(ValueHandle, EmptyFrame, Error));
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
		if (Call.BindingId == EAvidScriptHostBindingId::DebugProbe)
		{
			LastInt64Argument = Call.Int64Args[0];
			OutResult.bSucceeded = true;
			OutResult.ReturnValue = static_cast<int32>(EAvidScriptDebugProbeAction::Pause);
			return true;
		}
		if (Call.BindingId == EAvidScriptHostBindingId::EventIsCurrentSource)
		{
			LastSlot = Call.IntArgs[0];
			LastGeneration = Call.IntArgs[1];
			OutResult.bSucceeded = true;
			OutResult.ReturnValue = LastSlot == 17 && LastGeneration == 23 ? 1 : 0;
			return true;
		}
		OutResult.bSucceeded = Call.BindingId == EAvidScriptHostBindingId::HostAddI32;
		OutResult.ReturnValue = Call.IntArgs[0] + 1;
		return OutResult.bSucceeded;
	}

	int32 CallCount = 0;
	EAvidScriptHostBindingId LastBindingId = EAvidScriptHostBindingId::Invalid;
	int64 LastInt64Argument = 0;
	int32 LastSlot = 0;
	int32 LastGeneration = 0;
};

class FAvidScriptVmContinuationHostDispatcher final : public IAvidScriptHostDispatcher
{
public:
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) override
	{
		OutResult.bSucceeded = true;
		switch (Call.BindingId)
		{
		case EAvidScriptHostBindingId::ContinuationDelay:
			DelaySeconds = Call.FloatArgs[0];
			CallbackId = Call.IntArgs[0];
			OutResult.ReturnValueI64 = ContinuationToken;
			return true;
		case EAvidScriptHostBindingId::ContinuationCancel:
			CancelledToken = Call.Int64Args[0];
			OutResult.ReturnValue = 1;
			return true;
		case EAvidScriptHostBindingId::ContinuationLoadObject:
			ObjectPathId = Call.IntArgs[0];
			LoadCallbackId = Call.IntArgs[1];
			OutResult.ReturnValueI64 = LoadToken;
			return true;
		default:
			OutResult.bSucceeded = false;
			return false;
		}
	}

	static constexpr int64 ContinuationToken = 0x1122334455667788LL;
	static constexpr int64 LoadToken = 0x123456789abcdef0LL;
	float DelaySeconds = 0.0f;
	int32 CallbackId = 0;
	int64 CancelledToken = 0;
	int32 ObjectPathId = 0;
	int32 LoadCallbackId = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmEventSenderStaticInvocationTest,
	"AvidScript.Architecture.VM.EventSenderStaticInvocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmEventSenderStaticInvocationTest::RunTest(const FString& Parameters)
{
	const FAvidScriptVmStaticHostImport& Import =
		GetAvidScriptVmStaticHostImport(EAvidScriptHostBindingId::EventIsCurrentSource);
	FAvidScriptVmAbiSignature Signature;
	FString FailureDetails;
	if (!TestTrue(TEXT("Sender query catalog signature parses"),
		ParseAvidScriptVmAbiSignature(UTF8_TO_TCHAR(Import.Signature), Signature, FailureDetails)))
	{
		return false;
	}
	FAvidScriptVmStaticValue Arguments[2];
	Arguments[0].I32 = 17;
	Arguments[1].I32 = 23;
	FAvidScriptVmTestHostDispatcher Dispatcher;
	FAvidScriptNonBorrowingGuestMemory GuestMemory;
	FAvidScriptVmStaticCallResult Result;
	TestTrue(TEXT("Sender query dispatches without guest memory access"),
		InvokeAvidScriptVmStaticHostImport(Import, Signature, MakeArrayView(Arguments),
			&Dispatcher, GuestMemory, Result, FailureDetails));
	TestEqual(TEXT("Sender query preserves slot"), Dispatcher.LastSlot, 17);
	TestEqual(TEXT("Sender query preserves generation"), Dispatcher.LastGeneration, 23);
	TestEqual(TEXT("Matching sender returns true"), Result.I32, 1);
	TestEqual(TEXT("Sender query result stays i32"), Result.Kind, EAvidScriptVmValueKind::I32);
	Arguments[1].I32 = 24;
	TestTrue(TEXT("Non-matching sender remains a successful host call"),
		InvokeAvidScriptVmStaticHostImport(Import, Signature, MakeArrayView(Arguments),
			&Dispatcher, GuestMemory, Result, FailureDetails));
	TestEqual(TEXT("Non-matching sender returns false"), Result.I32, 0);
	Arguments[1].Kind = EAvidScriptVmValueKind::I64;
	TestFalse(TEXT("Sender query rejects an incorrect argument type"),
		InvokeAvidScriptVmStaticHostImport(Import, Signature, MakeArrayView(Arguments),
			&Dispatcher, GuestMemory, Result, FailureDetails));
	TestEqual(TEXT("Malformed sender query does not reach the host"), Dispatcher.CallCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmDebugProbeStaticInvocationTest,
	"AvidScript.Architecture.VM.DebugProbeStaticInvocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmDebugProbeStaticInvocationTest::RunTest(const FString& Parameters)
{
	const FAvidScriptVmStaticHostImport& Import =
		GetAvidScriptVmStaticHostImport(EAvidScriptHostBindingId::DebugProbe);
	FAvidScriptVmAbiSignature Signature;
	FString FailureDetails;
	if (!TestTrue(
			TEXT("Debug probe catalog signature parses"),
			ParseAvidScriptVmAbiSignature(
				UTF8_TO_TCHAR(Import.Signature),
				Signature,
				FailureDetails)))
	{
		return false;
	}

	constexpr int64 ProbeId = 0x123456789abcdef0LL;
	FAvidScriptVmStaticValue Argument;
	Argument.Kind = EAvidScriptVmValueKind::I64;
	Argument.I64 = ProbeId;
	FAvidScriptVmTestHostDispatcher Dispatcher;
	FAvidScriptNonBorrowingGuestMemory GuestMemory;
	FAvidScriptVmStaticCallResult Result;
	TestTrue(
		TEXT("Static catalog dispatches debug probe"),
		InvokeAvidScriptVmStaticHostImport(
			Import,
			Signature,
			MakeArrayView(&Argument, 1),
			&Dispatcher,
			GuestMemory,
			Result,
			FailureDetails));
	TestEqual(TEXT("Probe id is forwarded without truncation"), Dispatcher.LastInt64Argument, ProbeId);
	TestEqual(
		TEXT("Probe action is returned to the Guest"),
		Result.I32,
		static_cast<int32>(EAvidScriptDebugProbeAction::Pause));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmContinuationLoadObjectStaticInvocationTest,
	"AvidScript.Architecture.VM.ContinuationLoadObjectStaticInvocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmContinuationLoadObjectStaticInvocationTest::RunTest(
	const FString& Parameters)
{
	const FAvidScriptVmStaticHostImport& Import =
		GetAvidScriptVmStaticHostImport(
			EAvidScriptHostBindingId::ContinuationLoadObject);
	FAvidScriptVmAbiSignature Signature;
	FString FailureDetails;
	if (!TestTrue(
			TEXT("Continuation object load catalog signature parses"),
			ParseAvidScriptVmAbiSignature(
				UTF8_TO_TCHAR(Import.Signature),
				Signature,
				FailureDetails)))
	{
		return false;
	}

	FAvidScriptVmStaticValue Arguments[2];
	Arguments[0].Kind = EAvidScriptVmValueKind::I32;
	Arguments[0].I32 = 73;
	Arguments[1].Kind = EAvidScriptVmValueKind::I32;
	Arguments[1].I32 = 41;
	FAvidScriptVmContinuationHostDispatcher Dispatcher;
	FAvidScriptNonBorrowingGuestMemory GuestMemory;
	FAvidScriptVmStaticCallResult Result;
	TestTrue(
		TEXT("Static catalog invokes continuation object load"),
		InvokeAvidScriptVmStaticHostImport(
			Import,
			Signature,
			MakeArrayView(Arguments),
			&Dispatcher,
			GuestMemory,
			Result,
			FailureDetails));
	TestEqual(TEXT("Static adapter forwards IntArgs[0]"), Dispatcher.ObjectPathId, 73);
	TestEqual(TEXT("Static adapter forwards IntArgs[1]"), Dispatcher.LoadCallbackId, 41);
	TestEqual(TEXT("Static invocation preserves the i64 result kind"), Result.Kind, EAvidScriptVmValueKind::I64);
	TestEqual(
		TEXT("Static invocation returns ReturnValueI64"),
		Result.I64,
		FAvidScriptVmContinuationHostDispatcher::LoadToken);
	return true;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWamrContinuationHostMappingTest,
	"AvidScript.Architecture.VM.WamrContinuationHostMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWamrContinuationHostMappingTest::RunTest(const FString& Parameters)
{
	FAvidScriptVmContinuationHostDispatcher Dispatcher;
	FAvidScriptVmLoadConfig Config;
	Config.HostDispatcher = &Dispatcher;
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptWamrBackend();
	if (!TestNotNull(TEXT("WAMR continuation backend is created"), Backend.Get()))
	{
		return false;
	}

	const TArray<uint8> Fixture = BuildVmContinuationImportFixture();
	if (!TestTrue(
			TEXT("WAMR continuation import fixture loads"),
			Backend->Load(
				MakeArrayView(Fixture),
				TEXT("wamr_continuation_host_mapping"),
				Config,
				Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}

	FAvidScriptVmExportHandle DelayHandle;
	FAvidScriptVmExportHandle CancelHandle;
	FAvidScriptVmExportHandle LoadObjectHandle;
	TestTrue(
		TEXT("Continuation delay export resolves"),
		Backend->ResolveExport(TEXT("call_delay"), DelayHandle, Error));
	TestTrue(
		TEXT("Continuation cancel export resolves"),
		Backend->ResolveExport(TEXT("call_cancel"), CancelHandle, Error));
	TestTrue(
		TEXT("Env-compatible continuation object load export resolves"),
		Backend->ResolveExport(TEXT("call_load_object"), LoadObjectHandle, Error));

	const float DelaySeconds = 0.25f;
	FAvidScriptVmCallFrame DelayFrame;
	DelayFrame.CellCount = 2;
	FMemory::Memcpy(&DelayFrame.Cells[0], &DelaySeconds, sizeof(DelaySeconds));
	DelayFrame.Cells[1] = 37;
	FAvidScriptVmCallResult DelayResult;
	TestTrue(
		TEXT("WAMR continuation delay dispatches"),
		Backend->Call(DelayHandle, DelayFrame, Error, &DelayResult));
	TestEqual(TEXT("WAMR delay maps f32 to FloatArgs[0]"), Dispatcher.DelaySeconds, DelaySeconds);
	TestEqual(TEXT("WAMR delay maps i32 to IntArgs[0]"), Dispatcher.CallbackId, 37);
	TestEqual(TEXT("WAMR delay returns two i64 cells"), DelayResult.CellCount, 2u);
	if (DelayResult.CellCount == 2)
	{
		const uint64 ReturnedToken = static_cast<uint64>(DelayResult.Cells[0])
			| (static_cast<uint64>(DelayResult.Cells[1]) << 32);
		TestEqual(
			TEXT("WAMR delay returns ReturnValueI64"),
			ReturnedToken,
			static_cast<uint64>(FAvidScriptVmContinuationHostDispatcher::ContinuationToken));
	}

	FAvidScriptVmCallFrame CancelFrame;
	CancelFrame.CellCount = 2;
	CancelFrame.Cells[0] = static_cast<uint32>(FAvidScriptVmContinuationHostDispatcher::ContinuationToken);
	CancelFrame.Cells[1] = static_cast<uint32>(
		static_cast<uint64>(FAvidScriptVmContinuationHostDispatcher::ContinuationToken) >> 32);
	FAvidScriptVmCallResult CancelResult;
	TestTrue(
		TEXT("WAMR continuation cancel dispatches"),
		Backend->Call(CancelHandle, CancelFrame, Error, &CancelResult));
	TestEqual(
		TEXT("WAMR cancel maps i64 to Int64Args[0]"),
		Dispatcher.CancelledToken,
		FAvidScriptVmContinuationHostDispatcher::ContinuationToken);
	TestEqual(TEXT("WAMR cancel returns one i32 cell"), CancelResult.CellCount, 1u);
	if (CancelResult.CellCount == 1)
	{
		TestEqual(TEXT("WAMR cancel returns ReturnValue"), CancelResult.Cells[0], 1u);
	}

	FAvidScriptVmCallFrame LoadObjectFrame;
	LoadObjectFrame.CellCount = 2;
	LoadObjectFrame.Cells[0] = 73;
	LoadObjectFrame.Cells[1] = 41;
	FAvidScriptVmCallResult LoadObjectResult;
	TestTrue(
		TEXT("WAMR env continuation object load thunk is callable"),
		Backend->Call(
			LoadObjectHandle,
			LoadObjectFrame,
			Error,
			&LoadObjectResult));
	TestEqual(TEXT("WAMR load maps first i32 to IntArgs[0]"), Dispatcher.ObjectPathId, 73);
	TestEqual(TEXT("WAMR load maps second i32 to IntArgs[1]"), Dispatcher.LoadCallbackId, 41);
	TestEqual(TEXT("WAMR load returns two i64 cells"), LoadObjectResult.CellCount, 2u);
	if (LoadObjectResult.CellCount == 2)
	{
		const uint64 ReturnedToken = static_cast<uint64>(LoadObjectResult.Cells[0])
			| (static_cast<uint64>(LoadObjectResult.Cells[1]) << 32);
		TestEqual(
			TEXT("WAMR load returns ReturnValueI64"),
			ReturnedToken,
			static_cast<uint64>(FAvidScriptVmContinuationHostDispatcher::LoadToken));
	}
	return true;
}
#endif
