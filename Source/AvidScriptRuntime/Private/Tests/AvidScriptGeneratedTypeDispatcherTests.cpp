#if WITH_DEV_AUTOMATION_TESTS

#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace
{
class FGeneratedTypeInstance final : public IAvidScriptGeneratedTypeInstance
{
public:
	bool InvokeGeneratedTypeMember(
		UObject& Receiver,
		const FAvidScriptObjectHandle& ReceiverHandle,
		const uint32 TypeOrdinal,
		const uint32 MemberOrdinal,
		const TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result) override
	{
		LastReceiver = &Receiver;
		LastReceiverHandle = ReceiverHandle;
		LastTypeOrdinal = TypeOrdinal;
		LastMemberOrdinal = MemberOrdinal;
		if (RegistrationToReset != nullptr)
		{
			bReentrantResetResult = RegistrationToReset->Reset();
		}
		if (Arguments.Num() != 1 || Arguments[0].Data == nullptr || Result == nullptr)
		{
			return false;
		}
		*static_cast<int32*>(Result) = *static_cast<int32*>(Arguments[0].Data) + 1;
		return true;
	}

	UObject* LastReceiver = nullptr;
	FAvidScriptObjectHandle LastReceiverHandle;
	FAvidScriptGeneratedTypeInstanceRegistration* RegistrationToReset = nullptr;
	bool bReentrantResetResult = true;
	uint32 LastTypeOrdinal = MAX_uint32;
	uint32 LastMemberOrdinal = MAX_uint32;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedTypeDispatcherTest,
	"AvidScript.Runtime.GeneratedTypes.Dispatcher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedTypeDispatcherTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	FAvidScriptGeneratedTypeRouter& Router = FAvidScriptGeneratedTypeRouter::Get();
	FGeneratedTypeInstance Instance;
	FGeneratedTypeInstance Competing;
	FAvidScriptGeneratedTypeInstanceRegistration Registration;
	FAvidScriptGeneratedTypeInstanceRegistration CompetingRegistration;
	const int32 BaselineRouteCount = Router.GetRegisteredInstanceCountForTesting();
	UObject* Receiver = GetTransientPackage();
	const FAvidScriptObjectHandle ReceiverHandle{ 17, 5 };
	int32 ArgumentValue = 41;
	int32 ResultValue = 0;
	const FAvidScriptGeneratedCallArgument Argument{ &ArgumentValue };

	TestFalse(
		TEXT("Dispatch fails closed without a registered instance"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver, 3, 7, MakeArrayView(&Argument, 1), &ResultValue));
	TestFalse(
		TEXT("CDOs cannot register as runtime script instances"),
		Router.RegisterInstance(
			*UObject::StaticClass()->GetDefaultObject(),
			ReceiverHandle,
			Instance,
			CompetingRegistration));
	TestFalse(
		TEXT("Invalid ObjectHandles fail closed"),
		Router.RegisterInstance(
			*Receiver,
			FAvidScriptObjectHandle(),
			Instance,
			CompetingRegistration));
	TestTrue(
		TEXT("Session-owned instance registers"),
		Router.RegisterInstance(*Receiver, ReceiverHandle, Instance, Registration));
	TestFalse(
		TEXT("A receiver cannot be claimed by a competing instance"),
		Router.RegisterInstance(*Receiver, ReceiverHandle, Competing, CompetingRegistration));
	TestEqual(
		TEXT("Router adds one instance route"),
		Router.GetRegisteredInstanceCountForTesting(),
		BaselineRouteCount + 1);
	Instance.RegistrationToReset = &Registration;

	TestTrue(
		TEXT("Registered instance receives generated dispatch"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver, 3, 7, MakeArrayView(&Argument, 1), &ResultValue));
	TestEqual(TEXT("Dispatch result is returned"), ResultValue, 42);
	TestTrue(TEXT("Receiver identity is preserved"), Instance.LastReceiver == Receiver);
	TestTrue(TEXT("Stable ObjectHandle is preserved"), Instance.LastReceiverHandle == ReceiverHandle);
	TestEqual(TEXT("Type ordinal is preserved"), Instance.LastTypeOrdinal, 3u);
	TestEqual(TEXT("Member ordinal is preserved"), Instance.LastMemberOrdinal, 7u);
	TestFalse(TEXT("Teardown cannot reenter the active dispatch"), Instance.bReentrantResetResult);
	TestTrue(TEXT("Rejected reentrant teardown preserves registration"), Registration.IsValid());

	Instance.RegistrationToReset = nullptr;
	TestTrue(TEXT("Registration teardown succeeds"), Registration.Reset());
	TestFalse(TEXT("Registration token is invalidated"), Registration.IsValid());
	TestEqual(
		TEXT("Router restores its route baseline"),
		Router.GetRegisteredInstanceCountForTesting(),
		BaselineRouteCount);
	TestFalse(
		TEXT("Registration teardown fences subsequent dispatch"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver, 3, 7, MakeArrayView(&Argument, 1), &ResultValue));
	return true;
}

#endif
