#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "UObject/ObjectKey.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptGeneratedTypeRuntimeHost, Log, All);

namespace
{
struct FGeneratedTypeRuntimePackage
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FAvidScriptRuntimeArtifact Artifact;
};

struct FGeneratedTypeRuntimeInstance
{
	TWeakObjectPtr<UObject> Receiver;
	FAvidScriptObjectHandle ReceiverHandle;
	uint32 TypeOrdinal = 0;
	TUniquePtr<FAvidScriptRuntimeSession> Session;
};

bool TeardownInstance(
	FGeneratedTypeRuntimeInstance& Instance,
	FAvidScriptObjectRegistry& ObjectRegistry,
	FString& OutError)
{
	bool bSucceeded = true;
	if (Instance.Session)
	{
		if (Instance.Session->IsLiveLoaded())
		{
			FAvidScriptWasmSmokeResult StopResult;
			if (!Instance.Session->StopAndUnload(StopResult))
			{
				bSucceeded = false;
				OutError = StopResult.ErrorMessage.IsEmpty()
					? TEXT("generated type Session failed to stop")
					: StopResult.ErrorMessage;
			}
		}
		FString ClearError;
		if (!Instance.Session->ClearGeneratedTypeInstance(ClearError))
		{
			bSucceeded = false;
			if (OutError.IsEmpty())
			{
				OutError = ClearError;
			}
		}
		Instance.Session.Reset();
	}
	if (Instance.ReceiverHandle.IsValid())
	{
		FAvidScriptObjectHandleResult ReleaseResult;
		if (!ObjectRegistry.ReleaseHandle(
			Instance.ReceiverHandle,
			ReleaseResult,
			false))
		{
			bSucceeded = false;
			if (OutError.IsEmpty())
			{
				OutError = ReleaseResult.ErrorMessage.IsEmpty()
					? TEXT("generated type ObjectHandle release failed")
					: ReleaseResult.ErrorMessage;
			}
		}
		Instance.ReceiverHandle = {};
	}
	return bSucceeded;
}
}

struct FAvidScriptGeneratedTypeRuntimeHost::FImpl
{
	bool bStarted = false;
	TOptional<FGeneratedTypeRuntimePackage> Package;
	FAvidScriptObjectRegistry ObjectRegistry;
	TMap<FObjectKey, TUniquePtr<FGeneratedTypeRuntimeInstance>> Instances;
};

FAvidScriptGeneratedTypeRuntimeHost& FAvidScriptGeneratedTypeRuntimeHost::Get()
{
	static FAvidScriptGeneratedTypeRuntimeHost Host;
	return Host;
}

FAvidScriptGeneratedTypeRuntimeHost::FAvidScriptGeneratedTypeRuntimeHost() = default;

FAvidScriptGeneratedTypeRuntimeHost::~FAvidScriptGeneratedTypeRuntimeHost()
{
	ensureMsgf(!Impl, TEXT("Generated type Runtime host must shut down before static destruction."));
}

bool FAvidScriptGeneratedTypeRuntimeHost::Startup()
{
	if (!IsInGameThread())
	{
		return false;
	}
	if (Impl)
	{
		return Impl->bStarted;
	}
	Impl = MakeUnique<FImpl>();
	Impl->bStarted = true;
	return true;
}

void FAvidScriptGeneratedTypeRuntimeHost::Shutdown()
{
	if (!Impl)
	{
		return;
	}
	check(IsInGameThread());
	for (TPair<FObjectKey, TUniquePtr<FGeneratedTypeRuntimeInstance>>& Pair : Impl->Instances)
	{
		FString Error;
		ensureMsgf(
			TeardownInstance(*Pair.Value, Impl->ObjectRegistry, Error),
			TEXT("Generated type Runtime host shutdown failed: %s"),
			Error.IsEmpty() ? TEXT("unknown") : *Error);
	}
	Impl->Instances.Reset();
	Impl->ObjectRegistry.Reset();
	Impl->Package.Reset();
	Impl.Reset();
}

bool FAvidScriptGeneratedTypeRuntimeHost::InstallPackage(
	const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
	const FAvidScriptRuntimeArtifact& Artifact,
	FString& OutError)
{
	OutError.Reset();
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type package installation requires a started GameThread host");
		return false;
	}
	if (!Impl->Instances.IsEmpty())
	{
		OutError = TEXT("generated type package replacement requires zero active instances");
		return false;
	}
	if (!Registry.IsValid() || Registry->Num() == 0)
	{
		OutError = TEXT("generated type package registry is empty");
		return false;
	}
	if (Artifact.Manifest.ModuleId.IsEmpty()
		|| Artifact.Manifest.RequiredExports.IsEmpty()
		|| Artifact.VmArtifact.ExecutionBytes.IsEmpty()
			&& Artifact.VmArtifact.CanonicalWasmBytes.IsEmpty())
	{
		OutError = TEXT("generated type package Runtime artifact is incomplete");
		return false;
	}

	Impl->Package.Emplace(FGeneratedTypeRuntimePackage{ Registry, Artifact });
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::ClearPackage(FString& OutError)
{
	OutError.Reset();
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type package clear requires a started GameThread host");
		return false;
	}
	if (!Impl->Instances.IsEmpty())
	{
		OutError = TEXT("generated type package clear requires zero active instances");
		return false;
	}
	Impl->Package.Reset();
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::BeginInstance(
	UObject& Receiver,
	const uint32 TypeOrdinal,
	FString& OutError)
{
	OutError.Reset();
	if (!Impl || !Impl->bStarted || !IsInGameThread() || !Impl->Package.IsSet())
	{
		OutError = TEXT("generated type instance activation requires an installed GameThread package");
		return false;
	}
	if (Receiver.HasAnyFlags(
		RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutError = TEXT("generated type instance receiver is not a live runtime object");
		return false;
	}

	const FObjectKey ReceiverKey(&Receiver);
	if (Impl->Instances.Contains(ReceiverKey))
	{
		return true;
	}
	const FGeneratedTypeRuntimePackage& Package = Impl->Package.GetValue();
	const FAvidScriptGeneratedTypePlan* const RequestedType =
		Package.Registry->FindTypeByOrdinal(TypeOrdinal);
	if (RequestedType == nullptr || RequestedType->Class == nullptr
		|| !Receiver.IsA(RequestedType->Class))
	{
		OutError = TEXT("generated type instance does not satisfy the installed type ordinal");
		return false;
	}
	const FAvidScriptGeneratedTypePlan* RuntimeType = nullptr;
	for (UClass* Class = Receiver.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
	{
		RuntimeType = Package.Registry->FindTypeByClass(Class);
		if (RuntimeType != nullptr)
		{
			break;
		}
	}
	if (RuntimeType == nullptr)
	{
		OutError = TEXT("generated type instance has no registered runtime UClass ancestry");
		return false;
	}

	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ReceiverHandle =
		Impl->ObjectRegistry.RegisterObject(&Receiver, RegisterResult, false);
	if (!ReceiverHandle.IsValid())
	{
		OutError = RegisterResult.ErrorMessage.IsEmpty()
			? TEXT("generated type instance ObjectHandle registration failed")
			: RegisterResult.ErrorMessage;
		return false;
	}

	TUniquePtr<FGeneratedTypeRuntimeInstance> Instance =
		MakeUnique<FGeneratedTypeRuntimeInstance>();
	Instance->Receiver = &Receiver;
	Instance->ReceiverHandle = ReceiverHandle;
	Instance->TypeOrdinal = RuntimeType->TypeOrdinal;
	Instance->Session = MakeUnique<FAvidScriptRuntimeSession>();

	FAvidScriptWasmHostContext HostContext;
	HostContext.World = Receiver.GetWorld();
	HostContext.ObjectRegistry = &Impl->ObjectRegistry;
	HostContext.OwnerHandle = ReceiverHandle;
	Instance->Session->SetHostContext(HostContext);
	if (!Instance->Session->ConfigureGeneratedTypeInstance(
		Receiver,
		ReceiverHandle,
		RuntimeType->TypeOrdinal,
		Package.Registry,
		OutError))
	{
		TeardownInstance(*Instance, Impl->ObjectRegistry, OutError);
		return false;
	}

	FAvidScriptWasmReloadResult LoadResult;
	if (!Instance->Session->LoadInitialArtifact(Package.Artifact, LoadResult))
	{
		OutError = LoadResult.ErrorMessage.IsEmpty()
			? TEXT("generated type instance Runtime artifact load failed")
			: LoadResult.ErrorMessage;
		FString TeardownError;
		TeardownInstance(*Instance, Impl->ObjectRegistry, TeardownError);
		return false;
	}

	Impl->Instances.Add(ReceiverKey, MoveTemp(Instance));
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::EndInstance(UObject& Receiver, FString& OutError)
{
	OutError.Reset();
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type instance teardown requires a started GameThread host");
		return false;
	}

	const FObjectKey ReceiverKey(&Receiver);
	TUniquePtr<FGeneratedTypeRuntimeInstance>* const Found =
		Impl->Instances.Find(ReceiverKey);
	if (Found == nullptr || !Found->IsValid())
	{
		return true;
	}
	TUniquePtr<FGeneratedTypeRuntimeInstance> Instance = MoveTemp(*Found);
	Impl->Instances.Remove(ReceiverKey);
	return TeardownInstance(*Instance, Impl->ObjectRegistry, OutError);
}

bool FAvidScriptGeneratedTypeRuntimeHost::IsInstanceActive(const UObject& Receiver) const
{
	return Impl && Impl->Instances.Contains(FObjectKey(&Receiver));
}

int32 FAvidScriptGeneratedTypeRuntimeHost::GetActiveInstanceCount() const
{
	return Impl ? Impl->Instances.Num() : 0;
}

int32 FAvidScriptGeneratedTypeRuntimeHost::GetRegisteredHandleCount() const
{
	return Impl ? Impl->ObjectRegistry.GetLiveHandleCount() : 0;
}
