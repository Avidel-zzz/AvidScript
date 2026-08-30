#include "AvidScriptRuntimeSession.h"

#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeSessionPrivate.h"
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

bool ResolveGeneratedPropertyReceiver(
	FAvidScriptGeneratedPropertyHostContext& Context,
	const int64 PackedSelf,
	UObject*& OutReceiver)
{
	OutReceiver = nullptr;
	const uint64 PackedBits = static_cast<uint64>(PackedSelf);
	const uint32 SelfSlot = static_cast<uint32>(PackedBits);
	const uint32 SelfGeneration = static_cast<uint32>(PackedBits >> 32);
	if (!IsInGameThread()
		|| SelfSlot != Context.ReceiverHandle.Slot
		|| SelfGeneration != Context.ReceiverHandle.Generation
		|| Context.ExpectedClass == nullptr || Context.Property == nullptr)
	{
		return false;
	}
	UObject* const Receiver = Context.Receiver.Get();
	if (Receiver == nullptr || !Receiver->IsA(Context.ExpectedClass)
		|| Receiver->HasAnyFlags(
			RF_ClassDefaultObject | RF_ArchetypeObject
			| RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return false;
	}
	OutReceiver = Receiver;
	return true;
}

template <typename PropertyType>
bool MatchesGeneratedPropertyCodec(FProperty& Property)
{
	return CastField<PropertyType>(&Property) != nullptr;
}

bool ReadGeneratedBoolProperty(FProperty& Property, UObject& Receiver, int32& OutValue)
{
	OutValue = static_cast<FBoolProperty&>(Property).GetPropertyValue_InContainer(&Receiver)
		? 1
		: 0;
	return true;
}

bool WriteGeneratedBoolProperty(FProperty& Property, UObject& Receiver, const int32 Value)
{
	if (Value != 0 && Value != 1)
	{
		return false;
	}
	static_cast<FBoolProperty&>(Property).SetPropertyValue_InContainer(
		&Receiver,
		Value != 0);
	return true;
}

template <typename PropertyType, typename ValueType>
bool ReadGeneratedScalarProperty(
	FProperty& Property,
	UObject& Receiver,
	ValueType& OutValue)
{
	OutValue = static_cast<PropertyType&>(Property).GetPropertyValue_InContainer(&Receiver);
	return true;
}

template <typename PropertyType, typename ValueType>
bool WriteGeneratedScalarProperty(
	FProperty& Property,
	UObject& Receiver,
	const ValueType Value)
{
	static_cast<PropertyType&>(Property).SetPropertyValue_InContainer(&Receiver, Value);
	return true;
}

void ConfigureGeneratedBoolCodec(FAvidScriptGeneratedPropertyHostContext& Context)
{
	Context.ReadI32 = &ReadGeneratedBoolProperty;
	Context.WriteI32 = &WriteGeneratedBoolProperty;
}

template <typename PropertyType, typename ValueType>
void ConfigureGeneratedI32Codec(FAvidScriptGeneratedPropertyHostContext& Context)
{
	Context.ReadI32 = &ReadGeneratedScalarProperty<PropertyType, ValueType>;
	Context.WriteI32 = &WriteGeneratedScalarProperty<PropertyType, ValueType>;
}

template <typename PropertyType, typename ValueType>
void ConfigureGeneratedI64Codec(FAvidScriptGeneratedPropertyHostContext& Context)
{
	Context.ReadI64 = &ReadGeneratedScalarProperty<PropertyType, ValueType>;
	Context.WriteI64 = &WriteGeneratedScalarProperty<PropertyType, ValueType>;
}

template <typename PropertyType, typename ValueType>
void ConfigureGeneratedF32Codec(FAvidScriptGeneratedPropertyHostContext& Context)
{
	Context.ReadF32 = &ReadGeneratedScalarProperty<PropertyType, ValueType>;
	Context.WriteF32 = &WriteGeneratedScalarProperty<PropertyType, ValueType>;
}

template <typename PropertyType, typename ValueType>
void ConfigureGeneratedF64Codec(FAvidScriptGeneratedPropertyHostContext& Context)
{
	Context.ReadF64 = &ReadGeneratedScalarProperty<PropertyType, ValueType>;
	Context.WriteF64 = &WriteGeneratedScalarProperty<PropertyType, ValueType>;
}

struct FGeneratedPropertyCodecDescriptor
{
	bool (*Matches)(FProperty& Property) = nullptr;
	void (*Configure)(FAvidScriptGeneratedPropertyHostContext& Context) = nullptr;
};

bool TryConfigureGeneratedScalarCodec(
	FProperty& Property,
	FAvidScriptGeneratedPropertyHostContext& Context)
{
	static const FGeneratedPropertyCodecDescriptor Codecs[] = {
		{ &MatchesGeneratedPropertyCodec<FBoolProperty>, &ConfigureGeneratedBoolCodec },
		{ &MatchesGeneratedPropertyCodec<FIntProperty>, &ConfigureGeneratedI32Codec<FIntProperty, int32> },
		{ &MatchesGeneratedPropertyCodec<FInt64Property>, &ConfigureGeneratedI64Codec<FInt64Property, int64> },
		{ &MatchesGeneratedPropertyCodec<FFloatProperty>, &ConfigureGeneratedF32Codec<FFloatProperty, float> },
		{ &MatchesGeneratedPropertyCodec<FDoubleProperty>, &ConfigureGeneratedF64Codec<FDoubleProperty, double> },
	};
	for (const FGeneratedPropertyCodecDescriptor& Codec : Codecs)
	{
		if (Codec.Matches(Property))
		{
			Codec.Configure(Context);
			return true;
		}
	}
	return false;
}

template <typename ValueType, auto ReadMember>
EAvidScriptVmTypedHostStatus GetGeneratedScalarProperty(
	void* OpaqueContext,
	const int64 PackedSelf,
	ValueType& OutValue)
{
	FAvidScriptGeneratedPropertyHostContext* const Context =
		static_cast<FAvidScriptGeneratedPropertyHostContext*>(OpaqueContext);
	UObject* Receiver = nullptr;
	if (Context == nullptr || Context->*ReadMember == nullptr
		|| !ResolveGeneratedPropertyReceiver(*Context, PackedSelf, Receiver))
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	const auto Read = Context->*ReadMember;
	return Read(*Context->Property, *Receiver, OutValue)
		? EAvidScriptVmTypedHostStatus::Succeeded
		: EAvidScriptVmTypedHostStatus::Rejected;
}

template <typename ValueType, auto WriteMember>
EAvidScriptVmTypedHostStatus SetGeneratedScalarProperty(
	void* OpaqueContext,
	const int64 PackedSelf,
	const ValueType Value)
{
	FAvidScriptGeneratedPropertyHostContext* const Context =
		static_cast<FAvidScriptGeneratedPropertyHostContext*>(OpaqueContext);
	UObject* Receiver = nullptr;
	if (Context == nullptr || Context->*WriteMember == nullptr
		|| !ResolveGeneratedPropertyReceiver(*Context, PackedSelf, Receiver))
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	const auto Write = Context->*WriteMember;
	return Write(*Context->Property, *Receiver, Value)
		? EAvidScriptVmTypedHostStatus::Succeeded
		: EAvidScriptVmTypedHostStatus::Rejected;
}

FAvidScriptVmTypedHostImport MakeGeneratedScalarPropertyImport(
	const FAvidScriptGeneratedMemberPlan& Member,
	const FString& ImportName,
	const bool bWrite,
	FAvidScriptGeneratedPropertyHostContext& Context)
{
	FAvidScriptVmTypedHostImport Import;
	Import.StableId = Member.StableMemberId
		+ (bWrite ? TEXT(":set") : TEXT(":get"));
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = ImportName;
	Import.bSupplementalRuntimeAuthority = true;
	Import.PreparedTarget.Context = &Context;
	if (Context.ReadI32 != nullptr)
	{
		Import.Signature = bWrite ? TEXT("(Ii)") : TEXT("(I)i");
		Import.Shape = bWrite
			? EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Set
			: EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get;
		if (bWrite)
		{
			Import.PreparedTarget.PackedSelfPropertyI32Set =
				&SetGeneratedScalarProperty<int32, &FAvidScriptGeneratedPropertyHostContext::WriteI32>;
		}
		else
		{
			Import.PreparedTarget.PackedSelfPropertyI32Get =
				&GetGeneratedScalarProperty<int32, &FAvidScriptGeneratedPropertyHostContext::ReadI32>;
		}
	}
	else if (Context.ReadI64 != nullptr)
	{
		Import.Signature = bWrite ? TEXT("(II)") : TEXT("(I)I");
		Import.Shape = bWrite
			? EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Set
			: EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Get;
		if (bWrite)
		{
			Import.PreparedTarget.PackedSelfPropertyI64Set =
				&SetGeneratedScalarProperty<int64, &FAvidScriptGeneratedPropertyHostContext::WriteI64>;
		}
		else
		{
			Import.PreparedTarget.PackedSelfPropertyI64Get =
				&GetGeneratedScalarProperty<int64, &FAvidScriptGeneratedPropertyHostContext::ReadI64>;
		}
	}
	else if (Context.ReadF32 != nullptr)
	{
		Import.Signature = bWrite ? TEXT("(If)") : TEXT("(I)f");
		Import.Shape = bWrite
			? EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Set
			: EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Get;
		if (bWrite)
		{
			Import.PreparedTarget.PackedSelfPropertyF32Set =
				&SetGeneratedScalarProperty<float, &FAvidScriptGeneratedPropertyHostContext::WriteF32>;
		}
		else
		{
			Import.PreparedTarget.PackedSelfPropertyF32Get =
				&GetGeneratedScalarProperty<float, &FAvidScriptGeneratedPropertyHostContext::ReadF32>;
		}
	}
	else
	{
		Import.Signature = bWrite ? TEXT("(Id)") : TEXT("(I)d");
		Import.Shape = bWrite
			? EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Set
			: EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Get;
		if (bWrite)
		{
			Import.PreparedTarget.PackedSelfPropertyF64Set =
				&SetGeneratedScalarProperty<double, &FAvidScriptGeneratedPropertyHostContext::WriteF64>;
		}
		else
		{
			Import.PreparedTarget.PackedSelfPropertyF64Get =
				&GetGeneratedScalarProperty<double, &FAvidScriptGeneratedPropertyHostContext::ReadF64>;
		}
	}
	return Import;
}

EGeneratedCallShape ResolveCallShape(
	const FAvidScriptGeneratedMemberPlan& Member,
	const FAvidScriptVmPreparedExportCall& Call)
{
	if (Member.Kind != EAvidScriptGeneratedMemberKind::Function)
	{
		return EGeneratedCallShape::Unsupported;
	}
	if (Member.Function == nullptr)
	{
		if (!Member.bLifecycle || Call.ResultCellCount != 0)
		{
			return EGeneratedCallShape::Unsupported;
		}
		if (Call.ParameterCellCount == 2)
		{
			return EGeneratedCallShape::ReceiverVoid;
		}
		return Call.ParameterCellCount == 3
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
		&& Call.ParameterCellCount == 2 && Call.ResultCellCount == 0)
	{
		return EGeneratedCallShape::ReceiverVoid;
	}
	if (InputCount == 0 && CastField<FIntProperty>(ReturnProperty) != nullptr
		&& Call.ParameterCellCount == 2 && Call.ResultCellCount == 1)
	{
		return EGeneratedCallShape::ReceiverI32;
	}
	if (InputCount == 1 && CastField<FFloatProperty>(InputProperty) != nullptr
		&& ReturnProperty == nullptr
		&& Call.ParameterCellCount == 3 && Call.ResultCellCount == 0)
	{
		return EGeneratedCallShape::ReceiverF32Void;
	}
	return EGeneratedCallShape::Unsupported;
}
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
	for (const FAvidScriptGeneratedTypePlan& RegistryType : Registry->GetTypes())
	{
		for (const FAvidScriptGeneratedMemberPlan& Member : RegistryType.Members)
		{
			if (Member.Kind != EAvidScriptGeneratedMemberKind::Property)
			{
				continue;
			}
			TUniquePtr<FAvidScriptGeneratedPropertyHostContext> Context =
				MakeUnique<FAvidScriptGeneratedPropertyHostContext>();
			Context->Receiver = &Receiver;
			Context->ReceiverHandle = ReceiverHandle;
			Context->ExpectedClass = RegistryType.Class;
			Context->Property = Member.Property;
			if (Context->Property == nullptr
				|| !TryConfigureGeneratedScalarCodec(*Context->Property, *Context))
			{
				OutError = FString::Printf(
					TEXT("generated property '%s' has no prepared scalar codec"),
					*Member.StableMemberId);
				return false;
			}
			FAvidScriptGeneratedPropertyHostContext* const ContextPointer =
				Context.Get();
			State->PropertyContexts.Add(MoveTemp(Context));
			if (!Member.GetterImportName.IsEmpty())
			{
				State->PropertyImports.Add(MakeGeneratedScalarPropertyImport(
					Member,
					Member.GetterImportName,
					false,
					*ContextPointer));
			}
			if (!Member.SetterImportName.IsEmpty())
			{
				State->PropertyImports.Add(MakeGeneratedScalarPropertyImport(
					Member,
					Member.SetterImportName,
					true,
					*ContextPointer));
			}
		}
	}
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
	return true;
}

bool FAvidScriptRuntimeSession::ClearGeneratedTypeInstance(FString& OutError)
{
	OutError.Reset();
	if (!GeneratedTypeInstance)
	{
		return true;
	}
	if (!IsInGameThread() || IsOperationActive())
	{
		OutError = TEXT("generated type instance teardown requires an idle GameThread session");
		return false;
	}
	if (!GeneratedTypeInstance->Registration.Reset())
	{
		OutError = TEXT("generated type instance router rejected registration teardown");
		return false;
	}
	GeneratedTypeInstance.Reset();
	return true;
}

bool FAvidScriptRuntimeSession::PrepareGeneratedTypeExports(
	FAvidScriptWasmRuntimeInstance& Runtime,
	TArray<FAvidScriptVmPreparedExportCall>& OutCalls,
	TArray<uint8>& OutCallShapes,
	FString& OutError) const
{
	OutCalls.Reset();
	OutCallShapes.Reset();
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

	const FAvidScriptGeneratedTypePlan* const Type =
		GeneratedTypeInstance->Registry->FindTypeByOrdinal(
			GeneratedTypeInstance->TypeOrdinal);
	if (Type == nullptr || Type->Class == nullptr
		|| !GeneratedTypeInstance->Receiver->IsA(Type->Class))
	{
		OutError = TEXT("generated type instance no longer satisfies its registry plan");
		return false;
	}

	OutCalls.SetNum(Type->Members.Num());
	OutCallShapes.SetNumZeroed(Type->Members.Num());
	for (const FAvidScriptGeneratedMemberPlan& Member : Type->Members)
	{
		if (Member.Kind != EAvidScriptGeneratedMemberKind::Function)
		{
			continue;
		}
		FAvidScriptVmPreparedExportCall& Call = OutCalls[Member.MemberOrdinal];
		FString PrepareError;
		if (!Runtime.PrepareNamedExportCall(Member.ExportName, Call, PrepareError))
		{
			OutError = FString::Printf(
				TEXT("stable_member_id=%s; export=%s; %s"),
				*Member.StableMemberId,
				*Member.ExportName,
				PrepareError.IsEmpty() ? TEXT("prepare failed") : *PrepareError);
			OutCalls.Reset();
			OutCallShapes.Reset();
			return false;
		}
		if (Call.ParameterCellCount < 2)
		{
			OutError = FString::Printf(
				TEXT("generated export '%s' omits the packed ObjectHandle receiver"),
				*Member.ExportName);
			OutCalls.Reset();
			OutCallShapes.Reset();
			return false;
		}
		OutCallShapes[Member.MemberOrdinal] = static_cast<uint8>(
			ResolveCallShape(Member, Call));
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
	if (!IsInGameThread() || IsOperationActive() || !IsLiveLoaded()
		|| !GeneratedTypeInstance
		|| GeneratedTypeInstance->Receiver.Get() != &Receiver
		|| GeneratedTypeInstance->ReceiverHandle != ReceiverHandle
		|| GeneratedTypeInstance->TypeOrdinal != TypeOrdinal
		|| !GeneratedTypeInstance->Registry.IsValid()
		|| !GeneratedTypeInstance->PreparedCalls.IsValidIndex(static_cast<int32>(MemberOrdinal))
		|| !GeneratedTypeInstance->CallShapes.IsValidIndex(static_cast<int32>(MemberOrdinal)))
	{
		return false;
	}

	const FAvidScriptGeneratedTypePlan* const Type =
		GeneratedTypeInstance->Registry->FindTypeByOrdinal(TypeOrdinal);
	const FAvidScriptGeneratedMemberPlan* const Member =
		Type != nullptr ? Type->FindMember(MemberOrdinal) : nullptr;
	const FAvidScriptVmPreparedExportCall& Call =
		GeneratedTypeInstance->PreparedCalls[MemberOrdinal];
	const EGeneratedCallShape Shape = static_cast<EGeneratedCallShape>(
		GeneratedTypeInstance->CallShapes[MemberOrdinal]);
	if (Member == nullptr || Member->Kind != EAvidScriptGeneratedMemberKind::Function
		|| !Call.IsValid() || Shape == EGeneratedCallShape::Unsupported)
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
	TGuardValue<int32> GuestCallGuard(ActiveGuestCallDepth, ActiveGuestCallDepth + 1);
	if (Shape == EGeneratedCallShape::ReceiverI32)
	{
		if (Result == nullptr || !Call.Call(Frame, Error, &CallResult)
			|| CallResult.CellCount != 1)
		{
			return false;
		}
		*static_cast<int32*>(Result) = static_cast<int32>(CallResult.Cells[0]);
		return true;
	}
	return Result == nullptr && Call.Call(Frame, Error);
}
