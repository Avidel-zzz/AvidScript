#include "ScriptTypes/AvidScriptGeneratedTypeHostBindings.h"
#include "ScriptTypes/AvidScriptGeneratedTypeAuthority.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "AvidScriptWasmModuleLayout.h"
#include "UObject/UnrealType.h"

namespace
{
EAvidScriptVmTypedHostStatus ResolveGeneratedReceiverType(void* OpaqueContext, const int64 PackedSelf, int32& OutType)
{
	OutType = 0;
	const auto* Context = static_cast<const FAvidScriptGeneratedReceiverHostContext*>(OpaqueContext);
	uint32 Ordinal = 0;
	if (!Context || !Context->Runtime || !Context->Registry
		|| !Context->Runtime->ResolveGeneratedReceiverType(PackedSelf, *Context->Registry, Ordinal)
		|| Ordinal >= MAX_int32) return EAvidScriptVmTypedHostStatus::Rejected;
	OutType = static_cast<int32>(Ordinal + 1);
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

EAvidScriptVmTypedHostStatus RequireGeneratedReceiver(void* OpaqueContext, const int64 PackedSelf, int32& OutValid)
{
	OutValid = 0;
	const auto* Context = static_cast<const FAvidScriptGeneratedReceiverHostContext*>(OpaqueContext);
	if (Context == nullptr || Context->Runtime == nullptr || Context->Registry == nullptr
		|| Context->Runtime->ResolveGeneratedTypeReceiver(PackedSelf, Context->TypeOrdinal, *Context->Registry) == nullptr) return EAvidScriptVmTypedHostStatus::Rejected;
	OutValid = 1;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

bool ResolveGeneratedPropertyReceiver(
	FAvidScriptGeneratedPropertyHostContext& Context,
	const int64 PackedSelf,
	UObject*& OutReceiver)
{
	OutReceiver = nullptr;
	if (Context.Runtime == nullptr || Context.Registry == nullptr
		|| Context.ExpectedClass == nullptr || Context.Property == nullptr) return false;
	UObject* Receiver = Context.Runtime->ResolveGeneratedTypeReceiver(PackedSelf, Context.TypeOrdinal, *Context.Registry);
	if (Receiver == nullptr || !Receiver->IsA(Context.ExpectedClass)) return false;
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

}

UObject* FAvidScriptWasmRuntimeInstance::ResolveGeneratedTypeReceiver(
	const int64 PackedSelf, const uint32 TypeOrdinal, const FAvidScriptGeneratedTypeRegistrySnapshot& Registry) const
{
	if (!IsInGameThread() || HostContext.OwnerHandle.ToUInt64() != static_cast<uint64>(PackedSelf)) return nullptr;
	const TSharedPtr<IAvidScriptGeneratedTypeAuthority> Authority = HostContext.GeneratedTypeAuthority.Pin();
	return Authority ? Authority->ResolveGeneratedTypeReceiver(PackedSelf, TypeOrdinal, Registry) : nullptr;
}

bool FAvidScriptWasmRuntimeInstance::ResolveGeneratedReceiverType(const int64 PackedSelf,
	const FAvidScriptGeneratedTypeRegistrySnapshot& Registry, uint32& OutOrdinal)
{
	OutOrdinal = 0;
	if (!IsInGameThread() || !IsContextInvocationActive()) return false;
	const auto Authority = HostContext.GeneratedTypeAuthority.Pin();
	const uint64 Packed = static_cast<uint64>(PackedSelf);
	const FAvidScriptObjectHandle Target{static_cast<uint32>(Packed), static_cast<uint32>(Packed >> 32)};
	FAvidScriptObjectHandleResult Resolve;
	UObject* Object = HostContext.ObjectRegistry ? HostContext.ObjectRegistry->ResolveObject(Target, Resolve, false) : nullptr;
	if (!Object) return false;
	FAvidScriptVmError Error;
	if (!Authority || !Authority->ResolveInstanceTypeOrdinal(*this, Target, Registry, OutOrdinal, Error)) return false;
	const auto* Type = Registry.FindTypeByOrdinal(OutOrdinal);
	return Type && Type->Class && Object->IsA(Type->Class);
}

bool FAvidScriptWasmRuntimeInstance::ConfigureGeneratedTypeHostBindings(
	const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
	TArray<FAvidScriptVmTypedHostImport>& OutImports, FString& OutError, TConstArrayView<uint8> CanonicalWasm)
{
	OutImports.Reset();
	OutError.Reset();
	if (!IsInGameThread() || IsLoaded() || ContextInvocationDepth != 0 || !Registry)
	{
		OutError = TEXT("generated bindings require an unloaded GameThread Runtime and a registry");
		return false;
	}
	TSharedPtr<FAvidScriptGeneratedTypeHostBindings> State = MakeShared<FAvidScriptGeneratedTypeHostBindings>();
	State->Registry = Registry;
	if (!CanonicalWasm.IsEmpty())
	{
		FAvidScriptWasmModuleLayout Layout;
		if (!InspectAvidScriptWasmModuleLayout(CanonicalWasm, Layout, OutError)) return false;
		if (Layout.FunctionImports.ContainsByPredicate([](const auto& Import) {
			return Import.ModuleName == TEXT("avidscript") && Import.ImportName == TEXT("avid_ue_receiver_type_v1"); }))
		{
			auto Context = MakeUnique<FAvidScriptGeneratedReceiverHostContext>();
			Context->Runtime = this; Context->Registry = Registry.Get();
			FAvidScriptVmTypedHostImport Import;
			Import.StableId = TEXT("generated:receiver:type:v1");
			Import.ModuleName = TEXT("avidscript"); Import.ImportName = TEXT("avid_ue_receiver_type_v1");
			Import.Signature = TEXT("(I)i"); Import.Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get;
			Import.bSupplementalRuntimeAuthority = true;
			Import.PreparedTarget.Context = Context.Get();
			Import.PreparedTarget.PackedSelfPropertyI32Get = &::ResolveGeneratedReceiverType;
			State->ReceiverContexts.Add(MoveTemp(Context)); State->HostImports.Add(MoveTemp(Import));
		}
	}
	for (const FAvidScriptGeneratedTypePlan& RegistryType : Registry->GetTypes())
	{
		TUniquePtr<FAvidScriptGeneratedReceiverHostContext> ReceiverContext = MakeUnique<FAvidScriptGeneratedReceiverHostContext>();
		ReceiverContext->Runtime = this;
		ReceiverContext->Registry = Registry.Get();
		ReceiverContext->TypeOrdinal = RegistryType.TypeOrdinal;
		FAvidScriptVmTypedHostImport ReceiverImport;
		ReceiverImport.StableId = RegistryType.StableTypeId + TEXT(":receiver:require:v1");
		ReceiverImport.ModuleName = TEXT("avidscript");
		ReceiverImport.ImportName = FString::Printf(TEXT("avid_ue_receiver_%u_require_v1"), RegistryType.TypeOrdinal);
		ReceiverImport.Signature = TEXT("(I)i");
		ReceiverImport.Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get;
		ReceiverImport.bSupplementalRuntimeAuthority = true;
		ReceiverImport.PreparedTarget.Context = ReceiverContext.Get();
		ReceiverImport.PreparedTarget.PackedSelfPropertyI32Get = &RequireGeneratedReceiver;
		State->ReceiverContexts.Add(MoveTemp(ReceiverContext));
		State->HostImports.Add(MoveTemp(ReceiverImport));
		for (const FAvidScriptGeneratedMemberPlan& Member : RegistryType.Members)
		{
			if (Member.Kind != EAvidScriptGeneratedMemberKind::Property)
			{
				continue;
			}
			TUniquePtr<FAvidScriptGeneratedPropertyHostContext> Context =
				MakeUnique<FAvidScriptGeneratedPropertyHostContext>();
			Context->Runtime = this;
			Context->Registry = Registry.Get();
			Context->TypeOrdinal = RegistryType.TypeOrdinal;
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
				State->HostImports.Add(MakeGeneratedScalarPropertyImport(
					Member,
					Member.GetterImportName,
					false,
					*ContextPointer));
			}
			if (!Member.SetterImportName.IsEmpty())
			{
				State->HostImports.Add(MakeGeneratedScalarPropertyImport(
					Member,
					Member.SetterImportName,
					true,
					*ContextPointer));
			}
		}
	}
	if (!CanonicalWasm.IsEmpty() && !ConfigureAvidScriptGeneratedMethodRoutes(*this, CanonicalWasm, *State, OutError)) return false;
	if (!SetSupplementalTypedHostImports(State->HostImports, OutError)) return false;
	OutImports = State->HostImports;
	GeneratedTypeHostBindings = MoveTemp(State);
	return true;
}
