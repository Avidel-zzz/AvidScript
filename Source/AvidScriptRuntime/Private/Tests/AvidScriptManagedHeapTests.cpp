#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptWasmRuntime.h"
#include "AvidScriptRuntimeSession.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Misc/AutomationTest.h"
#include <array>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptManagedHeapOwnershipTest,
	"AvidScript.Runtime.ManagedHeap.Ownership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptManagedHeapOwnershipTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	FAvidScriptWasmRuntimeInstance Live, Candidate;
	FAvidScriptWasmSmokeResult Result;
	if (!TestTrue(TEXT("Live module loads"), Live.LoadEmbeddedSmokeModule(Result))) return false;
	FHeap* LiveHeap = Live.GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Loaded module owns heap"), LiveHeap)) return false;
	const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
	FToken Root = 0, Object = 0;
	TestTrue(TEXT("Configure environment layout"), LiveHeap->Configure(Layouts) == EHeapError::Ok);
	TestTrue(TEXT("Create persistent root"), LiveHeap->CreateRoot(0, 0, Root) == EHeapError::Ok);
	TestTrue(TEXT("Allocate environment"), LiveHeap->Allocate(1, Root, Object) == EHeapError::Ok);
	const uint8 InvalidModule[] = {0, 0, 0, 0};
	TestFalse(TEXT("Invalid candidate rejected"), Candidate.LoadModule(InvalidModule, UE_ARRAY_COUNT(InvalidModule), TEXT("invalid_heap_candidate"), Result));
	TestNull(TEXT("Failed candidate has no heap"), Candidate.GetManagedHeapForTesting());
	TestTrue(TEXT("Failed candidate preserves live environment"), LiveHeap->IsAlive(Object));
	if (!TestTrue(TEXT("Valid candidate loads"), Candidate.LoadEmbeddedSmokeModule(Result))) return false;
	FHeap* CandidateHeap = Candidate.GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Candidate owns separate heap"), CandidateHeap)) return false;
	TestTrue(TEXT("Candidate configure"), CandidateHeap->Configure(Layouts) == EHeapError::Ok);
	TestFalse(TEXT("Live token cannot enter candidate"), CandidateHeap->IsAlive(Object));
	Candidate.Unload();
	TestNull(TEXT("Candidate unload releases heap"), Candidate.GetManagedHeapForTesting());
	TestTrue(TEXT("Candidate unload preserves live heap"), LiveHeap->IsAlive(Object));
	Live.Unload(); Live.Unload();
	TestNull(TEXT("Repeated unload releases live heap"), Live.GetManagedHeapForTesting());
	if (!TestTrue(TEXT("Same runtime can load fresh module"), Live.LoadEmbeddedSmokeModule(Result))) return false;
	FHeap* FreshHeap = Live.GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Fresh module owns fresh heap"), FreshHeap)) return false;
	TestTrue(TEXT("Fresh configure"), FreshHeap->Configure(Layouts) == EHeapError::Ok);
	FToken FreshRoot = 0, FreshObject = 0;
	TestTrue(TEXT("Fresh root"), FreshHeap->CreateRoot(0, 0, FreshRoot) == EHeapError::Ok);
	TestTrue(TEXT("Fresh allocation"), FreshHeap->Allocate(1, FreshRoot, FreshObject) == EHeapError::Ok);
	TestTrue(TEXT("New module tokens differ"), FreshObject != Object);
	TestFalse(TEXT("Prior module token remains invalid"), FreshHeap->IsAlive(Object));

	// Exercise actual Session prepare/rollback/commit, including a BeginPlay trap.
	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult ReloadResult;
	if (!TestTrue(TEXT("Session loads live module"), Session.LoadEmbeddedSmoke(ReloadResult))) return false;
	FAvidScriptWasmRuntimeInstance* OriginalRuntime = Session.GetLiveRuntimeForTesting();
	FHeap* OriginalHeap = OriginalRuntime->GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Session owns module heap"), OriginalHeap)) return false;
	TestTrue(TEXT("Session heap configure"), OriginalHeap->Configure(Layouts) == EHeapError::Ok);
	FToken SessionRoot = 0, SessionObject = 0;
	TestTrue(TEXT("Session root"), OriginalHeap->CreateRoot(0, 0, SessionRoot) == EHeapError::Ok);
	TestTrue(TEXT("Session allocation"), OriginalHeap->Allocate(1, SessionRoot, SessionObject) == EHeapError::Ok);
	const uint8 BeginTrap[] = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
		0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
		0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
		0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
		0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
		0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
		0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x08,
		0x02, 0x03, 0x00, 0x00, 0x0b, 0x02, 0x00, 0x0b
	};
	TestFalse(TEXT("Candidate BeginPlay trap rejects reload"), Session.ReloadModule(BeginTrap, UE_ARRAY_COUNT(BeginTrap),
		FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("heap_begin_trap")), ReloadResult));
	TestTrue(TEXT("Rollback preserves live runtime identity"), ReloadResult.bRollbackPreservedLiveRuntime && Session.GetLiveRuntimeForTesting() == OriginalRuntime);
	TestTrue(TEXT("Rollback preserves live environment"), OriginalHeap->IsAlive(SessionObject));
	TArray<uint8> Recover(BeginTrap, UE_ARRAY_COUNT(BeginTrap));
	Recover[UE_ARRAY_COUNT(BeginTrap) - 5] = 0x01; // Replace unreachable with nop in BeginPlay.
	if (!TestTrue(TEXT("Compatible candidate commits"), Session.ReloadModule(Recover.GetData(), Recover.Num(),
		FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("heap_recovered")), ReloadResult))) return false;
	FHeap* CommittedHeap = Session.GetLiveRuntimeForTesting()->GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Committed runtime owns heap"), CommittedHeap)) return false;
	TestTrue(TEXT("Committed heap configure"), CommittedHeap->Configure(Layouts) == EHeapError::Ok);
	TestFalse(TEXT("Committed module rejects old environment"), CommittedHeap->IsAlive(SessionObject));
	TestTrue(TEXT("Session stop succeeds"), Session.StopAndUnload(Result));
	TestNull(TEXT("Session stop releases runtime and heap"), Session.GetLiveRuntimeForTesting());
	return true;
}
#endif
