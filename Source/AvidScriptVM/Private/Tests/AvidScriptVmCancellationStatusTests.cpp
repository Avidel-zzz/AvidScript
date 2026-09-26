#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptContinuationCancellationAbi.h"
#include "AvidScriptVmBackend.h"
#include "AvidScriptVmStaticHostImports.h"
#include "Misc/AutomationTest.h"

namespace AvidScriptCancellationStatusTests
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

// ABI-only fixture: export query(i64)->i32 forwards the complete token to the import.
TArray<uint8> BuildFixture(const ANSICHAR* ModuleName, const ANSICHAR* ImportName, bool bWrongType = false)
{
	TArray<uint8> Bytes = { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };
	TArray<uint8> Types = { 1, 0x60, 1, static_cast<uint8>(bWrongType ? 0x7f : 0x7e), 1, 0x7f };
	AppendSection(Bytes, 1, Types);
	TArray<uint8> Imports = { 1 };
	AppendString(Imports, ModuleName);
	AppendString(Imports, ImportName);
	Imports.Append({ 0, 0 });
	AppendSection(Bytes, 2, Imports);
	AppendSection(Bytes, 3, { 1, 0 });
	TArray<uint8> Exports = { 1 };
	AppendString(Exports, "query");
	Exports.Append({ 0, 1 });
	AppendSection(Bytes, 7, Exports);
	AppendSection(Bytes, 10, { 1, 6, 0, 0x20, 0, 0x10, 0, 0x0b });
	return Bytes;
}

class FDispatcher final : public IAvidScriptHostDispatcher
{
public:
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& Result) override
	{
		++Calls;
		LastToken = Call.Int64Args[0];
		Result.bSucceeded = Call.BindingId == EAvidScriptHostBindingId::ContinuationCancelStatusV1;
		Result.ReturnValue = Reply;
		return Result.bSucceeded;
	}
	int64 LastToken = 0;
	int32 Calls = 0;
	int32 Reply = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmCancellationStatusTest,
	"AvidScript.Architecture.VM.CancellationStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmCancellationStatusTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptCancellationStatusTests;
	using namespace AvidScript::ContinuationCancellation;
	const auto& Import = GetAvidScriptVmStaticHostImport(EAvidScriptHostBindingId::ContinuationCancelStatusV1);
	TestEqual(TEXT("Status import has its frozen name"), FString(UTF8_TO_TCHAR(Import.ImportName)), FString(UTF8_TO_TCHAR(Abi::StatusImport)));
	TestEqual(TEXT("Status signature is i64 to i32"), FString(UTF8_TO_TCHAR(Import.Signature)), FString(TEXT("(I)i")));
	TestTrue(TEXT("Versioned status import is admitted"), IsAvidScriptVmStaticHostImport(TEXT("avidscript"), UTF8_TO_TCHAR(Abi::StatusImport)));
	TestFalse(TEXT("No legacy env alias"), IsAvidScriptVmStaticHostImport(TEXT("env"), UTF8_TO_TCHAR(Abi::StatusImport)));
	TestEqual(TEXT("Append-only binding id preserves older imports"),
		static_cast<uint16>(EAvidScriptHostBindingId::ContinuationCancelStatusV1),
		static_cast<uint16>(EAvidScriptHostBindingId::TaskTerminalErrorRootV1) + 1);
	const TArray<uint8> Fixture = BuildFixture(Abi::Module, Abi::StatusImport);
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
		TUniquePtr<IAvidScriptVmBackend> Backend = CreateBackend();
		if (!TestNotNull(TEXT("Cancellation status backend exists"), Backend.Get())
			|| !TestTrue(TEXT("Cancellation status fixture loads"), Backend->Load(Fixture, TEXT("cancellation_status"), Config, Error)))
		{
			AddError(Error.Category + TEXT(": ") + Error.Details);
			return false;
		}
		FAvidScriptVmExportHandle Export;
		if (!TestTrue(TEXT("Query export resolves"), Backend->ResolveExport(TEXT("query"), Export, Error))) return false;
		const uint64 Token = 0x80000001fedcba98ULL;
		for (int32 Status = 0; Status != 3; ++Status)
		{
			Dispatcher.Reply = Status;
			FAvidScriptVmCallFrame Frame;
			Frame.CellCount = 2;
			Frame.Cells[0] = static_cast<uint32>(Token);
			Frame.Cells[1] = static_cast<uint32>(Token >> 32);
			FAvidScriptVmCallResult Result;
			if (!TestTrue(TEXT("Query calls the status import"), Backend->Call(Export, Frame, Error, &Result)))
			{
				AddError(Error.Category + TEXT(": ") + Error.Details);
				return false;
			}
			TestEqual(TEXT("Query preserves high and low token bits"), static_cast<uint64>(Dispatcher.LastToken), Token);
			TestEqual(TEXT("Query returns one i32 cell"), Result.CellCount, 1u);
			if (Result.CellCount == 1) TestEqual(TEXT("Status is not collapsed to bool"), Result.Cells[0], static_cast<uint32>(Status));
		}
		TestEqual(TEXT("Exactly one host call per query"), Dispatcher.Calls, 3);
		for (int32 Invalid = 0; Invalid != 3; ++Invalid)
		{
			const TArray<uint8> Rejected = BuildFixture(Invalid == 0 ? "env" : Abi::Module,
				Invalid == 1 ? "avid_continuation_cancel_status_v2" : Abi::StatusImport, Invalid == 2);
			TUniquePtr<IAvidScriptVmBackend> RejectingBackend = CreateBackend();
			TestFalse(TEXT("Wrong module, version or signature is rejected"),
				RejectingBackend->Load(Rejected, TEXT("rejected_cancellation_status"), Config, Error));
		}
		TestEqual(TEXT("Rejected fixtures do not call host"), Dispatcher.Calls, 3);
		AddInfo(FString::Printf(TEXT("cancellation-status backend=%d statuses=3 rejected=3"), BackendIndex));
	}
	return true;
}
#endif
