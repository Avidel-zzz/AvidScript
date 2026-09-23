#include "AvidScriptRuntimeSession.h"
#include "Engine/World.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"

#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeSessionPrivate.h"
#include "Session/AvidScriptRuntimeExecutionDomain.h"
#include "UObject/UnrealType.h"

namespace
{
enum class EGeneratedCallShape : uint8
{
	Unsupported,
	ReceiverVoid,
	ReceiverI32,
	ReceiverF32Void,
};
}

class FGeneratedSessionAuthority final : public IAvidScriptGeneratedTypeAuthority
{
public:
	explicit FGeneratedSessionAuthority(FAvidScriptRuntimeSession& InSession) : Session(InSession) {}
	UObject* ResolveGeneratedTypeReceiver(int64 PackedSelf, uint32 TypeOrdinal,
		const FAvidScriptGeneratedTypeRegistrySnapshot& Registry) const override
	{
		return Session.ResolveGeneratedTypeReceiver(PackedSelf, TypeOrdinal, Registry);
	}
	bool InvokeInstanceExport(FAvidScriptWasmRuntimeInstance& SourceRuntime,
		const FAvidScriptObjectHandle& Target, const FAvidScriptContextualExportCall& Call,
		const FAvidScriptVmCallFrame& Frame, FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult) override
	{
		return Session.InvokeGeneratedInstanceExport(SourceRuntime, Target, Call, Frame, OutError, OutResult);
	}
	bool ResolveInstanceTypeOrdinal(FAvidScriptWasmRuntimeInstance& SourceRuntime,
		const FAvidScriptObjectHandle& Target, const FAvidScriptGeneratedTypeRegistrySnapshot& Registry,
		uint32& OutOrdinal, FAvidScriptVmError& OutError) const override
	{
		return Session.ResolveGeneratedInstanceTypeOrdinal(SourceRuntime, Target, Registry, OutOrdinal, OutError);
	}
private:
	FAvidScriptRuntimeSession& Session;
};

namespace
{
EGeneratedCallShape ResolveCallShape(
	const FAvidScriptGeneratedMemberPlan& Member,
	const FAvidScriptContextualExportCall& Call)
{
	if (Member.Kind != EAvidScriptGeneratedMemberKind::Function)
	{
		return EGeneratedCallShape::Unsupported;
	}
	if (Member.Function == nullptr)
	{
		if (!Member.bLifecycle || Call.GetResultCellCount() != 0)
		{
			return EGeneratedCallShape::Unsupported;
		}
		if (Call.GetParameterCellCount() == 2)
		{
			return EGeneratedCallShape::ReceiverVoid;
		}
		return Call.GetParameterCellCount() == 3
			? EGeneratedCallShape::ReceiverF32Void
			: EGeneratedCallShape::Unsupported;
	}

	FProperty* InputProperty = nullptr;
	FProperty* ReturnProperty = nullptr;
	int32 InputCount = 0;
	for (TFieldIterator<FProperty> Iterator(Member.Function); Iterator; ++Iterator)
	{
		FProperty* const Property = *Iterator;
		if (!Property->HasAnyPropertyFlags(CPF_Parm))
		{
			continue;
		}
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			ReturnProperty = Property;
			continue;
		}
		++InputCount;
		InputProperty = Property;
	}

	if (InputCount == 0 && ReturnProperty == nullptr
		&& Call.GetParameterCellCount() == 2 && Call.GetResultCellCount() == 0)
	{
		return EGeneratedCallShape::ReceiverVoid;
	}
	if (InputCount == 0 && CastField<FIntProperty>(ReturnProperty) != nullptr
		&& Call.GetParameterCellCount() == 2 && Call.GetResultCellCount() == 1)
	{
		return EGeneratedCallShape::ReceiverI32;
	}
	if (InputCount == 1 && CastField<FFloatProperty>(InputProperty) != nullptr
		&& ReturnProperty == nullptr
		&& Call.GetParameterCellCount() == 3 && Call.GetResultCellCount() == 0)
	{
		return EGeneratedCallShape::ReceiverF32Void;
	}
	return EGeneratedCallShape::Unsupported;
}
}

bool FAvidScriptRuntimeSession::ResolveGeneratedInstanceTypeOrdinal(FAvidScriptWasmRuntimeInstance& SourceRuntime,
	const FAvidScriptObjectHandle& Target, const FAvidScriptGeneratedTypeRegistrySnapshot& Registry,
	uint32& OutOrdinal, FAvidScriptVmError& OutError) const
{
	OutError.Reset();
	if (!IsInGameThread() || !SourceRuntime.IsContextInvocationActive() || !GeneratedTypeInstance
		|| GeneratedTypeInstance->Registry.Get() != &Registry)
	{
		OutError.Category = TEXT("generated_invocation_source");
		OutError.Details = TEXT("type selection requires an active generated source and matching registry");
		return false;
	}
	if (Target == HostContext.OwnerHandle)
	{
		const uint32 Ordinal = GeneratedTypeInstance->TypeOrdinal;
		if (ResolveGeneratedTypeReceiver(static_cast<int64>(Target.ToUInt64()), Ordinal, Registry))
		{ OutOrdinal = Ordinal; return true; }
	}
	else if (LiveDomain && LiveRuntime.Get() == &SourceRuntime)
		return LiveDomain->ResolveTypeOrdinal(*this, Target, OutOrdinal, OutError);
	OutError.Category = TEXT("generated_invocation_target");
	OutError.Details = TEXT("type selection requires a valid owner or a live target in the published domain");
	return false;
}

bool FAvidScriptRuntimeSession::InvokeGeneratedInstanceExport(FAvidScriptWasmRuntimeInstance& SourceRuntime,
	const FAvidScriptObjectHandle& Target, const FAvidScriptContextualExportCall& Call,
	const FAvidScriptVmCallFrame& Frame, FAvidScriptVmError& OutError, FAvidScriptVmCallResult* OutResult)
{
	if (!IsInGameThread() || !LiveDomain || LiveRuntime.Get() != &SourceRuntime)
	{
		OutError.Category = TEXT("generated_invocation_domain");
		OutError.Details = TEXT("source instance has no matching published execution domain");
		return false;
	}
	// Retain the domain while nested failure marks members for deferred cleanup.
	auto Domain = LiveDomain;
	return Domain->Invoke(*this, Target, Call, Frame, OutError, OutResult);
}

bool FAvidScriptRuntimeSession::ConfigureGeneratedTypeInstance(
	UObject& Receiver,
	const FAvidScriptObjectHandle& ReceiverHandle,
	const uint32 TypeOrdinal,
	const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
	FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || IsOperationActive() || LiveRuntime || GeneratedTypeInstance)
	{
		OutError = TEXT("generated type instance configuration requires an idle unloaded GameThread session");
		return false;
	}
	if (!Registry.IsValid() || !ReceiverHandle.IsValid()
		|| Receiver.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutError = TEXT("generated type instance configuration has an invalid registry, handle or receiver");
		return false;
	}

	const FAvidScriptGeneratedTypePlan* const Type = Registry->FindTypeByOrdinal(TypeOrdinal);
	if (Type == nullptr || Type->Class == nullptr || !Receiver.IsA(Type->Class))
	{
		OutError = TEXT("generated type instance receiver does not satisfy the manifest UClass");
		return false;
	}
	if (HostContext.OwnerHandle.IsValid() && HostContext.OwnerHandle != ReceiverHandle)
	{
		OutError = TEXT("generated type instance handle does not match the configured Session owner");
		return false;
	}
	if (HostContext.ObjectRegistry != nullptr)
	{
		FAvidScriptObjectHandleResult ResolveResult;
		if (HostContext.ObjectRegistry->ResolveObject(ReceiverHandle, ResolveResult, false) != &Receiver)
		{
			OutError = TEXT("generated type instance handle does not resolve to the configured receiver");
			return false;
		}
	}

	TUniquePtr<FAvidScriptRuntimeGeneratedTypeInstanceState> State =
		MakeUnique<FAvidScriptRuntimeGeneratedTypeInstanceState>();
	State->Registry = Registry;
	State->Receiver = &Receiver;
	State->ReceiverHandle = ReceiverHandle;
	State->TypeOrdinal = TypeOrdinal;
	State->Authority = MakeShared<FGeneratedSessionAuthority>(*this);
	if (!FAvidScriptGeneratedTypeRouter::Get().RegisterInstance(
		Receiver,
		ReceiverHandle,
		*this,
		State->Registration))
	{
		OutError = TEXT("generated type instance router rejected the receiver registration");
		return false;
	}
	GeneratedTypeInstance = MoveTemp(State);
	HostContext.GeneratedTypeAuthority = GeneratedTypeInstance->Authority;
	return true;
}

bool FAvidScriptRuntimeSession::ValidateGeneratedTypeReceiver(const int64 PackedSelf, const uint32 TypeOrdinal) const
{
	return GeneratedTypeInstance && GeneratedTypeInstance->Registry
		&& ResolveGeneratedTypeReceiver(PackedSelf, TypeOrdinal, *GeneratedTypeInstance->Registry) != nullptr;
}

UObject* FAvidScriptRuntimeSession::ResolveGeneratedTypeReceiver(
	const int64 PackedSelf, const uint32 TypeOrdinal, const FAvidScriptGeneratedTypeRegistrySnapshot& Registry) const
{
	if (!IsInGameThread() || bApplicationSuspended || bLifecycleInvalidated || bFaultQuarantined
		|| !GeneratedTypeInstance || !GeneratedTypeInstance->Registration.IsValid()
		|| GeneratedTypeInstance->Registry.Get() != &Registry || HostContext.ObjectRegistry == nullptr
		|| (!LiveRuntime && !bMutationInProgress)) return nullptr;
	const FAvidScriptObjectHandle& Handle = GeneratedTypeInstance->ReceiverHandle;
	if (!Handle.IsValid() || Handle.ToUInt64() != static_cast<uint64>(PackedSelf)
		|| HostContext.OwnerHandle != Handle) return nullptr;
	const FAvidScriptGeneratedTypePlan* Type = GeneratedTypeInstance->Registry->FindTypeByOrdinal(TypeOrdinal);
	UObject* Receiver = GeneratedTypeInstance->Receiver.Get();
	if (Type == nullptr || Type->Class == nullptr || Receiver == nullptr || !Receiver->IsA(Type->Class)
		|| Receiver->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed)) return nullptr;
	if (const UWorld* World = Receiver->GetWorld(); World != nullptr
		&& (World != HostContext.World.Get()
			|| (World->bIsTearingDown && !bGeneratedWorldTeardownLifecycle))) return nullptr;
	FAvidScriptObjectHandleResult ResolveResult;
	return HostContext.ObjectRegistry->ResolveObject(Handle, ResolveResult, false) == Receiver ? Receiver : nullptr;
}

bool FAvidScriptRuntimeSession::ClearGeneratedTypeInstance(FString& OutError)
{
	OutError.Reset();
	if (!GeneratedTypeInstance)
	{
		return true;
	}
	if (!IsInGameThread() || IsOperationActive() || LiveDomain)
	{
		OutError = TEXT("generated type instance teardown requires an idle GameThread session");
		return false;
	}
	if (!GeneratedTypeInstance->Registration.Reset())
	{
		OutError = TEXT("generated type instance router rejected registration teardown");
		return false;
	}
	if (LiveRuntime && HostContext.InstanceExecutionState && !HostContext.InstanceExecutionState->IsRetired()
		&& !LiveRuntime->RetireInstanceExecutionState(HostContext.InstanceExecutionState, OutError)) return false;
	HostContext.GeneratedTypeAuthority.Reset();
	GeneratedTypeInstance.Reset();
	return true;
}

bool FAvidScriptRuntimeSession::PrepareGeneratedTypeExports(
	FAvidScriptWasmRuntimeInstance& Runtime,
	TArray<FAvidScriptGeneratedPreparedTypeRoute>& OutRoutes,
	FString& OutError) const
{
	OutRoutes.Reset();
	OutError.Reset();
	if (!GeneratedTypeInstance)
	{
		return true;
	}
	if (!GeneratedTypeInstance->Registry.IsValid()
		|| !GeneratedTypeInstance->Receiver.IsValid()
		|| !GeneratedTypeInstance->Registration.IsValid())
	{
		OutError = TEXT("generated type instance identity is no longer valid");
		return false;
	}

	UObject* const Receiver = GeneratedTypeInstance->Receiver.Get();
	const FAvidScriptGeneratedTypePlan* const ConcreteType =
		GeneratedTypeInstance->Registry->FindTypeByOrdinal(
			GeneratedTypeInstance->TypeOrdinal);
	if (ConcreteType == nullptr || ConcreteType->Class == nullptr
		|| Receiver == nullptr || !Receiver->IsA(ConcreteType->Class))
	{
		OutError = TEXT("generated type instance no longer satisfies its registry plan");
		return false;
	}

	OutRoutes.SetNum(GeneratedTypeInstance->Registry->Num());
	for (const FAvidScriptGeneratedTypePlan& Type : GeneratedTypeInstance->Registry->GetTypes())
	{
		if (Type.Class == nullptr || !Receiver->IsA(Type.Class))
		{
			continue;
		}
		if (!OutRoutes.IsValidIndex(static_cast<int32>(Type.TypeOrdinal)))
		{
			OutError = TEXT("generated type registry contains a non-dense type ordinal");
			OutRoutes.Reset();
			return false;
		}

		FAvidScriptGeneratedPreparedTypeRoute& Route = OutRoutes[Type.TypeOrdinal];
		Route.bEnabled = true;
		Route.Calls.SetNum(Type.Members.Num());
		Route.CallShapes.SetNumZeroed(Type.Members.Num());
		for (const FAvidScriptGeneratedMemberPlan& Member : Type.Members)
		{
			if (Member.Kind != EAvidScriptGeneratedMemberKind::Function)
			{
				continue;
			}
			FAvidScriptContextualExportCall& Call = Route.Calls[Member.MemberOrdinal];
			FString PrepareError;
			if (!Runtime.PrepareContextualExportCall(Member.ExportName, Call, PrepareError))
			{
				OutError = FString::Printf(
					TEXT("stable_member_id=%s; export=%s; %s"),
					*Member.StableMemberId,
					*Member.ExportName,
					PrepareError.IsEmpty() ? TEXT("prepare failed") : *PrepareError);
				OutRoutes.Reset();
				return false;
			}
			if (Call.GetParameterCellCount() < 2)
			{
				OutError = FString::Printf(
					TEXT("generated export '%s' omits the packed ObjectHandle receiver"),
					*Member.ExportName);
				OutRoutes.Reset();
				return false;
			}
			Route.CallShapes[Member.MemberOrdinal] = static_cast<uint8>(
				ResolveCallShape(Member, Call));
		}
	}
	return true;
}

bool FAvidScriptRuntimeSession::InvokeGeneratedTypeMember(
	UObject& Receiver,
	const FAvidScriptObjectHandle& ReceiverHandle,
	const uint32 TypeOrdinal,
	const uint32 MemberOrdinal,
	const TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
	void* Result)
{
	FAvidScriptWasmSmokeResult EntryFailure;
	if (!IsInGameThread()
		|| !CanEnterGuest(TEXT("<generated_type>"), EntryFailure)
		|| !IsLiveLoaded()
		|| !GeneratedTypeInstance
		|| GeneratedTypeInstance->Receiver.Get() != &Receiver
		|| GeneratedTypeInstance->ReceiverHandle != ReceiverHandle
		|| !GeneratedTypeInstance->Registry.IsValid()
		|| !GeneratedTypeInstance->PreparedTypeRoutes.IsValidIndex(static_cast<int32>(TypeOrdinal)))
	{
		return false;
	}
	const FAvidScriptGeneratedTypePlan* const Type =
		GeneratedTypeInstance->Registry->FindTypeByOrdinal(TypeOrdinal);
	if (Type == nullptr
		|| !Type->Members.IsValidIndex(static_cast<int32>(MemberOrdinal)))
	{
		return false;
	}
	const FAvidScriptGeneratedMemberPlan& Member = Type->Members[MemberOrdinal];
	const FString& ExportName = Member.ExportName;
	// UE marks the World as tearing down before Actor EndPlay and subsystem Deinitialize.
	// Only these native terminal routes may enter their already-owned Session.
	const bool bWorldTeardownLifecycle = Member.bLifecycle
		&& ((Receiver.IsA<UWorldSubsystem>()
				&& Member.StableMemberId.EndsWith(TEXT(".Deinitialize():void"), ESearchCase::CaseSensitive))
			|| ((Receiver.IsA<AActor>() || Receiver.IsA<UActorComponent>())
				&& Member.StableMemberId.EndsWith(TEXT(".EndPlay():void"), ESearchCase::CaseSensitive)))
		&& HostContext.World.IsValid()
		&& HostContext.World->bIsTearingDown;

	const FAvidScriptGeneratedPreparedTypeRoute& Route =
		GeneratedTypeInstance->PreparedTypeRoutes[TypeOrdinal];
	if (!Route.bEnabled
		|| !Route.Calls.IsValidIndex(static_cast<int32>(MemberOrdinal))
		|| !Route.CallShapes.IsValidIndex(static_cast<int32>(MemberOrdinal)))
	{
		return false;
	}

	const FAvidScriptContextualExportCall& Call = Route.Calls[MemberOrdinal];
	const EGeneratedCallShape Shape = static_cast<EGeneratedCallShape>(
		Route.CallShapes[MemberOrdinal]);
	if (!Call.IsValid() || Shape == EGeneratedCallShape::Unsupported)
	{
		return false;
	}

	FAvidScriptVmCallFrame Frame;
	Frame.Cells[0] = ReceiverHandle.Slot;
	Frame.Cells[1] = ReceiverHandle.Generation;
	Frame.CellCount = 2;
	if (Shape == EGeneratedCallShape::ReceiverF32Void)
	{
		if (Arguments.Num() != 1 || Arguments[0].Data == nullptr || Result != nullptr)
		{
			return false;
		}
		FMemory::Memcpy(&Frame.Cells[2], Arguments[0].Data, sizeof(uint32));
		Frame.CellCount = 3;
	}
	else if (!Arguments.IsEmpty())
	{
		return false;
	}

	FAvidScriptVmCallResult CallResult;
	FAvidScriptVmError Error;
	if ((Shape == EGeneratedCallShape::ReceiverI32 && Result == nullptr)
		|| (Shape != EGeneratedCallShape::ReceiverI32 && Result != nullptr))
	{
		return false;
	}
	bool bCalled = false;
	bool bResultValid = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		TGuardValue<bool> TeardownCallGuard(
			bGeneratedWorldTeardownLifecycle, bWorldTeardownLifecycle);
		FAvidScriptWasmHostContext InvocationContext = HostContext;
		InvocationContext.bAllowWorldTeardownLifecycle = bWorldTeardownLifecycle;
		bCalled = LiveRuntime->InvokeInContext(Call, InvocationContext,
			Frame,
			Error,
			Shape == EGeneratedCallShape::ReceiverI32
				? &CallResult
				: nullptr);
		bResultValid = bCalled
			&& (Shape != EGeneratedCallShape::ReceiverI32
				|| CallResult.CellCount == 1);
	}
	if (!bCalled)
	{
		FAvidScriptWasmSmokeResult Failure;
		LiveRuntime->RecordContextualFailure(HostContext,
			ExportName,
			Error,
			Failure);
		QuarantineFaultedRuntime(Failure);
		return false;
	}
	if (Shape == EGeneratedCallShape::ReceiverI32)
	{
		if (!bResultValid)
		{
			return false;
		}
		*static_cast<int32*>(Result) = static_cast<int32>(CallResult.Cells[0]);
		return true;
	}
	return bResultValid;
}
