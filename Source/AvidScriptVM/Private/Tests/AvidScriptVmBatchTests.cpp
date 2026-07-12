#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"

#include "Misc/AutomationTest.h"

namespace
{
enum class EAvidScriptVmBatchFixtureCase : uint8
{
	Success,
	Empty,
	ExcessiveCount,
	UnalignedInput,
	InvalidOutput
};

void AppendVmBatchU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendVmBatchI32Leb(TArray<uint8>& Bytes, int32 Value)
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

void AppendVmBatchString(TArray<uint8>& Bytes, const char* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendVmBatchU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

void AppendVmBatchSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendVmBatchU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendVmBatchI32Const(TArray<uint8>& Body, int32 Value)
{
	Body.Add(0x41);
	AppendVmBatchI32Leb(Body, Value);
}

TArray<uint8> BuildVmTransformBatchFixture(EAvidScriptVmBatchFixtureCase FixtureCase)
{
	constexpr int32 InputAddress = 64;
	constexpr int32 OutputAddress = 128;

	int32 BatchInputAddress = InputAddress;
	int32 BatchOutputAddress = OutputAddress;
	int32 BatchCount = 2;
	if (FixtureCase == EAvidScriptVmBatchFixtureCase::Empty)
	{
		BatchInputAddress = 0;
		BatchOutputAddress = 0;
		BatchCount = 0;
	}
	else if (FixtureCase == EAvidScriptVmBatchFixtureCase::ExcessiveCount)
	{
		BatchCount = 257;
	}
	else if (FixtureCase == EAvidScriptVmBatchFixtureCase::UnalignedInput)
	{
		BatchInputAddress = InputAddress + 1;
	}
	else if (FixtureCase == EAvidScriptVmBatchFixtureCase::InvalidOutput)
	{
		BatchOutputAddress = MAX_int32 - 3;
	}

	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendVmBatchU32Leb(Types, 4);
	Types.Add(0x60);
	AppendVmBatchU32Leb(Types, 3);
	Types.Add(0x7f);
	Types.Add(0x7f);
	Types.Add(0x7f);
	AppendVmBatchU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendVmBatchU32Leb(Types, 1);
	Types.Add(0x7f);
	AppendVmBatchU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendVmBatchU32Leb(Types, 0);
	AppendVmBatchU32Leb(Types, 0);
	Types.Add(0x60);
	AppendVmBatchU32Leb(Types, 1);
	Types.Add(0x7d);
	AppendVmBatchU32Leb(Types, 0);
	AppendVmBatchSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendVmBatchU32Leb(Imports, 2);
	AppendVmBatchString(Imports, "avidscript");
	AppendVmBatchString(Imports, "actor_get_transform_batch");
	Imports.Add(0x00);
	AppendVmBatchU32Leb(Imports, 0);
	AppendVmBatchString(Imports, "avidscript");
	AppendVmBatchString(Imports, "host_add_i32");
	Imports.Add(0x00);
	AppendVmBatchU32Leb(Imports, 1);
	AppendVmBatchSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendVmBatchU32Leb(Functions, 2);
	AppendVmBatchU32Leb(Functions, 2);
	AppendVmBatchU32Leb(Functions, 3);
	AppendVmBatchSection(Module, 3, Functions);

	TArray<uint8> Memory;
	AppendVmBatchU32Leb(Memory, 1);
	Memory.Add(0x00);
	AppendVmBatchU32Leb(Memory, 1);
	AppendVmBatchSection(Module, 5, Memory);

	TArray<uint8> Exports;
	AppendVmBatchU32Leb(Exports, 2);
	AppendVmBatchString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendVmBatchU32Leb(Exports, 2);
	AppendVmBatchString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	AppendVmBatchU32Leb(Exports, 3);
	AppendVmBatchSection(Module, 7, Exports);

	TArray<uint8> BeginBody;
	AppendVmBatchU32Leb(BeginBody, 0);
	AppendVmBatchI32Const(BeginBody, BatchInputAddress);
	AppendVmBatchI32Const(BeginBody, BatchCount);
	AppendVmBatchI32Const(BeginBody, BatchOutputAddress);
	BeginBody.Add(0x10);
	AppendVmBatchU32Leb(BeginBody, 0);
	BeginBody.Add(0x1a);
	if (FixtureCase == EAvidScriptVmBatchFixtureCase::Success)
	{
		AppendVmBatchI32Const(BeginBody, OutputAddress);
		BeginBody.Add(0x28);
		AppendVmBatchU32Leb(BeginBody, 2);
		AppendVmBatchU32Leb(BeginBody, 0);
		BeginBody.Add(0x10);
		AppendVmBatchU32Leb(BeginBody, 1);
		BeginBody.Add(0x1a);
	}
	BeginBody.Add(0x0b);

	TArray<uint8> TickBody;
	AppendVmBatchU32Leb(TickBody, 0);
	TickBody.Add(0x0b);

	TArray<uint8> Code;
	AppendVmBatchU32Leb(Code, 2);
	AppendVmBatchU32Leb(Code, static_cast<uint32>(BeginBody.Num()));
	Code.Append(BeginBody);
	AppendVmBatchU32Leb(Code, static_cast<uint32>(TickBody.Num()));
	Code.Append(TickBody);
	AppendVmBatchSection(Module, 10, Code);

	const uint32 InputCells[] = { 7u, 11u, 13u, 17u };
	TArray<uint8> Data;
	AppendVmBatchU32Leb(Data, 1);
	Data.Add(0x00);
	AppendVmBatchI32Const(Data, InputAddress);
	Data.Add(0x0b);
	AppendVmBatchU32Leb(Data, static_cast<uint32>(sizeof(InputCells)));
	Data.Append(reinterpret_cast<const uint8*>(InputCells), UE_ARRAY_COUNT(InputCells) * sizeof(uint32));
	AppendVmBatchSection(Module, 11, Data);
	return Module;
}

class FAvidScriptVmBatchDispatcher final : public IAvidScriptHostDispatcher
{
public:
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) override
	{
		++CallCount;
		if (Call.BindingId == EAvidScriptHostBindingId::ActorGetTransformBatch)
		{
			++BatchCallCount;
			CapturedInputCells.Reset();
			if (!Call.InputCells.IsEmpty())
			{
				CapturedInputCells.Append(Call.InputCells.GetData(), Call.InputCells.Num());
			}
			CapturedOutputFloatCount = Call.OutputFloats.Num();
			if (!Call.OutputFloats.IsEmpty())
			{
				Call.OutputFloats[0] = 42.0f;
			}
			OutResult.bSucceeded = true;
			OutResult.ReturnValue = Call.IntArgs[0];
			return true;
		}
		if (Call.BindingId == EAvidScriptHostBindingId::HostAddI32)
		{
			ObservedOutputBits = Call.IntArgs[0];
			OutResult.bSucceeded = true;
			OutResult.ReturnValue = Call.IntArgs[0] + 1;
			return true;
		}
		OutResult.Details = TEXT("Unexpected VM batch test binding.");
		return false;
	}

	int32 CallCount = 0;
	int32 BatchCallCount = 0;
	int32 CapturedOutputFloatCount = -1;
	int32 ObservedOutputBits = 0;
	TArray<uint32> CapturedInputCells;
};

bool LoadAndResolveVmBatchFixture(
	FAutomationTestBase& Test,
	EAvidScriptVmBatchFixtureCase FixtureCase,
	FAvidScriptVmBatchDispatcher& Dispatcher,
	TUniquePtr<IAvidScriptVmBackend>& OutBackend,
	FAvidScriptVmExportHandle& OutBeginHandle,
	FAvidScriptVmError& OutError)
{
	FAvidScriptVmLoadConfig Config;
	Config.HostDispatcher = &Dispatcher;
	OutBackend = CreateAvidScriptWamrBackend();
	const TArray<uint8> WasmBytes = BuildVmTransformBatchFixture(FixtureCase);
	if (!Test.TestTrue(TEXT("batch fixture loads"), OutBackend->Load(WasmBytes, TEXT("vm_transform_batch"), Config, OutError)))
	{
		Test.AddError(OutError.Details);
		return false;
	}
	return Test.TestTrue(
		TEXT("batch begin export resolves"),
		OutBackend->ResolveExport(TEXT("avid_on_begin_play"), OutBeginHandle, OutError));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWamrTransformBatchMemoryTest,
	"AvidScript.Architecture.VM.WamrTransformBatchMemory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWamrTransformBatchMemoryTest::RunTest(const FString& Parameters)
{
	FAvidScriptVmBatchDispatcher Dispatcher;
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend;
	FAvidScriptVmExportHandle BeginHandle;
	if (!LoadAndResolveVmBatchFixture(*this, EAvidScriptVmBatchFixtureCase::Success, Dispatcher, Backend, BeginHandle, Error))
	{
		return false;
	}

	FAvidScriptVmCallFrame EmptyFrame;
	TestTrue(TEXT("batch import completes"), Backend->Call(BeginHandle, EmptyFrame, Error));
	TestEqual(TEXT("one batch call reaches dispatcher"), Dispatcher.BatchCallCount, 1);
	const TArray<uint32> ExpectedInputCells = { 7u, 11u, 13u, 17u };
	TestTrue(TEXT("two handles are exposed as four cells"), Dispatcher.CapturedInputCells == ExpectedInputCells);
	TestEqual(TEXT("two transform outputs expose eighteen floats"), Dispatcher.CapturedOutputFloatCount, 18);
	uint32 ExpectedOutputBits = 0;
	const float ExpectedOutput = 42.0f;
	FMemory::Memcpy(&ExpectedOutputBits, &ExpectedOutput, sizeof(float));
	TestEqual(TEXT("guest observes the host memory write"), Dispatcher.ObservedOutputBits, static_cast<int32>(ExpectedOutputBits));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWamrTransformBatchValidationTest,
	"AvidScript.Architecture.VM.WamrTransformBatchValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWamrTransformBatchValidationTest::RunTest(const FString& Parameters)
{
	const EAvidScriptVmBatchFixtureCase InvalidCases[] = {
		EAvidScriptVmBatchFixtureCase::ExcessiveCount,
		EAvidScriptVmBatchFixtureCase::UnalignedInput,
		EAvidScriptVmBatchFixtureCase::InvalidOutput
	};
	for (const EAvidScriptVmBatchFixtureCase FixtureCase : InvalidCases)
	{
		FAvidScriptVmBatchDispatcher Dispatcher;
		FAvidScriptVmError Error;
		TUniquePtr<IAvidScriptVmBackend> Backend;
		FAvidScriptVmExportHandle BeginHandle;
		if (!LoadAndResolveVmBatchFixture(*this, FixtureCase, Dispatcher, Backend, BeginHandle, Error))
		{
			return false;
		}
		FAvidScriptVmCallFrame EmptyFrame;
		TestFalse(TEXT("invalid batch arguments fail before dispatch"), Backend->Call(BeginHandle, EmptyFrame, Error));
		TestEqual(TEXT("invalid batch failure is structured"), Error.Category, FString(TEXT("host_import_failed")));
		TestEqual(TEXT("invalid batch identifies the import"), Error.ImportName, FString(TEXT("actor_get_transform_batch")));
		TestEqual(TEXT("invalid batch never reaches dispatcher"), Dispatcher.CallCount, 0);
	}

	FAvidScriptVmBatchDispatcher EmptyDispatcher;
	FAvidScriptVmError EmptyError;
	TUniquePtr<IAvidScriptVmBackend> EmptyBackend;
	FAvidScriptVmExportHandle EmptyBeginHandle;
	if (!LoadAndResolveVmBatchFixture(*this, EAvidScriptVmBatchFixtureCase::Empty, EmptyDispatcher, EmptyBackend, EmptyBeginHandle, EmptyError))
	{
		return false;
	}
	FAvidScriptVmCallFrame EmptyFrame;
	TestTrue(TEXT("empty batch allows null addresses"), EmptyBackend->Call(EmptyBeginHandle, EmptyFrame, EmptyError));
	TestEqual(TEXT("empty batch reaches dispatcher once"), EmptyDispatcher.BatchCallCount, 1);
	TestTrue(TEXT("empty input view has no cells"), EmptyDispatcher.CapturedInputCells.IsEmpty());
	TestEqual(TEXT("empty output view has no floats"), EmptyDispatcher.CapturedOutputFloatCount, 0);
	return true;
}

#endif