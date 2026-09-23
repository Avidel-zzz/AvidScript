#include "AvidScriptRuntimeSession.h"
#include "Engine/World.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"

#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeSessionPrivate.h"
#include "Session/AvidScriptRuntimeExecutionDomain.h"
#include "UObject/UnrealType.h"

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
EAvidScriptGeneratedNativeScalar ResolveNativeScalar(const FProperty* Property)
{
	if (CastField<FIntProperty>(Property) != nullptr
		|| CastField<FUInt32Property>(Property) != nullptr)
	{
		return EAvidScriptGeneratedNativeScalar::I32;
	}
	if (CastField<FFloatProperty>(Property) != nullptr)
	{
		return EAvidScriptGeneratedNativeScalar::F32;
	}
	if (CastField<FBoolProperty>(Property) != nullptr)
	{
		return EAvidScriptGeneratedNativeScalar::Bool;
	}
	if (CastField<FInt64Property>(Property) != nullptr
		|| CastField<FUInt64Property>(Property) != nullptr)
	{
		return EAvidScriptGeneratedNativeScalar::I64;
	}
	if (CastField<FDoubleProperty>(Property) != nullptr)
	{
		return EAvidScriptGeneratedNativeScalar::F64;
	}
	return EAvidScriptGeneratedNativeScalar::Invalid;
}

uint32 NativeScalarCellCount(const EAvidScriptGeneratedNativeScalar Kind)
{
	switch (Kind)
	{
	case EAvidScriptGeneratedNativeScalar::I32:
	case EAvidScriptGeneratedNativeScalar::F32:
	case EAvidScriptGeneratedNativeScalar::Bool:
		return 1;
	case EAvidScriptGeneratedNativeScalar::I64:
	case EAvidScriptGeneratedNativeScalar::F64:
		return 2;
	default:
		return 0;
	}
}

bool PrepareNativeCallShape(
	const FAvidScriptGeneratedMemberPlan& Member,
	FAvidScriptGeneratedPreparedMemberRoute& Route)
{
	if (Member.Kind != EAvidScriptGeneratedMemberKind::Function)
	{
		return false;
	}
	const FAvidScriptContextualExportCall& Call = Route.Call;
	if (Member.Function == nullptr)
	{
		if (!Member.bLifecycle || Call.GetResultCellCount() != 0)
		{
			return false;
		}
		Route.Result = EAvidScriptGeneratedNativeScalar::Void;
		if (Call.GetParameterCellCount() == 2)
		{
			return Member.StableMemberId.EndsWith(TEXT("():void"), ESearchCase::CaseSensitive);
		}
		if (Call.GetParameterCellCount() == 3
			&& Member.StableMemberId.EndsWith(TEXT(".Tick(float32):void"), ESearchCase::CaseSensitive))
		{
			Route.Parameters.Add(EAvidScriptGeneratedNativeScalar::F32);
			return true;
		}
		return false;
	}

	uint32 ParameterCells = 2;
	EAvidScriptGeneratedNativeScalar ReturnKind = EAvidScriptGeneratedNativeScalar::Void;
	for (TFieldIterator<FProperty> Iterator(Member.Function); Iterator; ++Iterator)
	{
	const FProperty* const Property = *Iterator;
		if (!Property->HasAnyPropertyFlags(CPF_Parm))
		{
			continue;
		}
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			ReturnKind = ResolveNativeScalar(Property);
			if (ReturnKind == EAvidScriptGeneratedNativeScalar::Invalid)
			{
				return false;
			}
			continue;
		}
		if (Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
		{
			return false;
		}
		const EAvidScriptGeneratedNativeScalar Kind = ResolveNativeScalar(Property);
		if (Kind == EAvidScriptGeneratedNativeScalar::Invalid)
		{
			return false;
		}
		ParameterCells += NativeScalarCellCount(Kind);
		if (ParameterCells > FAvidScriptVmCallFrame::MaxCells)
		{
			return false;
		}
		Route.Parameters.Add(Kind);
	}
	if (ParameterCells != Call.GetParameterCellCount()
		|| NativeScalarCellCount(ReturnKind) != Call.GetResultCellCount())
	{
		return false;
	}
	Route.Result = ReturnKind;
	return true;
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
		Route.Members.SetNum(Type.Members.Num());
		for (const FAvidScriptGeneratedMemberPlan& Member : Type.Members)
		{
			if (Member.Kind != EAvidScriptGeneratedMemberKind::Function)
			{
				continue;
			}
			FAvidScriptGeneratedPreparedMemberRoute& Prepared =
				Route.Members[Member.MemberOrdinal];
			FAvidScriptContextualExportCall& Call = Prepared.Call;
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
			if (!PrepareNativeCallShape(Member, Prepared))
			{
				OutError = FString::Printf(
					TEXT("stable_member_id=%s; export=%s; native call shape is unsupported or mismatched"),
					*Member.StableMemberId,
					*Member.ExportName);
				OutRoutes.Reset();
				return false;
			}
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
	if (!Route.bEnabled || !Route.Members.IsValidIndex(static_cast<int32>(MemberOrdinal)))
	{
		return false;
	}

	const FAvidScriptGeneratedPreparedMemberRoute& Prepared = Route.Members[MemberOrdinal];
	const FAvidScriptContextualExportCall& Call = Prepared.Call;
	const EAvidScriptGeneratedNativeScalar ResultKind = Prepared.Result;
	if (!Call.IsValid() || ResultKind == EAvidScriptGeneratedNativeScalar::Invalid
		|| Arguments.Num() != Prepared.Parameters.Num()
		|| (Result != nullptr) != (ResultKind != EAvidScriptGeneratedNativeScalar::Void))
	{
		return false;
	}

	FAvidScriptVmCallFrame Frame;
	Frame.Cells[0] = ReceiverHandle.Slot;
	Frame.Cells[1] = ReceiverHandle.Generation;
	Frame.CellCount = 2;
	for (int32 Index = 0; Index < Arguments.Num(); ++Index)
	{
		const EAvidScriptGeneratedNativeScalar Kind = Prepared.Parameters[Index];
		const uint32 CellCount = NativeScalarCellCount(Kind);
		if (Arguments[Index].Data == nullptr || CellCount == 0
			|| Frame.CellCount + CellCount > FAvidScriptVmCallFrame::MaxCells)
		{
			return false;
		}
		if (Kind == EAvidScriptGeneratedNativeScalar::Bool)
		{
			Frame.Cells[Frame.CellCount] = *static_cast<const bool*>(Arguments[Index].Data) ? 1u : 0u;
		}
		else
		{
			FMemory::Memcpy(&Frame.Cells[Frame.CellCount],
				Arguments[Index].Data, CellCount * sizeof(uint32));
		}
		Frame.CellCount += CellCount;
	}
	if (Frame.CellCount != Call.GetParameterCellCount()) return false;

	FAvidScriptVmCallResult CallResult;
	FAvidScriptVmError Error;
	const uint32 ResultCells = NativeScalarCellCount(ResultKind);
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
			ResultCells != 0 ? &CallResult : nullptr);
		bResultValid = bCalled
			&& (ResultCells == 0 || CallResult.CellCount == ResultCells);
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
	if (!bResultValid) return false;
	if (ResultCells != 0)
	{
		if (ResultKind == EAvidScriptGeneratedNativeScalar::Bool)
		{
			*static_cast<bool*>(Result) = CallResult.Cells[0] != 0;
		}
		else
		{
			FMemory::Memcpy(Result, CallResult.Cells, ResultCells * sizeof(uint32));
		}
	}
	return true;
}
