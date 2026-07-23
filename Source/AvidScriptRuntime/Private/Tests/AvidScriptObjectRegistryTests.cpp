#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"

#include "AvidScriptObjectRegistryTestTypes.h"

#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryHandleSmokeTest,
	"AvidScript.Binding.ObjectRegistry.HandleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryHandleSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	UObject* Object = NewObject<UAvidScriptObjectRegistryTestObject>();

	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(Object, RegisterResult);

	TestTrue(TEXT("Registering a live UObject succeeds"), RegisterResult.bSucceeded);
	TestTrue(TEXT("Generated object handle is valid"), Handle.IsValid());
	TestFalse(TEXT("Default registration preserves the diagnostic object path"), RegisterResult.ObjectPath.IsEmpty());
	TestNotEqual(TEXT("Guest-visible handle is not the raw UObject pointer"), Handle.ToUInt64(), reinterpret_cast<uint64>(Object));
	FAvidScriptObjectHandleResult DiagnosticRegisterResult;
	const FAvidScriptObjectHandle DiagnosticHandle = Registry.RegisterObject(Object, DiagnosticRegisterResult, false);
	TestEqual(TEXT("Fast registration reuses the same handle"), DiagnosticHandle, Handle);
	TestTrue(TEXT("Explicit fast registration omits the object path"), DiagnosticRegisterResult.ObjectPath.IsEmpty());

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* ResolvedObject = Registry.ResolveObject<UObject>(Handle, ResolveResult, false);

	TestTrue(TEXT("Resolving a valid handle succeeds"), ResolveResult.bSucceeded);
	TestTrue(TEXT("Resolved object is the registered UObject"), ResolvedObject == Object);
	TestTrue(TEXT("Successful hot-path resolve does not construct an object path"), ResolveResult.ObjectPath.IsEmpty());
	FAvidScriptObjectHandleResult DefaultResolveResult;
	TestTrue(
		TEXT("Default resolve preserves the registered object"),
		Registry.ResolveObject<UObject>(Handle, DefaultResolveResult) == Object);
	TestFalse(TEXT("Default resolve preserves the diagnostic object path"), DefaultResolveResult.ObjectPath.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryGenerationSmokeTest,
	"AvidScript.Binding.ObjectRegistry.GenerationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryGenerationSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;

	UObject* FirstObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult FirstRegisterResult;
	const FAvidScriptObjectHandle FirstHandle = Registry.RegisterObject(FirstObject, FirstRegisterResult);
	TestTrue(TEXT("First object registers"), FirstRegisterResult.bSucceeded);

	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(TEXT("Releasing a live handle succeeds"), Registry.ReleaseHandle(FirstHandle, ReleaseResult));

	UObject* SecondObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult SecondRegisterResult;
	const FAvidScriptObjectHandle SecondHandle = Registry.RegisterObject(SecondObject, SecondRegisterResult);
	TestTrue(TEXT("Second object registers"), SecondRegisterResult.bSucceeded);
	TestEqual(TEXT("Released slot is reused"), SecondHandle.Slot, FirstHandle.Slot);
	TestNotEqual(TEXT("Reused slot receives a new generation"), SecondHandle.Generation, FirstHandle.Generation);

	FAvidScriptObjectHandleResult OldResolveResult;
	UObject* OldResolvedObject = Registry.ResolveObject<UObject>(FirstHandle, OldResolveResult);
	TestNull(TEXT("Old generation does not resolve a reused slot"), OldResolvedObject);
	TestEqual(TEXT("Old generation reports generation mismatch"), OldResolveResult.ErrorCategory, FString(TEXT("generation_mismatch")));

	FAvidScriptObjectHandleResult NewResolveResult;
	UObject* NewResolvedObject = Registry.ResolveObject<UObject>(SecondHandle, NewResolveResult);
	TestTrue(TEXT("New generation resolves"), NewResolveResult.bSucceeded);
	TestTrue(TEXT("New generation resolves the second object"), NewResolvedObject == SecondObject);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryInvalidationSmokeTest,
	"AvidScript.Binding.ObjectRegistry.InvalidationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryInvalidationSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;

	UObject* Object = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(Object, RegisterResult);
	TestTrue(TEXT("Object registers before GC"), RegisterResult.bSucceeded);

	Object = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* ResolvedObject = Registry.ResolveObject<UObject>(Handle, ResolveResult);
	TestNull(TEXT("Collected UObject does not resolve"), ResolvedObject);
	TestEqual(TEXT("Collected UObject reports invalid object"), ResolveResult.ErrorCategory, FString(TEXT("invalid_object")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryResetAndTypeSmokeTest,
	"AvidScript.Binding.ObjectRegistry.ResetAndTypeSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryResetAndTypeSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;

	UObject* ObjectBeforeReset = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterBeforeResetResult;
	const FAvidScriptObjectHandle HandleBeforeReset = Registry.RegisterObject(ObjectBeforeReset, RegisterBeforeResetResult);
	TestTrue(TEXT("Object before reset registers"), RegisterBeforeResetResult.bSucceeded);

	Registry.Reset();

	UObject* ObjectAfterReset = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterAfterResetResult;
	const FAvidScriptObjectHandle HandleAfterReset = Registry.RegisterObject(ObjectAfterReset, RegisterAfterResetResult);
	TestTrue(TEXT("Object after reset registers"), RegisterAfterResetResult.bSucceeded);
	TestEqual(TEXT("Reset can reuse slot numbers"), HandleAfterReset.Slot, HandleBeforeReset.Slot);
	TestNotEqual(TEXT("Reset advances generation domain"), HandleAfterReset.Generation, HandleBeforeReset.Generation);

	FAvidScriptObjectHandleResult OldResolveResult;
	UObject* OldResolvedObject = Registry.ResolveObject<UObject>(HandleBeforeReset, OldResolveResult);
	TestNull(TEXT("Handle from previous generation domain does not resolve"), OldResolvedObject);
	TestEqual(TEXT("Previous generation domain reports generation mismatch"), OldResolveResult.ErrorCategory, FString(TEXT("generation_mismatch")));

	FAvidScriptObjectHandleResult TypeResolveResult;
	UClass* WrongTypeObject = Registry.ResolveObject<UClass>(HandleAfterReset, TypeResolveResult);
	TestNull(TEXT("Wrong typed lookup does not resolve"), WrongTypeObject);
	TestEqual(TEXT("Wrong typed lookup reports type mismatch"), TypeResolveResult.ErrorCategory, FString(TEXT("type_mismatch")));

	return true;
}

#endif
