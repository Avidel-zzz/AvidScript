#if WITH_DEV_AUTOMATION_TESTS

#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"
#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace AvidScriptGeneratedInvocationTests
{
class FInstance final : public IAvidScriptGeneratedTypeInstance
{
public:
	uint64 GetGeneratedExecutionGeneration() const override { return Generation; }
	bool InvokeGeneratedTypeMember(UObject&, const FAvidScriptObjectHandle&, uint32, uint32,
		TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments, void* Result) override
	{
		++Calls;
		return Invoke ? Invoke(Arguments, Result) : true;
	}
	TFunction<bool(TConstArrayView<FAvidScriptGeneratedCallArgument>, void*)> Invoke;
	uint64 Generation = 1;
	uint32 Calls = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedInvocationChainTest,
	"AvidScript.Runtime.GeneratedTypes.InvocationChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAvidScriptGeneratedInvocationChainTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptGeneratedInvocationTests;
	FAvidScriptGeneratedTypeRouter& Router = FAvidScriptGeneratedTypeRouter::Get();
	FAvidScriptObjectRegistry Objects;
	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	FAvidScriptObjectHandleResult HandleResult;
	const auto HandleA = Objects.RegisterObject(A.Get(), HandleResult, false);
	const auto HandleB = Objects.RegisterObject(B.Get(), HandleResult, false);
	FInstance InstanceA, InstanceB;
	InstanceA.Generation = 7;
	InstanceB.Generation = 9;
	FAvidScriptGeneratedTypeInstanceRegistration RegistrationA, RegistrationB;
	const int32 Baseline = Router.GetRegisteredInstanceCountForTesting();
	if (!TestTrue(TEXT("register A"), Router.RegisterInstance(*A, HandleA, InstanceA, RegistrationA))
		|| !TestTrue(TEXT("register B"), Router.RegisterInstance(*B, HandleB, InstanceB, RegistrationB))) return false;
	int32 Shared = 0, Result = 0;
	const FAvidScriptGeneratedCallArgument Aliases[] = { { &Shared }, { &Shared } };
	TArray<FAvidScriptGeneratedInvocation> Frames;
	auto Dispatch = [&](UObject* Receiver)
	{
		return FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver, 2, 3, MakeArrayView(Aliases), &Result);
	};
	auto Capture = [&]()
	{
		FAvidScriptGeneratedInvocation Frame;
		TestTrue(TEXT("active invocation is observable"), Router.GetActiveInvocation(Frame));
		Frames.Add(Frame);
		return Frame;
	};
	InstanceA.Invoke = [&](TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments, void* Out)
	{
		const auto Frame = Capture();
		TestTrue(TEXT("A receives the same native reference location twice"), Arguments.Num() == 2
			&& Arguments[0].Data == &Shared && Arguments[1].Data == &Shared);
		Shared += Frame.Depth == 1 ? 1 : 4;
		if (Frame.Depth == 1)
		{
			TestTrue(TEXT("A enters B synchronously"), Dispatch(B.Get()));
			FAvidScriptGeneratedInvocation Restored;
			TestTrue(TEXT("return from B restores A invocation"), Router.GetActiveInvocation(Restored)
				&& Restored.InvocationId == Frame.InvocationId);
		}
		*static_cast<int32*>(Out) = Shared;
		return true;
	};
	InstanceB.Invoke = [&](TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments, void*)
	{
		Capture();
		TestTrue(TEXT("B receives the original aliased location"), Arguments[0].Data == Arguments[1].Data
			&& Arguments[0].Data == &Shared);
		Shared += 2;
		TestFalse(TEXT("active target registration cannot be removed"), RegistrationB.Reset());
		return Dispatch(A.Get());
	};
	TestTrue(TEXT("native A to B to A chain completes"), Dispatch(A.Get()));
	TestEqual(TEXT("outer result sees all nested writes"), Result, 7);
	TestEqual(TEXT("three frames recorded"), Frames.Num(), 3);
	if (Frames.Num() != 3) return false;
	TestTrue(TEXT("parent and source links preserve A B A causality"),
		Frames[0].ParentInvocationId == 0 && Frames[0].SourceRegistrationId == 0
		&& Frames[1].ParentInvocationId == Frames[0].InvocationId
		&& Frames[2].ParentInvocationId == Frames[1].InvocationId
		&& Frames[1].SourceRegistrationId == Frames[0].TargetRegistrationId
		&& Frames[2].SourceRegistrationId == Frames[1].TargetRegistrationId
		&& Frames[2].TargetRegistrationId == Frames[0].TargetRegistrationId
		&& Frames[0].TargetRegistrationId != Frames[1].TargetRegistrationId);
	TestTrue(TEXT("code and receiver identities belong to the actual target"),
		Frames[0].ExecutionGeneration == 7 && Frames[1].ExecutionGeneration == 9 && Frames[2].ExecutionGeneration == 7
		&& Frames[0].ReceiverHandle == HandleA && Frames[1].ReceiverHandle == HandleB
		&& Frames[2].RootInvocationId == Frames[0].InvocationId && Frames[2].Depth == 3
		&& Frames[2].TypeOrdinal == 2 && Frames[2].MemberOrdinal == 3);
	FAvidScriptGeneratedInvocation Inactive;
	TestFalse(TEXT("outer return leaves no active context"), Router.GetActiveInvocation(Inactive));
	TestEqual(TEXT("inactive snapshot is cleared"), Inactive.InvocationId, uint64(0));

	InstanceA.Calls = 0;
	InstanceA.Invoke = [&](TConstArrayView<FAvidScriptGeneratedCallArgument>, void*)
	{
		Dispatch(A.Get()); // deliberately ignore failure: the Router must retain it
		return true;
	};
	TestFalse(TEXT("unbounded recursion is rejected even if target ignores failure"), Dispatch(A.Get()));
	TestEqual(TEXT("depth bound rejects before target side effects"), InstanceA.Calls, Router.MaxInvocationDepth);
	TestTrue(TEXT("depth failure is observable"), Router.GetInvocationFailure() == EAvidScriptGeneratedInvocationFailure::DepthLimit);
	TestFalse(TEXT("depth rejection unwinds every frame"), Router.GetActiveInvocation(Inactive));

	InstanceB.Invoke = nullptr;
	InstanceB.Calls = 0;
	InstanceA.Invoke = [&](TConstArrayView<FAvidScriptGeneratedCallArgument>, void*)
	{
		for (uint32 Index = 0; Index < Router.MaxInvocationsPerChain + 2; ++Index) Dispatch(B.Get());
		return true;
	};
	TestFalse(TEXT("sequential child entries share the root budget"), Dispatch(A.Get()));
	TestEqual(TEXT("exhausted chain suppresses later target effects"), InstanceB.Calls, Router.MaxInvocationsPerChain - 1);
	TestTrue(TEXT("entry-budget failure is observable"), Router.GetInvocationFailure() == EAvidScriptGeneratedInvocationFailure::EntryLimit);

	InstanceB.Calls = 0;
	InstanceB.Invoke = [](TConstArrayView<FAvidScriptGeneratedCallArgument>, void*) { return false; };
	InstanceA.Invoke = [&](TConstArrayView<FAvidScriptGeneratedCallArgument>, void*)
	{
		Dispatch(B.Get());
		Dispatch(B.Get());
		return true;
	};
	TestFalse(TEXT("child failure cannot be turned into successful outer dispatch"), Dispatch(A.Get()));
	TestEqual(TEXT("failed chain never reenters the child"), InstanceB.Calls, 1u);
	TestTrue(TEXT("first failure category survives unwinding"), Router.GetInvocationFailure() == EAvidScriptGeneratedInvocationFailure::TargetFailure);

	InstanceB.Invoke = [&](TConstArrayView<FAvidScriptGeneratedCallArgument>, void*) { ++InstanceB.Generation; return true; };
	InstanceA.Invoke = [&](TConstArrayView<FAvidScriptGeneratedCallArgument>, void*) { return Dispatch(B.Get()); };
	TestFalse(TEXT("changing code generation during a call invalidates the chain"), Dispatch(A.Get()));
	TestTrue(TEXT("generation violation is observable"), Router.GetInvocationFailure() == EAvidScriptGeneratedInvocationFailure::GenerationChanged);
	InstanceB.Invoke = nullptr;
	TestTrue(TEXT("independent root receives a fresh call budget and context"), Dispatch(A.Get()));
	TestTrue(TEXT("new root clears the previous failure"), Router.GetInvocationFailure() == EAvidScriptGeneratedInvocationFailure::None);
	InstanceB.Generation = 0;
	TestFalse(TEXT("unavailable target generation rejects before execution"), Dispatch(B.Get()));
	TestTrue(TEXT("zero-generation failure is observable"), Router.GetInvocationFailure() == EAvidScriptGeneratedInvocationFailure::InvalidGeneration);
	InstanceB.Generation = 10;
	TestTrue(TEXT("idle registration can retire"), RegistrationB.Reset());
	TestFalse(TEXT("retired target cannot enter"), Dispatch(B.Get()));
	TestTrue(TEXT("same receiver can register a new lifetime"), Router.RegisterInstance(*B, HandleB, InstanceB, RegistrationB));
	InstanceB.Invoke = [&](TConstArrayView<FAvidScriptGeneratedCallArgument>, void*)
	{
		const auto Current = Capture();
		TestTrue(TEXT("registration and invocation identities are never reused"),
			Current.TargetRegistrationId > Frames[1].TargetRegistrationId && Current.InvocationId > Frames[2].InvocationId);
		return true;
	};
	TestTrue(TEXT("new lifetime dispatch succeeds"), Dispatch(B.Get()));
	TestTrue(TEXT("B teardown"), RegistrationB.Reset());
	TestTrue(TEXT("A teardown"), RegistrationA.Reset());
	TestEqual(TEXT("all test routes are released"), Router.GetRegisteredInstanceCountForTesting(), Baseline);
	return true;
}

#endif
