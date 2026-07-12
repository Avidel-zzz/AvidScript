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

#endif