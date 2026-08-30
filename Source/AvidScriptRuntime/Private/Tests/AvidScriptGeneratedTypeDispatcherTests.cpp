#if WITH_DEV_AUTOMATION_TESTS

#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace
{
class FGeneratedTypeDispatchTarget final : public IAvidScriptGeneratedTypeDispatchTarget
{
public:
	bool InvokeGeneratedTypeMember(
		UObject& Receiver,
		const uint32 TypeOrdinal,
		const uint32 MemberOrdinal,
		const TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result) override
	{
		LastReceiver = &Receiver;
		LastTypeOrdinal = TypeOrdinal;
		LastMemberOrdinal = MemberOrdinal;
		if (Arguments.Num() != 1 || Arguments[0].Data == nullptr || Result == nullptr)
		{
			return false;
		}
		*static_cast<int32*>(Result) = *static_cast<int32*>(Arguments[0].Data) + 1;
		return true;
	}

	UObject* LastReceiver = nullptr;
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
	FGeneratedTypeDispatchTarget Primary;
	FGeneratedTypeDispatchTarget Competing;
	UObject* Receiver = GetTransientPackage();
	int32 ArgumentValue = 41;
	int32 ResultValue = 0;
	const FAvidScriptGeneratedCallArgument Argument{ &ArgumentValue };

	TestFalse(
		TEXT("Dispatch fails closed without an installed target"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver, 3, 7, MakeArrayView(&Argument, 1), &ResultValue));
	TestTrue(TEXT("Primary target installs"), FAvidScriptGeneratedTypeDispatcher::Install(Primary));
	TestTrue(TEXT("Primary target install is idempotent"), FAvidScriptGeneratedTypeDispatcher::Install(Primary));
	TestFalse(TEXT("Competing target cannot replace the owner"), FAvidScriptGeneratedTypeDispatcher::Install(Competing));

	TestTrue(
		TEXT("Installed target receives generated dispatch"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver, 3, 7, MakeArrayView(&Argument, 1), &ResultValue));
	TestEqual(TEXT("Dispatch result is returned"), ResultValue, 42);
	TestTrue(TEXT("Receiver identity is preserved"), Primary.LastReceiver == Receiver);
	TestEqual(TEXT("Type ordinal is preserved"), Primary.LastTypeOrdinal, 3u);
	TestEqual(TEXT("Member ordinal is preserved"), Primary.LastMemberOrdinal, 7u);

	FAvidScriptGeneratedTypeDispatcher::Uninstall(Competing);
	TestTrue(
		TEXT("Non-owner uninstall does not remove the primary target"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver, 3, 7, MakeArrayView(&Argument, 1), &ResultValue));
	FAvidScriptGeneratedTypeDispatcher::Uninstall(Primary);
	TestFalse(
		TEXT("Owner uninstall fences subsequent dispatch"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver, 3, 7, MakeArrayView(&Argument, 1), &ResultValue));
	return true;
}

#endif
