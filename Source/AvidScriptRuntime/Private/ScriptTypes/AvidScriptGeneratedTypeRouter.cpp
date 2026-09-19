#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"

namespace
{
// GameThread-only, never recycled by Router shutdown/startup.
uint64 GGeneratedRegistrationSerial = 0;
uint64 GGeneratedInvocationSerial = 0;

struct FGeneratedTypeInstanceRoute
{
	FAvidScriptObjectHandle ReceiverHandle;
	IAvidScriptGeneratedTypeInstance* Instance = nullptr;
	uint64 Serial = 0;
};
}

struct FAvidScriptGeneratedTypeRouter::FImpl
{
	TMap<FObjectKey, FGeneratedTypeInstanceRoute> Routes;
	TArray<FAvidScriptGeneratedInvocation> InvocationStack;
	uint32 ChainEntries = 0;
	EAvidScriptGeneratedInvocationFailure ChainFailure = EAvidScriptGeneratedInvocationFailure::None;
	int32 ActiveDispatchDepth = 0;
	bool bInstalled = false;
};

FAvidScriptGeneratedTypeInstanceRegistration::~FAvidScriptGeneratedTypeInstanceRegistration()
{
	ensureMsgf(Reset(), TEXT("Generated type instance registration was destroyed during active dispatch."));
}

FAvidScriptGeneratedTypeInstanceRegistration::FAvidScriptGeneratedTypeInstanceRegistration(
	FAvidScriptGeneratedTypeInstanceRegistration&& Other)
{
	MoveFrom(Other);
}

FAvidScriptGeneratedTypeInstanceRegistration&
FAvidScriptGeneratedTypeInstanceRegistration::operator=(
	FAvidScriptGeneratedTypeInstanceRegistration&& Other)
{
	if (this != &Other)
	{
		ensureMsgf(Reset(), TEXT("Generated type instance registration move raced active dispatch."));
		MoveFrom(Other);
	}
	return *this;
}

bool FAvidScriptGeneratedTypeInstanceRegistration::IsValid() const
{
	return Instance != nullptr && Serial != 0;
}

bool FAvidScriptGeneratedTypeInstanceRegistration::Reset()
{
	return FAvidScriptGeneratedTypeRouter::Get().UnregisterInstance(*this);
}

void FAvidScriptGeneratedTypeInstanceRegistration::Invalidate()
{
	ReceiverKey = FObjectKey();
	Instance = nullptr;
	Serial = 0;
}

void FAvidScriptGeneratedTypeInstanceRegistration::MoveFrom(
	FAvidScriptGeneratedTypeInstanceRegistration& Other)
{
	ReceiverKey = Other.ReceiverKey;
	Instance = Other.Instance;
	Serial = Other.Serial;
	Other.Invalidate();
}

FAvidScriptGeneratedTypeRouter& FAvidScriptGeneratedTypeRouter::Get()
{
	static FAvidScriptGeneratedTypeRouter Router;
	return Router;
}

FAvidScriptGeneratedTypeRouter::FAvidScriptGeneratedTypeRouter() = default;

FAvidScriptGeneratedTypeRouter::~FAvidScriptGeneratedTypeRouter()
{
	ensureMsgf(!Impl, TEXT("Generated type router must shut down before static destruction."));
}

bool FAvidScriptGeneratedTypeRouter::Startup()
{
	if (!IsInGameThread())
	{
		return false;
	}
	if (Impl)
	{
		return Impl->bInstalled;
	}

	Impl = MakeUnique<FImpl>();
	Impl->InvocationStack.Reserve(MaxInvocationDepth);
	Impl->bInstalled = FAvidScriptGeneratedTypeDispatcher::Install(*this);
	if (!Impl->bInstalled)
	{
		Impl.Reset();
		return false;
	}
	return true;
}

void FAvidScriptGeneratedTypeRouter::Shutdown()
{
	if (!Impl)
	{
		return;
	}
	check(IsInGameThread());
	checkf(
		Impl->ActiveDispatchDepth == 0,
		TEXT("Generated type router shutdown must run outside generated dispatch."));

	FAvidScriptGeneratedTypeDispatcher::Uninstall(*this);
	Impl->Routes.Reset();
	Impl.Reset();
}

bool FAvidScriptGeneratedTypeRouter::RegisterInstance(
	UObject& Receiver,
	const FAvidScriptObjectHandle& ReceiverHandle,
	IAvidScriptGeneratedTypeInstance& Instance,
	FAvidScriptGeneratedTypeInstanceRegistration& OutRegistration)
{
	if (!Impl
		|| !Impl->bInstalled
		|| !IsInGameThread()
		|| Impl->ActiveDispatchDepth != 0
		|| OutRegistration.IsValid()
		|| !ReceiverHandle.IsValid()
		|| Receiver.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return false;
	}

	for (auto Iterator = Impl->Routes.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator.Key().ResolveObjectPtr() == nullptr)
		{
			Iterator.RemoveCurrent();
		}
	}

	const FObjectKey ReceiverKey(&Receiver);
	if (Impl->Routes.Contains(ReceiverKey))
	{
		return false;
	}

	if (GGeneratedRegistrationSerial == MAX_uint64)
	{
		return false;
	}
	const uint64 Serial = ++GGeneratedRegistrationSerial;
	Impl->Routes.Add(ReceiverKey, FGeneratedTypeInstanceRoute{ ReceiverHandle, &Instance, Serial });
	OutRegistration.ReceiverKey = ReceiverKey;
	OutRegistration.Instance = &Instance;
	OutRegistration.Serial = Serial;
	return true;
}

bool FAvidScriptGeneratedTypeRouter::UnregisterInstance(
	FAvidScriptGeneratedTypeInstanceRegistration& Registration)
{
	if (!Registration.IsValid())
	{
		Registration.Invalidate();
		return true;
	}
	if (!Impl)
	{
		Registration.Invalidate();
		return true;
	}
	if (!IsInGameThread() || Impl->ActiveDispatchDepth != 0)
	{
		return false;
	}

	const FGeneratedTypeInstanceRoute* Route = Impl->Routes.Find(Registration.ReceiverKey);
	if (Route == nullptr
		|| Route->Instance != Registration.Instance
		|| Route->Serial != Registration.Serial)
	{
		return false;
	}
	Impl->Routes.Remove(Registration.ReceiverKey);
	Registration.Invalidate();
	return true;
}

bool FAvidScriptGeneratedTypeRouter::InvokeGeneratedTypeMember(
	UObject& Receiver,
	const uint32 TypeOrdinal,
	const uint32 MemberOrdinal,
	const TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
	void* Result)
{
	if (!Impl
		|| !Impl->bInstalled
		|| !IsInGameThread())
	{
		return false;
	}
	if (Impl->InvocationStack.IsEmpty())
	{
		Impl->ChainEntries = 0;
		Impl->ChainFailure = EAvidScriptGeneratedInvocationFailure::None;
	}
	const auto Fail = [this](EAvidScriptGeneratedInvocationFailure Failure)
	{
		if (Impl->ChainFailure == EAvidScriptGeneratedInvocationFailure::None)
		{
			Impl->ChainFailure = Failure;
		}
		return false;
	};
	if (Impl->ChainFailure != EAvidScriptGeneratedInvocationFailure::None) return false;
	if (!IsValid(&Receiver)
		|| Receiver.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Fail(EAvidScriptGeneratedInvocationFailure::InvalidReceiver);
	}

	FGeneratedTypeInstanceRoute* Route = Impl->Routes.Find(FObjectKey(&Receiver));
	if (Route == nullptr || Route->Instance == nullptr || !Route->ReceiverHandle.IsValid())
	{
		return Fail(EAvidScriptGeneratedInvocationFailure::UnregisteredReceiver);
	}
	const uint64 Generation = Route->Instance->GetGeneratedExecutionGeneration();
	if (Generation == 0) return Fail(EAvidScriptGeneratedInvocationFailure::InvalidGeneration);
	if (Impl->InvocationStack.Num() >= static_cast<int32>(MaxInvocationDepth))
		return Fail(EAvidScriptGeneratedInvocationFailure::DepthLimit);
	if (Impl->ChainEntries >= MaxInvocationsPerChain)
		return Fail(EAvidScriptGeneratedInvocationFailure::EntryLimit);
	if (GGeneratedInvocationSerial == MAX_uint64)
		return Fail(EAvidScriptGeneratedInvocationFailure::IdentityExhausted);

	FAvidScriptGeneratedInvocation Invocation;
	Invocation.InvocationId = ++GGeneratedInvocationSerial;
	Invocation.RootInvocationId = Impl->InvocationStack.IsEmpty()
		? Invocation.InvocationId : Impl->InvocationStack[0].InvocationId;
	if (!Impl->InvocationStack.IsEmpty())
	{
		Invocation.ParentInvocationId = Impl->InvocationStack.Last().InvocationId;
		Invocation.SourceRegistrationId = Impl->InvocationStack.Last().TargetRegistrationId;
	}
	Invocation.TargetRegistrationId = Route->Serial;
	Invocation.ExecutionGeneration = Generation;
	Invocation.ReceiverHandle = Route->ReceiverHandle;
	Invocation.TypeOrdinal = TypeOrdinal;
	Invocation.MemberOrdinal = MemberOrdinal;
	Invocation.Depth = static_cast<uint32>(Impl->InvocationStack.Num()) + 1;
	Impl->InvocationStack.Add(Invocation);
	++Impl->ChainEntries;

	++Impl->ActiveDispatchDepth;
	const bool bInvoked = Route->Instance->InvokeGeneratedTypeMember(
		Receiver,
		Route->ReceiverHandle,
		TypeOrdinal,
		MemberOrdinal,
		Arguments,
		Result);
	if (!bInvoked) Fail(EAvidScriptGeneratedInvocationFailure::TargetFailure);
	if (Route->Instance->GetGeneratedExecutionGeneration() != Generation)
		Fail(EAvidScriptGeneratedInvocationFailure::GenerationChanged);
	--Impl->ActiveDispatchDepth;
	Impl->InvocationStack.Pop(EAllowShrinking::No);
	return bInvoked && Impl->ChainFailure == EAvidScriptGeneratedInvocationFailure::None;
}

bool FAvidScriptGeneratedTypeRouter::GetActiveInvocation(FAvidScriptGeneratedInvocation& OutInvocation) const
{
	OutInvocation = {};
	if (!IsInGameThread() || !Impl || Impl->InvocationStack.IsEmpty()) return false;
	OutInvocation = Impl->InvocationStack.Last();
	return true;
}

EAvidScriptGeneratedInvocationFailure FAvidScriptGeneratedTypeRouter::GetInvocationFailure() const
{
	return IsInGameThread() && Impl ? Impl->ChainFailure : EAvidScriptGeneratedInvocationFailure::None;
}

#if WITH_DEV_AUTOMATION_TESTS
int32 FAvidScriptGeneratedTypeRouter::GetRegisteredInstanceCountForTesting() const
{
	return Impl ? Impl->Routes.Num() : 0;
}
#endif
