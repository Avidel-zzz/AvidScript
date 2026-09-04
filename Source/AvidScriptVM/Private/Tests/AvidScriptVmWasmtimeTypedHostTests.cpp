#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"

#include "Misc/AutomationTest.h"

#ifndef AVIDSCRIPT_WITH_WASMTIME
#define AVIDSCRIPT_WITH_WASMTIME 0
#endif

namespace
{
constexpr const TCHAR* TypedImportName = TEXT("avid_s1_1111111111111111");
constexpr const TCHAR* TypedStableId =
	TEXT("1111111111111111111111111111111111111111111111111111111111111111");

void AppendTypedU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendTypedI32Leb(TArray<uint8>& Bytes, int32 Value)
{
	bool bMore = true;
	while (bMore)
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		const bool bSignBitSet = (Byte & 0x40) != 0;
		bMore = !((Value == 0 && !bSignBitSet) || (Value == -1 && bSignBitSet));
		if (bMore)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	}
}

void AppendTypedString(TArray<uint8>& Bytes, const char* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendTypedU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

void AppendTypedSection(TArray<uint8>& Module, uint8 Id, const TArray<uint8>& Payload)
{
	Module.Add(Id);
	AppendTypedU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendTypedI32Const(TArray<uint8>& Bytes, int32 Value)
{
	Bytes.Add(0x41);
	AppendTypedI32Leb(Bytes, Value);
}

void AppendTypedF32Const(TArray<uint8>& Bytes, const float Value)
{
	Bytes.Add(0x43);
	uint32 Bits = 0;
	static_assert(sizeof(Bits) == sizeof(Value));
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	Bytes.Add(static_cast<uint8>(Bits));
	Bytes.Add(static_cast<uint8>(Bits >> 8));
	Bytes.Add(static_cast<uint8>(Bits >> 16));
	Bytes.Add(static_cast<uint8>(Bits >> 24));
}

TArray<uint8> BuildTypedHostFixture(bool bPairImport, int32 Left = 0, int32 Right = 0)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendTypedU32Leb(Types, 2);
	if (bPairImport)
	{
		const uint8 PairType[] = { 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f };
		Types.Append(PairType, UE_ARRAY_COUNT(PairType));
	}
	else
	{
		const uint8 EmptyResultType[] = { 0x60, 0x00, 0x01, 0x7f };
		Types.Append(EmptyResultType, UE_ARRAY_COUNT(EmptyResultType));
	}
	const uint8 ExportType[] = { 0x60, 0x00, 0x01, 0x7f };
	Types.Append(ExportType, UE_ARRAY_COUNT(ExportType));
	AppendTypedSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendTypedU32Leb(Imports, 1);
	AppendTypedString(Imports, "avidscript");
	AppendTypedString(Imports, "avid_s1_1111111111111111");
	Imports.Add(0x00);
	AppendTypedU32Leb(Imports, 0);
	AppendTypedSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendTypedU32Leb(Functions, 1);
	AppendTypedU32Leb(Functions, 1);
	AppendTypedSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendTypedU32Leb(Exports, 1);
	AppendTypedString(Exports, "run");
	Exports.Add(0x00);
	AppendTypedU32Leb(Exports, 1);
	AppendTypedSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	if (bPairImport)
	{
		AppendTypedI32Const(Body, Left);
		AppendTypedI32Const(Body, Right);
	}
	Body.Add(0x10);
	AppendTypedU32Leb(Body, 0);
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendTypedU32Leb(Code, 1);
	AppendTypedU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendTypedSection(Module, 10, Code);
	return Module;
}

TArray<uint8> BuildTypedHostFixture(
	TConstArrayView<int32> Arguments,
	const uint32 ImportCallCount = 1)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendTypedU32Leb(Types, 2);
	Types.Add(0x60);
	AppendTypedU32Leb(Types, static_cast<uint32>(Arguments.Num()));
	for (int32 Index = 0; Index < Arguments.Num(); ++Index)
	{
		Types.Add(0x7f);
	}
	Types.Add(0x01);
	Types.Add(0x7f);
	const uint8 ExportType[] = { 0x60, 0x00, 0x01, 0x7f };
	Types.Append(ExportType, UE_ARRAY_COUNT(ExportType));
	AppendTypedSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendTypedU32Leb(Imports, 1);
	AppendTypedString(Imports, "avidscript");
	AppendTypedString(Imports, "avid_s1_1111111111111111");
	Imports.Add(0x00);
	AppendTypedU32Leb(Imports, 0);
	AppendTypedSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendTypedU32Leb(Functions, 1);
	AppendTypedU32Leb(Functions, 1);
	AppendTypedSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendTypedU32Leb(Exports, 1);
	AppendTypedString(Exports, "run");
	Exports.Add(0x00);
	AppendTypedU32Leb(Exports, 1);
	AppendTypedSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	for (uint32 CallIndex = 0; CallIndex < ImportCallCount; ++CallIndex)
	{
		for (int32 Argument : Arguments)
		{
			AppendTypedI32Const(Body, Argument);
		}
		Body.Add(0x10);
		AppendTypedU32Leb(Body, 0);
		if (CallIndex + 1 < ImportCallCount)
		{
			Body.Add(0x1a);
		}
	}
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendTypedU32Leb(Code, 1);
	AppendTypedU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendTypedSection(Module, 10, Code);
	return Module;
}

TArray<uint8> BuildTypedF32TripleGuestVectorFixture(
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const float X,
	const float Y,
	const float Z,
	const int32 GuestAddress)
{
	TArray<uint8> Module;
	const uint8 Header[] = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00
	};
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendTypedU32Leb(Types, 2);
	const uint8 ImportType[] = {
		0x60, 0x06,
		0x7f, 0x7f, 0x7d, 0x7d, 0x7d, 0x7f,
		0x01, 0x7f
	};
	Types.Append(ImportType, UE_ARRAY_COUNT(ImportType));
	const uint8 ExportType[] = { 0x60, 0x00, 0x01, 0x7f };
	Types.Append(ExportType, UE_ARRAY_COUNT(ExportType));
	AppendTypedSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendTypedU32Leb(Imports, 1);
	AppendTypedString(Imports, "avidscript");
	AppendTypedString(Imports, "avid_s1_1111111111111111");
	Imports.Add(0x00);
	AppendTypedU32Leb(Imports, 0);
	AppendTypedSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendTypedU32Leb(Functions, 1);
	AppendTypedU32Leb(Functions, 1);
	AppendTypedSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendTypedU32Leb(Exports, 1);
	AppendTypedString(Exports, "run");
	Exports.Add(0x00);
	AppendTypedU32Leb(Exports, 1);
	AppendTypedSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	AppendTypedI32Const(Body, SelfSlot);
	AppendTypedI32Const(Body, SelfGeneration);
	AppendTypedF32Const(Body, X);
	AppendTypedF32Const(Body, Y);
	AppendTypedF32Const(Body, Z);
	AppendTypedI32Const(Body, GuestAddress);
	Body.Add(0x10);
	AppendTypedU32Leb(Body, 0);
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendTypedU32Leb(Code, 1);
	AppendTypedU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendTypedSection(Module, 10, Code);
	return Module;
}

FAvidScriptVmBindingPackage MakeTypedBindingPackage(
	const FString& Signature = TEXT("(ii)i"),
	uint32 Ordinal = 1)
{
	check(Ordinal <= 1);
	FAvidScriptVmBindingPackage Package;
	Package.PackageName = TEXT("avidscript.phase54.typed_host");
	Package.PackageHash = FString::ChrN(64, TEXT('1'));
	if (Ordinal == 1)
	{
		FAvidScriptVmDynamicImport Padding;
		Padding.StableId =
			TEXT("2222222222222222222222222222222222222222222222222222222222222222");
		Padding.Ordinal = 0;
		Padding.ModuleName = TEXT("avidscript");
		Padding.ImportName = TEXT("avid_ue_2222222222222222");
		Padding.Signature = TEXT("(i)i");
		Package.Imports.Add(MoveTemp(Padding));
	}
	FAvidScriptVmDynamicImport Import;
	Import.StableId = TypedStableId;
	Import.Ordinal = Ordinal;
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = TypedImportName;
	Import.Signature = Signature;
	Package.Imports.Add(MoveTemp(Import));
	return Package;
}

FAvidScriptVmTypedHostImport MakeTypedImport(
	EAvidScriptVmTypedHostShape Shape = EAvidScriptVmTypedHostShape::I32PairToI32,
	const FString& Signature = TEXT("(ii)i"),
	uint32 Ordinal = 1)
{
	FAvidScriptVmTypedHostImport Import;
	Import.StableId = TypedStableId;
	Import.BindingOrdinal = Ordinal;
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = TypedImportName;
	Import.Signature = Signature;
	Import.Shape = Shape;
	return Import;
}

TUniquePtr<IAvidScriptVmBackend> CreateTypedWasmtimeBackend(FAvidScriptVmError& OutError)
{
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	return CreateAvidScriptVmBackend(Selection, OutError);
}

class FTypedHostDispatcher final : public IAvidScriptVmTypedHostDispatcher
{
public:
	bool ConsumeTypedHostFailure(
		const FString& ExpectedModuleName,
		const FString& ExpectedImportName,
		FAvidScriptVmTypedHostFailure& OutFailure) override
	{
		++FailureConsumeCount;
		LastExpectedModuleName = ExpectedModuleName;
		LastExpectedImportName = ExpectedImportName;
		OutFailure.Reset();
		if (!bFailureAvailable)
		{
			return false;
		}
		bFailureAvailable = false;
		OutFailure.Category = FailureCategory;
		OutFailure.Details = FailureDetails;
		return true;
	}

	EAvidScriptVmTypedHostStatus DispatchEmptyI32(
		uint32 BindingOrdinal,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		OutValue = Bias + 5;
		return Status;
	}

	EAvidScriptVmTypedHostStatus DispatchI32PairToI32(
		uint32 BindingOrdinal,
		int32 Left,
		int32 Right,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		LastLeft = Left;
		LastRight = Right;
		OutValue = Left + Right + Bias;
		return Status;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfI32PairToI32(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		LastSelfSlot = SelfSlot;
		LastSelfGeneration = SelfGeneration;
		LastLeft = Left;
		LastRight = Right;
		++SelfI32PairCalls;
		OutValue = SelfSlot + SelfGeneration + Left + Right + Bias;
		return Status;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfPropertyI32GetSet(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		LastSelfSlot = SelfSlot;
		LastSelfGeneration = SelfGeneration;
		LastGuestAddress = GuestAddress;
		++SelfPropertyCalls;
		OutValue = SelfSlot + SelfGeneration + GuestAddress + Bias;
		return Status;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfVectorValue(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		LastSelfSlot = SelfSlot;
		LastSelfGeneration = SelfGeneration;
		LastGuestAddress = GuestAddress;
		++SelfVectorCalls;
		OutValue = SelfSlot + SelfGeneration + GuestAddress + Bias;
		return Status;
	}

	EAvidScriptVmTypedHostStatus DispatchStableObjectRoundtrip(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 ObjectSlot,
		int32 ObjectGeneration,
		int32 GuestAddress,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		LastSelfSlot = SelfSlot;
		LastSelfGeneration = SelfGeneration;
		LastObjectSlot = ObjectSlot;
		LastObjectGeneration = ObjectGeneration;
		LastGuestAddress = GuestAddress;
		++StableObjectCalls;
		OutValue = SelfSlot + SelfGeneration + ObjectSlot + ObjectGeneration + GuestAddress + Bias;
		return Status;
	}

	EAvidScriptVmTypedHostStatus DispatchCommandBufferSubmit(
		uint32 BindingOrdinal,
		int32 GuestAddress,
		int32 ByteCount,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		LastGuestAddress = GuestAddress;
		LastByteCount = ByteCount;
		++CommandBufferCalls;
		OutValue = GuestAddress + ByteCount + Bias;
		return Status;
	}

	EAvidScriptVmTypedHostStatus Status = EAvidScriptVmTypedHostStatus::Succeeded;
	uint32 LastOrdinal = MAX_uint32;
	int32 LastLeft = 0;
	int32 LastRight = 0;
	int32 LastSelfSlot = 0;
	int32 LastSelfGeneration = 0;
	int32 LastObjectSlot = 0;
	int32 LastObjectGeneration = 0;
	int32 LastGuestAddress = 0;
	int32 LastByteCount = 0;
	int32 SelfI32PairCalls = 0;
	int32 SelfPropertyCalls = 0;
	int32 SelfVectorCalls = 0;
	int32 StableObjectCalls = 0;
	int32 CommandBufferCalls = 0;
	int32 FailureConsumeCount = 0;
	int32 Bias = 0;
	bool bFailureAvailable = false;
	FString FailureCategory;
	FString FailureDetails;
	FString LastExpectedModuleName;
	FString LastExpectedImportName;
};

struct FPreparedSelfI32PairContext
{
	int32 CallCount = 0;
	int32 LastSelfSlot = 0;
	int32 LastSelfGeneration = 0;
	int32 Bias = 0;
};

EAvidScriptVmTypedHostStatus InvokePreparedSelfI32PairForTest(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 Left,
	const int32 Right,
	int32& OutValue)
{
	FPreparedSelfI32PairContext* Prepared =
		static_cast<FPreparedSelfI32PairContext*>(Context);
	if (Prepared == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	++Prepared->CallCount;
	Prepared->LastSelfSlot = SelfSlot;
	Prepared->LastSelfGeneration = SelfGeneration;
	OutValue =
		SelfSlot + SelfGeneration + Left + Right + Prepared->Bias;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

struct FPreparedSelfGuestAddressContext
{
	int32 CallCount = 0;
	int32 LastSelfSlot = 0;
	int32 LastSelfGeneration = 0;
	int32 LastGuestAddress = 0;
	int32 Bias = 0;
};

EAvidScriptVmTypedHostStatus InvokePreparedSelfGuestAddressForTest(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 GuestAddress,
	int32& OutValue)
{
	FPreparedSelfGuestAddressContext* Prepared =
		static_cast<FPreparedSelfGuestAddressContext*>(Context);
	if (Prepared == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	++Prepared->CallCount;
	Prepared->LastSelfSlot = SelfSlot;
	Prepared->LastSelfGeneration = SelfGeneration;
	Prepared->LastGuestAddress = GuestAddress;
	OutValue = SelfSlot + SelfGeneration + GuestAddress + Prepared->Bias;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

struct FPreparedSelfF32TripleGuestVectorContext
{
	int32 CallCount = 0;
	int32 LastSelfSlot = 0;
	int32 LastSelfGeneration = 0;
	float LastX = 0.0f;
	float LastY = 0.0f;
	float LastZ = 0.0f;
	int32 LastGuestAddress = 0;
	int32 Status = 0;
};

EAvidScriptVmTypedHostStatus
InvokePreparedSelfF32TripleGuestVectorForTest(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const float X,
	const float Y,
	const float Z,
	const int32 GuestAddress,
	int32& OutStatus)
{
	FPreparedSelfF32TripleGuestVectorContext* Prepared =
		static_cast<FPreparedSelfF32TripleGuestVectorContext*>(Context);
	if (Prepared == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	++Prepared->CallCount;
	Prepared->LastSelfSlot = SelfSlot;
	Prepared->LastSelfGeneration = SelfGeneration;
	Prepared->LastX = X;
	Prepared->LastY = Y;
	Prepared->LastZ = Z;
	Prepared->LastGuestAddress = GuestAddress;
	OutStatus = Prepared->Status;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

struct FPreparedStableObjectRoundtripContext
{
	int32 CallCount = 0;
	int32 LastObjectSlot = 0;
	int32 LastObjectGeneration = 0;
	int32 Bias = 0;
};

EAvidScriptVmTypedHostStatus InvokePreparedStableObjectRoundtripForTest(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 ObjectSlot,
	const int32 ObjectGeneration,
	const int32 GuestAddress,
	int32& OutValue)
{
	FPreparedStableObjectRoundtripContext* Prepared =
		static_cast<FPreparedStableObjectRoundtripContext*>(Context);
	if (Prepared == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	++Prepared->CallCount;
	Prepared->LastObjectSlot = ObjectSlot;
	Prepared->LastObjectGeneration = ObjectGeneration;
	OutValue = SelfSlot + SelfGeneration + ObjectSlot + ObjectGeneration
		+ GuestAddress + Prepared->Bias;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

struct FPreparedPropertyI32Context
{
	int32 GetCallCount = 0;
	int32 SetCallCount = 0;
	int32 Value = 0;
};

EAvidScriptVmTypedHostStatus InvokePreparedPropertyI32GetForTest(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	int32& OutValue)
{
	FPreparedPropertyI32Context* Prepared =
		static_cast<FPreparedPropertyI32Context*>(Context);
	if (Prepared == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	++Prepared->GetCallCount;
	OutValue = Prepared->Value + SelfSlot + SelfGeneration;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

EAvidScriptVmTypedHostStatus InvokePreparedPropertyI32SetForTest(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 Value)
{
	FPreparedPropertyI32Context* Prepared =
		static_cast<FPreparedPropertyI32Context*>(Context);
	if (Prepared == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	++Prepared->SetCallCount;
	Prepared->Value = Value + SelfSlot + SelfGeneration;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

bool ResolveAndCallTypedRun(
	FAutomationTestBase& Test,
	IAvidScriptVmBackend& Backend,
	int32 Expected,
	FAvidScriptVmError& OutError)
{
	FAvidScriptVmExportHandle Handle;
	if (!Test.TestTrue(TEXT("typed run resolves"), Backend.ResolveExport(TEXT("run"), Handle, OutError)))
	{
		return false;
	}
	FAvidScriptVmCallFrame Frame;
	FAvidScriptVmCallResult Result;
	if (!Test.TestTrue(TEXT("typed run succeeds"), Backend.Call(Handle, Frame, OutError, &Result)))
	{
		return false;
	}
	Test.TestEqual(TEXT("typed run returns one cell"), Result.CellCount, 1u);
	Test.TestEqual(TEXT("typed run result"), static_cast<int32>(Result.Cells[0]), Expected);
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeTypedHostTest,
	"AvidScript.VM.Wasmtime.TypedHost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeTypedHostTest::RunTest(const FString& Parameters)
{
	FPreparedSelfGuestAddressContext PreparedTargetContractContext;
	FAvidScriptVmPreparedTypedHostTarget PreparedTargetContract;
	TestFalse(
		TEXT("empty prepared target reports no callable"),
		PreparedTargetContract.HasAnyTarget());
	PreparedTargetContract.Context = &PreparedTargetContractContext;
	PreparedTargetContract.SelfGuestAddress =
		&InvokePreparedSelfGuestAddressForTest;
	TestTrue(
		TEXT("complete self-address target reports a callable"),
		PreparedTargetContract.HasAnyTarget());
	TestTrue(
		TEXT("combined property binds the self-address target"),
		PreparedTargetContract.IsBoundForShape(
			EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet));
	TestTrue(
		TEXT("vector value binds the self-address target"),
		PreparedTargetContract.IsBoundForShape(
			EAvidScriptVmTypedHostShape::SelfVectorValue));
	TestFalse(
		TEXT("stable object rejects a self-address target"),
		PreparedTargetContract.IsBoundForShape(
			EAvidScriptVmTypedHostShape::StableObjectRoundtrip));
	PreparedTargetContract.Context = nullptr;
	TestFalse(
		TEXT("prepared target without context is incomplete"),
		PreparedTargetContract.IsBoundForShape(
			EAvidScriptVmTypedHostShape::SelfVectorValue));
	FPreparedStableObjectRoundtripContext PreparedStableContractContext;
	FAvidScriptVmPreparedTypedHostTarget PreparedStableTargetContract;
	PreparedStableTargetContract.Context = &PreparedStableContractContext;
	PreparedStableTargetContract.StableObjectRoundtrip =
		&InvokePreparedStableObjectRoundtripForTest;
	TestTrue(
		TEXT("complete stable-object target reports a callable"),
		PreparedStableTargetContract.HasAnyTarget());
	TestTrue(
		TEXT("stable object binds its prepared target"),
		PreparedStableTargetContract.IsBoundForShape(
			EAvidScriptVmTypedHostShape::StableObjectRoundtrip));
	TestFalse(
		TEXT("combined property rejects a stable-object target"),
		PreparedStableTargetContract.IsBoundForShape(
			EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet));
	FPreparedSelfF32TripleGuestVectorContext PreparedVectorContractContext;
	FAvidScriptVmPreparedTypedHostTarget PreparedVectorTargetContract;
	PreparedVectorTargetContract.Context = &PreparedVectorContractContext;
	PreparedVectorTargetContract.SelfF32TripleGuestVector =
		&InvokePreparedSelfF32TripleGuestVectorForTest;
	TestTrue(
		TEXT("semantic FVector binds its expanded-f32 prepared target"),
		PreparedVectorTargetContract.IsBoundForShape(
			EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector));
	TestFalse(
		TEXT("packed FVector rejects the expanded-f32 prepared target"),
		PreparedVectorTargetContract.IsBoundForShape(
			EAvidScriptVmTypedHostShape::SelfVectorValue));

	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateTypedWasmtimeBackend(Error);
#if !AVIDSCRIPT_WITH_WASMTIME
	TestNull(TEXT("Wasmtime typed host is unavailable without the managed dependency"), Backend.Get());
	return true;
#else
	if (!TestNotNull(TEXT("Wasmtime typed backend is created"), Backend.Get()))
	{
		return false;
	}

	const TArray<uint8> PairFixture = BuildTypedHostFixture(true, 17, 19);
	FAvidScriptVmBindingPackage Package = MakeTypedBindingPackage();
	TArray<FAvidScriptVmTypedHostImport> Imports = { MakeTypedImport() };
	FTypedHostDispatcher Dispatcher;
	Dispatcher.Bias = 3;
	FAvidScriptVmLoadConfig Config;
	Config.BindingPackage = &Package;
	Config.TypedHostDispatcher = &Dispatcher;
	Config.TypedHostImports = Imports;
	if (!TestTrue(TEXT("typed pair fixture loads"), Backend->Load(PairFixture, TEXT("typed_pair"), Config, Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	ResolveAndCallTypedRun(*this, *Backend, 39, Error);
	TestEqual(TEXT("typed ordinal is instance-local"), Dispatcher.LastOrdinal, 1u);
	TestEqual(TEXT("typed left is direct"), Dispatcher.LastLeft, 17);
	TestEqual(TEXT("typed right is direct"), Dispatcher.LastRight, 19);

	auto VerifyTypedShape = [this](
		EAvidScriptVmTypedHostShape Shape,
		const FString& Signature,
		TConstArrayView<int32> Arguments,
		int32 Expected,
		int32 FTypedHostDispatcher::* CallCount)
	{
		FAvidScriptVmError ShapeError;
		TUniquePtr<IAvidScriptVmBackend> ShapeBackend = CreateTypedWasmtimeBackend(ShapeError);
		FTypedHostDispatcher ShapeDispatcher;
		ShapeDispatcher.Bias = 3;
		FAvidScriptVmBindingPackage ShapePackage = MakeTypedBindingPackage(Signature);
		TArray<FAvidScriptVmTypedHostImport> ShapeImports = { MakeTypedImport(Shape, Signature) };
		FAvidScriptVmLoadConfig ShapeConfig;
		ShapeConfig.BindingPackage = &ShapePackage;
		ShapeConfig.TypedHostDispatcher = &ShapeDispatcher;
		ShapeConfig.TypedHostImports = ShapeImports;
		if (!TestTrue(TEXT("extended typed shape loads"), ShapeBackend->Load(
			BuildTypedHostFixture(Arguments), TEXT("typed_extended_shape"), ShapeConfig, ShapeError)))
		{
			AddError(ShapeError.Category + TEXT(": ") + ShapeError.Details);
			return;
		}
		ResolveAndCallTypedRun(*this, *ShapeBackend, Expected, ShapeError);
		TestEqual(TEXT("extended typed shape selects its dedicated dispatcher"), ShapeDispatcher.*CallCount, 1);
	};
	const TArray<int32> SelfPairArguments = { 1, 2, 3, 4 };
	const TArray<int32> SelfAddressArguments = { 5, 6, 7 };
	const TArray<int32> StableObjectArguments = { 8, 9, 10, 11, 12 };
	const TArray<int32> CommandArguments = { 13, 14 };
	VerifyTypedShape(
		EAvidScriptVmTypedHostShape::SelfI32PairToI32,
		TEXT("(iiii)i"),
		SelfPairArguments,
		13,
		&FTypedHostDispatcher::SelfI32PairCalls);
	VerifyTypedShape(
		EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet,
		TEXT("(iii)i"),
		SelfAddressArguments,
		21,
		&FTypedHostDispatcher::SelfPropertyCalls);
	VerifyTypedShape(
		EAvidScriptVmTypedHostShape::SelfVectorValue,
		TEXT("(iii)i"),
		SelfAddressArguments,
		21,
		&FTypedHostDispatcher::SelfVectorCalls);
	VerifyTypedShape(
		EAvidScriptVmTypedHostShape::StableObjectRoundtrip,
		TEXT("(iiiii)i"),
		StableObjectArguments,
		53,
		&FTypedHostDispatcher::StableObjectCalls);
	VerifyTypedShape(
		EAvidScriptVmTypedHostShape::CommandBufferSubmit,
		TEXT("(ii)i"),
		CommandArguments,
		30,
		&FTypedHostDispatcher::CommandBufferCalls);

	FAvidScriptVmError PreparedError;
	TUniquePtr<IAvidScriptVmBackend> PreparedBackend =
		CreateTypedWasmtimeBackend(PreparedError);
	FTypedHostDispatcher PreparedFallbackDispatcher;
	PreparedFallbackDispatcher.Bias = 1000;
	FPreparedSelfI32PairContext PreparedContext;
	PreparedContext.Bias = 5;
	FAvidScriptVmBindingPackage PreparedPackage =
		MakeTypedBindingPackage(TEXT("(iiii)i"));
	TArray<FAvidScriptVmTypedHostImport> PreparedImports = {
		MakeTypedImport(
			EAvidScriptVmTypedHostShape::SelfI32PairToI32,
			TEXT("(iiii)i"))
	};
	PreparedImports[0].PreparedTarget.Context = &PreparedContext;
	PreparedImports[0].PreparedTarget.SelfI32Pair =
		&InvokePreparedSelfI32PairForTest;
	FAvidScriptVmLoadConfig PreparedConfig;
	PreparedConfig.BindingPackage = &PreparedPackage;
	PreparedConfig.TypedHostDispatcher = &PreparedFallbackDispatcher;
	PreparedConfig.TypedHostImports = PreparedImports;
	TestTrue(
		TEXT("prepared typed fixture loads"),
		PreparedBackend->Load(
			BuildTypedHostFixture(SelfPairArguments),
			TEXT("typed_prepared"),
			PreparedConfig,
			PreparedError));
	ResolveAndCallTypedRun(
		*this,
		*PreparedBackend,
		15,
		PreparedError);
	TestEqual(
		TEXT("prepared target is called exactly once"),
		PreparedContext.CallCount,
		1);
	TestEqual(
		TEXT("prepared target bypasses the virtual dispatcher"),
		PreparedFallbackDispatcher.SelfI32PairCalls,
		0);

	FAvidScriptVmError PreparedBudgetError;
	TUniquePtr<IAvidScriptVmBackend> PreparedBudgetBackend =
		CreateTypedWasmtimeBackend(PreparedBudgetError);
	FPreparedSelfI32PairContext PreparedBudgetContext;
	PreparedBudgetContext.Bias = 5;
	TArray<FAvidScriptVmTypedHostImport> PreparedBudgetImports =
		PreparedImports;
	PreparedBudgetImports[0].PreparedTarget.Context =
		&PreparedBudgetContext;
	FAvidScriptVmLoadConfig PreparedBudgetConfig = PreparedConfig;
	PreparedBudgetConfig.TypedHostImports = PreparedBudgetImports;
	PreparedBudgetConfig.ExecutionBudget.MaxHostCallsPerEntry = 1;
	TestTrue(
		TEXT("prepared typed budget fixture loads"),
		PreparedBudgetBackend->Load(
			BuildTypedHostFixture(SelfPairArguments, 2),
			TEXT("typed_prepared_budget"),
			PreparedBudgetConfig,
			PreparedBudgetError));
	FAvidScriptVmExportHandle PreparedBudgetHandle;
	TestTrue(
		TEXT("prepared typed budget export resolves"),
		PreparedBudgetBackend->ResolveExport(
			TEXT("run"),
			PreparedBudgetHandle,
			PreparedBudgetError));
	FAvidScriptVmCallFrame PreparedBudgetFrame;
	TestFalse(
		TEXT("prepared typed fast path enforces host-call budget"),
		PreparedBudgetBackend->Call(
			PreparedBudgetHandle,
			PreparedBudgetFrame,
			PreparedBudgetError));
	TestEqual(
		TEXT("prepared typed budget category is stable"),
		PreparedBudgetError.Category,
		FString(TEXT("host_call_budget_exhausted")));
	TestEqual(
		TEXT("prepared target stops at the host-call budget"),
		PreparedBudgetContext.CallCount,
		1);

	FAvidScriptVmError PreparedVectorError;
	TUniquePtr<IAvidScriptVmBackend> PreparedVectorBackend =
		CreateTypedWasmtimeBackend(PreparedVectorError);
	FPreparedSelfF32TripleGuestVectorContext PreparedVectorContext;
	PreparedVectorContext.Status = 73;
	FAvidScriptVmBindingPackage PreparedVectorPackage =
		MakeTypedBindingPackage(TEXT("(iifffi)i"));
	TArray<FAvidScriptVmTypedHostImport> PreparedVectorImports = {
		MakeTypedImport(
			EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector,
			TEXT("(iifffi)i"))
	};
	PreparedVectorImports[0].PreparedTarget.Context =
		&PreparedVectorContext;
	PreparedVectorImports[0].PreparedTarget.SelfF32TripleGuestVector =
		&InvokePreparedSelfF32TripleGuestVectorForTest;
	FAvidScriptVmLoadConfig PreparedVectorConfig;
	PreparedVectorConfig.BindingPackage = &PreparedVectorPackage;
	PreparedVectorConfig.TypedHostDispatcher =
		&PreparedFallbackDispatcher;
	PreparedVectorConfig.TypedHostImports = PreparedVectorImports;
	TestTrue(
		TEXT("expanded-f32 prepared FVector fixture loads"),
		PreparedVectorBackend->Load(
			BuildTypedF32TripleGuestVectorFixture(
				2,
				3,
				1.25f,
				-2.5f,
				4.0f,
				64),
			TEXT("typed_prepared_f32_triple_vector"),
			PreparedVectorConfig,
			PreparedVectorError));
	ResolveAndCallTypedRun(
		*this,
		*PreparedVectorBackend,
		73,
		PreparedVectorError);
	TestEqual(
		TEXT("expanded-f32 target is called exactly once"),
		PreparedVectorContext.CallCount,
		1);
	TestEqual(
		TEXT("expanded-f32 target receives self slot"),
		PreparedVectorContext.LastSelfSlot,
		2);
	TestEqual(
		TEXT("expanded-f32 target receives self generation"),
		PreparedVectorContext.LastSelfGeneration,
		3);
	TestEqual(
		TEXT("expanded-f32 target receives X"),
		PreparedVectorContext.LastX,
		1.25f);
	TestEqual(
		TEXT("expanded-f32 target receives Y"),
		PreparedVectorContext.LastY,
		-2.5f);
	TestEqual(
		TEXT("expanded-f32 target receives Z"),
		PreparedVectorContext.LastZ,
		4.0f);
	TestEqual(
		TEXT("expanded-f32 target receives guest address"),
		PreparedVectorContext.LastGuestAddress,
		64);

	auto VerifyPreparedSelfGuestAddressShape = [this, &SelfAddressArguments](
		const EAvidScriptVmTypedHostShape Shape,
		int32 FTypedHostDispatcher::* DispatcherCallCount)
	{
		FAvidScriptVmError ShapeError;
		TUniquePtr<IAvidScriptVmBackend> ShapeBackend =
			CreateTypedWasmtimeBackend(ShapeError);
		FTypedHostDispatcher ShapeFallbackDispatcher;
		ShapeFallbackDispatcher.Bias = 1000;
		FPreparedSelfGuestAddressContext ShapePreparedContext;
		ShapePreparedContext.Bias = 7;
		FAvidScriptVmBindingPackage ShapePackage =
			MakeTypedBindingPackage(TEXT("(iii)i"));
		TArray<FAvidScriptVmTypedHostImport> ShapeImports = {
			MakeTypedImport(Shape, TEXT("(iii)i"))
		};
		ShapeImports[0].PreparedTarget.Context = &ShapePreparedContext;
		ShapeImports[0].PreparedTarget.SelfGuestAddress =
			&InvokePreparedSelfGuestAddressForTest;
		FAvidScriptVmLoadConfig ShapeConfig;
		ShapeConfig.BindingPackage = &ShapePackage;
		ShapeConfig.TypedHostDispatcher = &ShapeFallbackDispatcher;
		ShapeConfig.TypedHostImports = ShapeImports;
		if (!TestTrue(
			TEXT("prepared self-address fixture loads"),
			ShapeBackend->Load(
				BuildTypedHostFixture(SelfAddressArguments),
				TEXT("typed_prepared_self_address"),
				ShapeConfig,
				ShapeError)))
		{
			AddError(ShapeError.Category + TEXT(": ") + ShapeError.Details);
			return;
		}
		ResolveAndCallTypedRun(*this, *ShapeBackend, 25, ShapeError);
		TestEqual(
			TEXT("prepared self-address target is called exactly once"),
			ShapePreparedContext.CallCount,
			1);
		TestEqual(
			TEXT("prepared self-address target receives self slot"),
			ShapePreparedContext.LastSelfSlot,
			5);
		TestEqual(
			TEXT("prepared self-address target receives self generation"),
			ShapePreparedContext.LastSelfGeneration,
			6);
		TestEqual(
			TEXT("prepared self-address target receives guest address"),
			ShapePreparedContext.LastGuestAddress,
			7);
		TestEqual(
			TEXT("prepared self-address target bypasses the dispatcher"),
			ShapeFallbackDispatcher.*DispatcherCallCount,
			0);
	};
	VerifyPreparedSelfGuestAddressShape(
		EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet,
		&FTypedHostDispatcher::SelfPropertyCalls);
	VerifyPreparedSelfGuestAddressShape(
		EAvidScriptVmTypedHostShape::SelfVectorValue,
		&FTypedHostDispatcher::SelfVectorCalls);

	FAvidScriptVmError PreparedStableError;
	TUniquePtr<IAvidScriptVmBackend> PreparedStableBackend =
		CreateTypedWasmtimeBackend(PreparedStableError);
	FTypedHostDispatcher PreparedStableFallbackDispatcher;
	PreparedStableFallbackDispatcher.Bias = 1000;
	FPreparedStableObjectRoundtripContext PreparedStableContext;
	PreparedStableContext.Bias = 9;
	FAvidScriptVmBindingPackage PreparedStablePackage =
		MakeTypedBindingPackage(TEXT("(iiiii)i"));
	TArray<FAvidScriptVmTypedHostImport> PreparedStableImports = {
		MakeTypedImport(
			EAvidScriptVmTypedHostShape::StableObjectRoundtrip,
			TEXT("(iiiii)i"))
	};
	PreparedStableImports[0].PreparedTarget.Context =
		&PreparedStableContext;
	PreparedStableImports[0].PreparedTarget.StableObjectRoundtrip =
		&InvokePreparedStableObjectRoundtripForTest;
	FAvidScriptVmLoadConfig PreparedStableConfig;
	PreparedStableConfig.BindingPackage = &PreparedStablePackage;
	PreparedStableConfig.TypedHostDispatcher =
		&PreparedStableFallbackDispatcher;
	PreparedStableConfig.TypedHostImports = PreparedStableImports;
	TestTrue(
		TEXT("prepared stable-object fixture loads"),
		PreparedStableBackend->Load(
			BuildTypedHostFixture(StableObjectArguments),
			TEXT("typed_prepared_stable_object"),
			PreparedStableConfig,
			PreparedStableError));
	ResolveAndCallTypedRun(
		*this,
		*PreparedStableBackend,
		59,
		PreparedStableError);
	TestEqual(
		TEXT("prepared stable-object target is called exactly once"),
		PreparedStableContext.CallCount,
		1);
	TestEqual(
		TEXT("prepared stable-object target receives object slot"),
		PreparedStableContext.LastObjectSlot,
		10);
	TestEqual(
		TEXT("prepared stable-object target receives object generation"),
		PreparedStableContext.LastObjectGeneration,
		11);
	TestEqual(
		TEXT("prepared stable-object target bypasses the dispatcher"),
		PreparedStableFallbackDispatcher.StableObjectCalls,
		0);

	FAvidScriptVmError ShapeMismatchError;
	TUniquePtr<IAvidScriptVmBackend> ShapeMismatchBackend =
		CreateTypedWasmtimeBackend(ShapeMismatchError);
	FAvidScriptVmBindingPackage ShapeMismatchPackage =
		MakeTypedBindingPackage(TEXT("(iii)i"));
	TArray<FAvidScriptVmTypedHostImport> ShapeMismatchImports = {
		MakeTypedImport(
			EAvidScriptVmTypedHostShape::SelfVectorValue,
			TEXT("(iii)i"))
	};
	ShapeMismatchImports[0].PreparedTarget.Context = &PreparedStableContext;
	ShapeMismatchImports[0].PreparedTarget.StableObjectRoundtrip =
		&InvokePreparedStableObjectRoundtripForTest;
	FAvidScriptVmLoadConfig ShapeMismatchConfig;
	ShapeMismatchConfig.BindingPackage = &ShapeMismatchPackage;
	ShapeMismatchConfig.TypedHostDispatcher =
		&PreparedStableFallbackDispatcher;
	ShapeMismatchConfig.TypedHostImports = ShapeMismatchImports;
	TestFalse(
		TEXT("prepared target shape mismatch rejects load"),
		ShapeMismatchBackend->Load(
			BuildTypedHostFixture(SelfAddressArguments),
			TEXT("typed_prepared_shape_mismatch"),
			ShapeMismatchConfig,
			ShapeMismatchError));
	TestEqual(
		TEXT("prepared target shape mismatch category"),
		ShapeMismatchError.Category,
		FString(TEXT("typed_host_prepared_target_invalid")));

	FPreparedPropertyI32Context PreparedPropertyContext;
	PreparedPropertyContext.Value = 20;
	FAvidScriptVmError PreparedGetterError;
	TUniquePtr<IAvidScriptVmBackend> PreparedGetterBackend =
		CreateTypedWasmtimeBackend(PreparedGetterError);
	FAvidScriptVmBindingPackage PreparedGetterPackage =
		MakeTypedBindingPackage(TEXT("(ii)i"));
	TArray<FAvidScriptVmTypedHostImport> PreparedGetterImports = {
		MakeTypedImport(
			EAvidScriptVmTypedHostShape::SelfPropertyI32Get,
			TEXT("(ii)i"))
	};
	PreparedGetterImports[0].PreparedTarget.Context =
		&PreparedPropertyContext;
	PreparedGetterImports[0].PreparedTarget.SelfPropertyI32Get =
		&InvokePreparedPropertyI32GetForTest;
	FAvidScriptVmLoadConfig PreparedGetterConfig;
	PreparedGetterConfig.BindingPackage = &PreparedGetterPackage;
	PreparedGetterConfig.TypedHostDispatcher = &PreparedFallbackDispatcher;
	PreparedGetterConfig.TypedHostImports = PreparedGetterImports;
	TestTrue(
		TEXT("split prepared property getter loads"),
		PreparedGetterBackend->Load(
			BuildTypedHostFixture(TArray<int32>{2, 3}),
			TEXT("typed_prepared_property_get"),
			PreparedGetterConfig,
			PreparedGetterError));
	ResolveAndCallTypedRun(
		*this,
		*PreparedGetterBackend,
		25,
		PreparedGetterError);
	TestEqual(
		TEXT("split property getter calls the prepared target once"),
		PreparedPropertyContext.GetCallCount,
		1);

	FAvidScriptVmError PreparedSetterError;
	TUniquePtr<IAvidScriptVmBackend> PreparedSetterBackend =
		CreateTypedWasmtimeBackend(PreparedSetterError);
	FAvidScriptVmBindingPackage PreparedSetterPackage =
		MakeTypedBindingPackage(TEXT("(iii)i"));
	TArray<FAvidScriptVmTypedHostImport> PreparedSetterImports = {
		MakeTypedImport(
			EAvidScriptVmTypedHostShape::SelfPropertyI32Set,
			TEXT("(iii)i"))
	};
	PreparedSetterImports[0].PreparedTarget.Context =
		&PreparedPropertyContext;
	PreparedSetterImports[0].PreparedTarget.SelfPropertyI32Set =
		&InvokePreparedPropertyI32SetForTest;
	FAvidScriptVmLoadConfig PreparedSetterConfig;
	PreparedSetterConfig.BindingPackage = &PreparedSetterPackage;
	PreparedSetterConfig.TypedHostDispatcher = &PreparedFallbackDispatcher;
	PreparedSetterConfig.TypedHostImports = PreparedSetterImports;
	TestTrue(
		TEXT("split prepared property setter loads"),
		PreparedSetterBackend->Load(
			BuildTypedHostFixture(TArray<int32>{2, 3, 17}),
			TEXT("typed_prepared_property_set"),
			PreparedSetterConfig,
			PreparedSetterError));
	ResolveAndCallTypedRun(
		*this,
		*PreparedSetterBackend,
		1,
		PreparedSetterError);
	TestEqual(
		TEXT("split property setter calls the prepared target once"),
		PreparedPropertyContext.SetCallCount,
		1);
	TestEqual(
		TEXT("split property setter forwards the scalar value without guest memory"),
		PreparedPropertyContext.Value,
		22);

	TArray<FAvidScriptVmTypedHostImport> PartialPreparedImports =
		PreparedImports;
	PartialPreparedImports[0].PreparedTarget.SelfI32Pair = nullptr;
	PreparedConfig.TypedHostImports = PartialPreparedImports;
	TestFalse(
		TEXT("partial prepared target rejects load"),
		PreparedBackend->Load(
			BuildTypedHostFixture(SelfPairArguments),
			TEXT("typed_prepared_partial"),
			PreparedConfig,
			PreparedError));
	TestEqual(
		TEXT("partial prepared target category"),
		PreparedError.Category,
		FString(TEXT("typed_host_prepared_target_invalid")));

	Dispatcher.Status = EAvidScriptVmTypedHostStatus::Rejected;
	FAvidScriptVmExportHandle RejectHandle;
	TestTrue(TEXT("reject export resolves"), Backend->ResolveExport(TEXT("run"), RejectHandle, Error));
	FAvidScriptVmCallFrame EmptyFrame;
	TestFalse(TEXT("typed rejection traps"), Backend->Call(RejectHandle, EmptyFrame, Error));
	TestEqual(TEXT("typed rejection category"), Error.Category, FString(TEXT("host_import_failed")));
	TestEqual(TEXT("typed rejection import"), Error.ImportName, FString(TypedImportName));
	TestEqual(TEXT("generic rejection probes the failure channel once"), Dispatcher.FailureConsumeCount, 1);

	Dispatcher.bFailureAvailable = true;
	Dispatcher.FailureCategory = TEXT("binding_write_denied");
	Dispatcher.FailureDetails = TEXT("The reflected property is read-only for this Session.");
	TestFalse(TEXT("structured typed rejection traps"), Backend->Call(RejectHandle, EmptyFrame, Error));
	TestEqual(TEXT("structured typed category crosses the VM"), Error.Category, Dispatcher.FailureCategory);
	TestEqual(TEXT("structured typed details cross the VM"), Error.Details, Dispatcher.FailureDetails);
	TestEqual(TEXT("VM metadata keeps the typed module identity"), Error.ImportModuleName, FString(TEXT("avidscript")));
	TestEqual(TEXT("VM metadata keeps the typed import identity"), Error.ImportName, FString(TypedImportName));
	TestEqual(TEXT("failure consumer receives the expected module"), Dispatcher.LastExpectedModuleName, FString(TEXT("avidscript")));
	TestEqual(TEXT("failure consumer receives the expected import"), Dispatcher.LastExpectedImportName, FString(TypedImportName));

	TestFalse(TEXT("consumed typed failure is not reused"), Backend->Call(RejectHandle, EmptyFrame, Error));
	TestEqual(TEXT("one-shot failure falls back to the generic category"), Error.Category, FString(TEXT("host_import_failed")));
	const int32 FailureConsumeCount = Dispatcher.FailureConsumeCount;
	Dispatcher.Status = EAvidScriptVmTypedHostStatus::Succeeded;
	TestTrue(TEXT("typed success still returns after failures"), Backend->Call(RejectHandle, EmptyFrame, Error));
	TestEqual(TEXT("typed success does not enter the failure channel"), Dispatcher.FailureConsumeCount, FailureConsumeCount);

	FAvidScriptVmLoadConfig MissingDispatcherConfig;
	MissingDispatcherConfig.BindingPackage = &Package;
	MissingDispatcherConfig.TypedHostImports = Imports;
	TestFalse(TEXT("missing typed dispatcher rejects load"), Backend->Load(
		PairFixture,
		TEXT("typed_missing_dispatcher"),
		MissingDispatcherConfig,
		Error));
	TestEqual(TEXT("missing dispatcher category"), Error.Category, FString(TEXT("typed_host_config_invalid")));

	TArray<FAvidScriptVmTypedHostImport> DuplicateImports = { Imports[0], Imports[0] };
	Config.TypedHostImports = DuplicateImports;
	TestFalse(TEXT("duplicate typed identity rejects load"), Backend->Load(
		PairFixture,
		TEXT("typed_duplicate"),
		Config,
		Error));
	TestEqual(TEXT("duplicate category"), Error.Category, FString(TEXT("typed_host_identity_conflict")));

	TArray<FAvidScriptVmTypedHostImport> InvalidImports = {
		MakeTypedImport(EAvidScriptVmTypedHostShape::I32PairToI32, TEXT("(i)i"))
	};
	Config.TypedHostImports = InvalidImports;
	TestFalse(TEXT("wrong typed metadata signature rejects load"), Backend->Load(
		PairFixture,
		TEXT("typed_wrong_metadata"),
		Config,
		Error));
	TestEqual(TEXT("wrong metadata category"), Error.Category, FString(TEXT("typed_host_contract_invalid")));

	TArray<FAvidScriptVmTypedHostImport> InvalidExtendedImports = {
		MakeTypedImport(EAvidScriptVmTypedHostShape::StableObjectRoundtrip, TEXT("(iiii)i"))
	};
	Config.TypedHostImports = InvalidExtendedImports;
	TestFalse(TEXT("wrong extended typed metadata signature rejects load"), Backend->Load(
		BuildTypedHostFixture(StableObjectArguments),
		TEXT("typed_wrong_extended_metadata"),
		Config,
		Error));
	TestEqual(TEXT("wrong extended metadata category"), Error.Category, FString(TEXT("typed_host_contract_invalid")));

	Config.TypedHostImports = Imports;
	const TArray<uint8> WrongArityFixture = BuildTypedHostFixture(false);
	TestFalse(TEXT("wrong actual import arity rejects load"), Backend->Load(
		WrongArityFixture,
		TEXT("typed_wrong_arity"),
		Config,
		Error));
	TestEqual(TEXT("wrong arity reaches Wasmtime type validation"), Error.Category, FString(TEXT("instantiate_failed")));

	FTypedHostDispatcher ExtendedRejectDispatcher;
	ExtendedRejectDispatcher.Status = EAvidScriptVmTypedHostStatus::Rejected;
	FAvidScriptVmBindingPackage ExtendedRejectPackage = MakeTypedBindingPackage(TEXT("(iii)i"));
	TArray<FAvidScriptVmTypedHostImport> ExtendedRejectImports = {
		MakeTypedImport(EAvidScriptVmTypedHostShape::SelfVectorValue, TEXT("(iii)i"))
	};
	FAvidScriptVmLoadConfig ExtendedRejectConfig;
	ExtendedRejectConfig.BindingPackage = &ExtendedRejectPackage;
	ExtendedRejectConfig.TypedHostDispatcher = &ExtendedRejectDispatcher;
	ExtendedRejectConfig.TypedHostImports = ExtendedRejectImports;
	TUniquePtr<IAvidScriptVmBackend> ExtendedRejectBackend = CreateTypedWasmtimeBackend(Error);
	TestTrue(TEXT("extended typed rejection fixture loads"), ExtendedRejectBackend->Load(
		BuildTypedHostFixture(SelfAddressArguments),
		TEXT("typed_extended_reject"),
		ExtendedRejectConfig,
		Error));
	FAvidScriptVmExportHandle ExtendedRejectHandle;
	TestTrue(TEXT("extended reject export resolves"), ExtendedRejectBackend->ResolveExport(TEXT("run"), ExtendedRejectHandle, Error));
	TestFalse(TEXT("extended typed rejection traps"), ExtendedRejectBackend->Call(ExtendedRejectHandle, EmptyFrame, Error));
	TestEqual(TEXT("extended typed rejection category"), Error.Category, FString(TEXT("host_import_failed")));

	FTypedHostDispatcher FirstDispatcher;
	FTypedHostDispatcher SecondDispatcher;
	FirstDispatcher.Bias = 1;
	SecondDispatcher.Bias = 100;
	FAvidScriptVmLoadConfig FirstConfig = Config;
	FAvidScriptVmLoadConfig SecondConfig = Config;
	FirstConfig.TypedHostDispatcher = &FirstDispatcher;
	SecondConfig.TypedHostDispatcher = &SecondDispatcher;
	TUniquePtr<IAvidScriptVmBackend> FirstBackend = CreateTypedWasmtimeBackend(Error);
	TUniquePtr<IAvidScriptVmBackend> SecondBackend = CreateTypedWasmtimeBackend(Error);
	TestTrue(TEXT("first typed instance loads"), FirstBackend->Load(
		PairFixture,
		TEXT("typed_first"),
		FirstConfig,
		Error));
	TestTrue(TEXT("second typed instance loads"), SecondBackend->Load(
		PairFixture,
		TEXT("typed_second"),
		SecondConfig,
		Error));
	ResolveAndCallTypedRun(*this, *FirstBackend, 37, Error);
	ResolveAndCallTypedRun(*this, *SecondBackend, 136, Error);

	FTypedHostDispatcher FirstStableDispatcher;
	FTypedHostDispatcher SecondStableDispatcher;
	FirstStableDispatcher.Bias = 1;
	SecondStableDispatcher.Bias = 100;
	FAvidScriptVmBindingPackage StablePackage = MakeTypedBindingPackage(TEXT("(iiiii)i"));
	TArray<FAvidScriptVmTypedHostImport> StableImports = {
		MakeTypedImport(EAvidScriptVmTypedHostShape::StableObjectRoundtrip, TEXT("(iiiii)i"))
	};
	FAvidScriptVmLoadConfig FirstStableConfig;
	FirstStableConfig.BindingPackage = &StablePackage;
	FirstStableConfig.TypedHostDispatcher = &FirstStableDispatcher;
	FirstStableConfig.TypedHostImports = StableImports;
	FAvidScriptVmLoadConfig SecondStableConfig = FirstStableConfig;
	SecondStableConfig.TypedHostDispatcher = &SecondStableDispatcher;
	TUniquePtr<IAvidScriptVmBackend> FirstStableBackend = CreateTypedWasmtimeBackend(Error);
	TUniquePtr<IAvidScriptVmBackend> SecondStableBackend = CreateTypedWasmtimeBackend(Error);
	TestTrue(TEXT("first stable-object typed instance loads"), FirstStableBackend->Load(
		BuildTypedHostFixture(StableObjectArguments), TEXT("typed_stable_first"), FirstStableConfig, Error));
	TestTrue(TEXT("second stable-object typed instance loads"), SecondStableBackend->Load(
		BuildTypedHostFixture(StableObjectArguments), TEXT("typed_stable_second"), SecondStableConfig, Error));
	ResolveAndCallTypedRun(*this, *FirstStableBackend, 51, Error);
	ResolveAndCallTypedRun(*this, *SecondStableBackend, 150, Error);
	return true;
#endif
}

#endif
