#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"

namespace
{
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
	uint64 NextSerial = 1;
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

	uint64 Serial = Impl->NextSerial++;
	if (Serial == 0)
	{
		Serial = Impl->NextSerial++;
	}
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
		|| !IsInGameThread()
		|| Receiver.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return false;
	}

	FGeneratedTypeInstanceRoute* Route = Impl->Routes.Find(FObjectKey(&Receiver));
	if (Route == nullptr || Route->Instance == nullptr || !Route->ReceiverHandle.IsValid())
	{
		return false;
	}

	++Impl->ActiveDispatchDepth;
	const bool bInvoked = Route->Instance->InvokeGeneratedTypeMember(
		Receiver,
		Route->ReceiverHandle,
		TypeOrdinal,
		MemberOrdinal,
		Arguments,
		Result);
	--Impl->ActiveDispatchDepth;
	return bInvoked;
}

#if WITH_DEV_AUTOMATION_TESTS
int32 FAvidScriptGeneratedTypeRouter::GetRegisteredInstanceCountForTesting() const
{
	return Impl ? Impl->Routes.Num() : 0;
}
#endif
