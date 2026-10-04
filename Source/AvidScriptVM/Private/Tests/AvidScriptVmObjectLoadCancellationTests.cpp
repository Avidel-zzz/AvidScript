#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptContinuationCancellationAbi.h"
#include "AvidScriptVmBackend.h"
#include "AvidScriptVmStaticHostImports.h"
#include "Misc/AutomationTest.h"

namespace AvidScriptObjectLoadCancellationTests
{
void AppendString(TArray<uint8>& Bytes, const ANSICHAR* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	check(Length < 128);
	Bytes.Add(static_cast<uint8>(Length));
	Bytes.Append(reinterpret_cast<const uint8*>(Value), Length);
}

void AppendSection(TArray<uint8>& Bytes, uint8 Id, const TArray<uint8>& Payload)
{
	check(Payload.Num() < 128);
	Bytes.Add(Id);
	Bytes.Add(static_cast<uint8>(Payload.Num()));
	Bytes.Append(Payload);
}

// ABI-only fixture: schedule(i32, i32)->i64 forwards both cells to the real VM import.
TArray<uint8> BuildFixture(const ANSICHAR* Module, const ANSICHAR* Name, bool bWrongType = false)
{
	TArray<uint8> Bytes = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	AppendSection(Bytes, 1, {1, 0x60, 2, 0x7f, 0x7f, 1, static_cast<uint8>(bWrongType ? 0x7f : 0x7e)});
	TArray<uint8> Imports = {1};
	AppendString(Imports, Module);
	AppendString(Imports, Name);
	Imports.Append({0, 0});
	AppendSection(Bytes, 2, Imports);
	AppendSection(Bytes, 3, {1, 0});
	TArray<uint8> Exports = {1};
	AppendString(Exports, "schedule");
	Exports.Append({0, 1});
	AppendSection(Bytes, 7, Exports);
	AppendSection(Bytes, 10, {1, 8, 0, 0x20, 0, 0x20, 1, 0x10, 0, 0x0b});
	return Bytes;
}

class FDispatcher final : public IAvidScriptHostDispatcher
{
public:
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& Result) override
	{
		++Calls;
		LastBinding = Call.BindingId;
		Path = Call.IntArgs[0];
		Callback = Call.IntArgs[1];
		Result.bSucceeded = Call.BindingId == EAvidScriptHostBindingId::ContinuationLoadObjectCancelResumeV1;
		Result.ReturnValueI64 = Reply;
		return Result.bSucceeded;
	}
	int64 Reply = 0;
	int32 Calls = 0;
	int32 Path = 0;
	int32 Callback = 0;
	EAvidScriptHostBindingId LastBinding = EAvidScriptHostBindingId::Invalid;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptVmObjectLoadCancelResumeTest,
	"AvidScript.Architecture.VM.ObjectLoadCancelResume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmObjectLoadCancelResumeTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptObjectLoadCancellationTests;
	using namespace AvidScript::ContinuationCancellation;
	TestTrue(TEXT("Object cancel-resume import is admitted"),
		IsAvidScriptVmStaticHostImport(UTF8_TO_TCHAR(Abi::Module), UTF8_TO_TCHAR(Abi::ObjectLoadCancelResumeImport)));
	TestFalse(TEXT("Object cancel-resume import has no env alias"),
		IsAvidScriptVmStaticHostImport(TEXT("env"), UTF8_TO_TCHAR(Abi::ObjectLoadCancelResumeImport)));
	const auto Fixture = BuildFixture(Abi::Module, Abi::ObjectLoadCancelResumeImport);
	for (int32 BackendIndex = 0; BackendIndex != 2; ++BackendIndex)
	{
		const auto CreateBackend = [BackendIndex]()
		{
			return BackendIndex == 0 ? CreateAvidScriptWasmtimeBackend() : CreateAvidScriptWamrBackend();
		};
		FDispatcher Dispatcher;
		FAvidScriptVmLoadConfig Config;
		Config.HostDispatcher = &Dispatcher;
		FAvidScriptVmError Error;
		auto Backend = CreateBackend();
		if (!TestNotNull(TEXT("Object cancel-resume backend exists"), Backend.Get())
			|| !TestTrue(TEXT("Object cancel-resume fixture loads"), Backend->Load(Fixture, TEXT("object_cancel_resume"), Config, Error)))
		{
			AddError(Error.Category + TEXT(": ") + Error.Details);
			return false;
		}
		FAvidScriptVmExportHandle Export;
		if (!TestTrue(TEXT("Schedule export resolves"), Backend->ResolveExport(TEXT("schedule"), Export, Error))) return false;
		for (const uint64 Reply : {0ULL, 0x80000001fedcba98ULL})
		{
			Dispatcher.Reply = static_cast<int64>(Reply);
			FAvidScriptVmCallFrame Frame;
			Frame.CellCount = 2;
			Frame.Cells[0] = 0x80000073u;
			Frame.Cells[1] = 41;
			FAvidScriptVmCallResult Result;
			if (!TestTrue(TEXT("Schedule calls the opt-in object import"), Backend->Call(Export, Frame, Error, &Result)))
			{
				AddError(Error.Category + TEXT(": ") + Error.Details);
				return false;
			}
			TestEqual(TEXT("VM forwards full path value bits"), static_cast<uint32>(Dispatcher.Path), Frame.Cells[0]);
			TestEqual(TEXT("VM forwards callback cell"), Dispatcher.Callback, 41);
			TestEqual(TEXT("VM preserves opt-in binding identity"), Dispatcher.LastBinding, EAvidScriptHostBindingId::ContinuationLoadObjectCancelResumeV1);
			TestEqual(TEXT("Schedule returns two i64 cells"), Result.CellCount, 2u);
			if (Result.CellCount == 2)
			{
				TestEqual(TEXT("VM preserves high and low token bits"),
					static_cast<uint64>(Result.Cells[0]) | (static_cast<uint64>(Result.Cells[1]) << 32), Reply);
			}
		}
		for (int32 Invalid = 0; Invalid != 3; ++Invalid)
		{
			const auto Rejected = BuildFixture(Invalid == 0 ? "env" : Abi::Module,
				Invalid == 1 ? "avid_continuation_load_object_cancel_resume_v2" : Abi::ObjectLoadCancelResumeImport, Invalid == 2);
			auto RejectingBackend = CreateBackend();
			TestFalse(TEXT("Wrong object import module, version or signature rejects"),
				RejectingBackend->Load(Rejected, TEXT("rejected_object_cancel_resume"), Config, Error));
		}
		TestEqual(TEXT("Rejected modules do not dispatch Host"), Dispatcher.Calls, 2);
		AddInfo(FString::Printf(TEXT("object-cancel-resume backend=%d returns=2 rejected=3"), BackendIndex));
	}
	return true;
}
#endif
