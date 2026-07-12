#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"
#include "AvidScriptVmExportTable.h"

#include "Misc/AutomationTest.h"

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
	FAvidScriptVmHostCallContractTest,
	"AvidScript.Architecture.VM.HostCallContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmHostCallContractTest::RunTest(const FString& Parameters)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ActorSetLocation;
	Call.IntArgs[0] = 7;
	Call.IntArgs[1] = 11;
	Call.FloatArgs[0] = 1.0f;
	Call.FloatArgs[1] = 2.0f;
	Call.FloatArgs[2] = 3.0f;

	TestEqual(TEXT("binding id is POD routed"), Call.BindingId, EAvidScriptHostBindingId::ActorSetLocation);
	TestEqual(TEXT("slot survives the frame"), Call.IntArgs[0], 7);
	TestEqual(TEXT("generation survives the frame"), Call.IntArgs[1], 11);
	TestEqual(TEXT("vector z survives the frame"), Call.FloatArgs[2], 3.0f);
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
		0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
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