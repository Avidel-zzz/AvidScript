#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectRegistry.h"

#include "AvidScriptBindingsTestTypes.h"

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
class FAvidScriptPreparedDelegateGuestMemory final
	: public IAvidScriptVmGuestMemory
{
public:
	FAvidScriptPreparedDelegateGuestMemory()
	{
		Bytes.SetNumZeroed(128);
	}

	void StoreInt32(const uint32 GuestAddress, const int32 Value)
	{
		check(GuestAddress + sizeof(Value) <= static_cast<uint32>(Bytes.Num()));
		FMemory::Memcpy(Bytes.GetData() + GuestAddress, &Value, sizeof(Value));
	}

	bool ReadBytes(
		const uint32 GuestAddress,
		TArrayView<uint8> OutBytes,
		FString& OutError) override
	{
		if (!IsRangeValid(GuestAddress, OutBytes.Num()))
		{
			OutError = TEXT("prepared delegate guest read range");
			return false;
		}
		FMemory::Memcpy(
			OutBytes.GetData(),
			Bytes.GetData() + GuestAddress,
			OutBytes.Num());
		OutError.Reset();
		return true;
	}

	bool WriteBytes(
		const uint32 GuestAddress,
		const TConstArrayView<uint8> InBytes,
		FString& OutError) override
	{
		if (!IsRangeValid(GuestAddress, InBytes.Num()))
		{
			OutError = TEXT("prepared delegate guest write range");
			return false;
		}
		FMemory::Memcpy(
			Bytes.GetData() + GuestAddress,
			InBytes.GetData(),
			InBytes.Num());
		OutError.Reset();
		return true;
	}

	bool BorrowReadOnlyBytes(
		const uint32 GuestAddress,
		const uint32 Count,
		const uint32 Alignment,
		TConstArrayView<uint8>& OutBytes,
		FString& OutError) override
	{
		if (!IsBorrowValid(GuestAddress, Count, Alignment))
		{
			OutBytes = TConstArrayView<uint8>();
			OutError = TEXT("prepared delegate guest borrow range");
			return false;
		}
		OutBytes = MakeArrayView(Bytes).Slice(GuestAddress, Count);
		OutError.Reset();
		return true;
	}

	bool BorrowMutableBytes(
		const uint32 GuestAddress,
		const uint32 Count,
		const uint32 Alignment,
		TArrayView<uint8>& OutBytes,
		FString& OutError) override
	{
		if (!IsBorrowValid(GuestAddress, Count, Alignment))
		{
			OutBytes = TArrayView<uint8>();
			OutError = TEXT("prepared delegate guest mutable borrow range");
			return false;
		}
		OutBytes = MakeArrayView(Bytes).Slice(GuestAddress, Count);
		OutError.Reset();
		return true;
	}

private:
	bool IsRangeValid(const uint32 GuestAddress, const int32 Count) const
	{
		return Count >= 0
			&& GuestAddress <= static_cast<uint32>(Bytes.Num())
			&& static_cast<uint32>(Count)
				<= static_cast<uint32>(Bytes.Num()) - GuestAddress;
	}

	bool IsBorrowValid(
		const uint32 GuestAddress,
		const uint32 Count,
		const uint32 Alignment) const
	{
		return Alignment > 0
			&& FMath::IsPowerOfTwo(Alignment)
			&& GuestAddress % Alignment == 0
			&& GuestAddress <= static_cast<uint32>(Bytes.Num())
			&& Count <= static_cast<uint32>(Bytes.Num()) - GuestAddress;
	}

	TArray<uint8> Bytes;
};

template <typename TProperty>
TProperty* FindPreparedDelegatePropertyChecked(
	const UStruct* Owner,
	const FName Name)
{
	TProperty* const Property = FindFProperty<TProperty>(Owner, Name);
	check(Property != nullptr);
	return Property;
}

FAvidScriptBindingTypeModel MakeDelegateLeafType(
	const FString& CanonicalType,
	const FString& Kind,
	const FString& CppType,
	const int32 Size,
	const int32 Alignment,
	TArray<FString> AbiTypes)
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = CanonicalType;
	Type.Kind = Kind;
	Type.CppType = CppType;
	Type.Size = Size;
	Type.Alignment = Alignment;
	Type.AbiTypes = MoveTemp(AbiTypes);
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		Type.CanonicalType,
		Type.EnumValues);
	return Type;
}

void FinalizeDelegateStructType(FAvidScriptBindingTypeModel& Type)
{
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		Type.CanonicalType,
		Type.EnumValues,
		Type.StructFields,
		Type.Size,
		Type.Alignment);
}

FAvidScriptBindingValueModel MakeDelegateValue(
	const FString& Name,
	const FString& Direction,
	const FAvidScriptBindingTypeModel& Type)
{
	FAvidScriptBindingValueModel Value;
	Value.Name = Name;
	Value.Direction = Direction;
	Value.CanonicalType = Type.CanonicalType;
	Value.TypeId = Type.StableId;
	Value.Kind = Type.Kind;
	Value.CppType = Type.CppType;
	Value.AbiTypes = Type.AbiTypes;
	return Value;
}

FAvidScriptBindingDelegateEventModel MakeDelegateEvent(
	const int32 Ordinal,
	const FString& MemberName,
	const FString& ScriptName,
	TArray<FAvidScriptBindingValueModel> Parameters)
{
	FAvidScriptBindingDelegateEventModel Event;
	Event.Ordinal = Ordinal;
	Event.OwnerClass = UAvidScriptBindingsTestObject::StaticClass()->GetPathName();
	Event.UeMember = MemberName;
	Event.ScriptName = ScriptName;
	Event.Parameters = MoveTemp(Parameters);
	Event.CanonicalIdentity =
		FAvidScriptBindingDescriptorIdentity::MakeDelegateEventCanonicalIdentity(
			Event.OwnerClass,
			Event.UeMember,
			Event.DelegateKind,
			Event.SourceMode,
			Event.Parameters);
	Event.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeDelegateEventStableId(
			Event.OwnerClass,
			Event.UeMember,
			Event.DelegateKind,
			Event.SourceMode,
			Event.Parameters);
	Event.ExportName = TEXT("avid_on_delegate_") + Event.StableId.Left(16);
	return Event;
}

FString GetPreparedDelegateParameterDirection(
	const TCHAR* DelegateMember,
	const TCHAR* ParameterName)
{
	const FMulticastDelegateProperty* DelegateProperty =
		FindPreparedDelegatePropertyChecked<FMulticastDelegateProperty>(
			UAvidScriptBindingsTestObject::StaticClass(),
			DelegateMember);
	const FProperty* Property = FindPreparedDelegatePropertyChecked<FProperty>(
		DelegateProperty->SignatureFunction,
		ParameterName);
	const bool bReference = Property->HasAnyPropertyFlags(CPF_ReferenceParm);
	const bool bOut = Property->HasAnyPropertyFlags(CPF_OutParm);
	const bool bConst = Property->HasAnyPropertyFlags(CPF_ConstParm);
	if (bReference && bOut)
	{
		return bConst ? TEXT("const_ref") : TEXT("ref");
	}
	if (bOut)
	{
		return TEXT("out");
	}
	if (bReference)
	{
		return bConst ? TEXT("const_ref") : TEXT("ref");
	}
	return TEXT("value");
}

void WriteDelegateStringArray(
	const TSharedRef<TJsonWriter<>>& Writer,
	const TCHAR* Name,
	const TArray<FString>& Values)
{
	Writer->WriteArrayStart(Name);
	for (const FString& Value : Values)
	{
		Writer->WriteValue(Value);
	}
	Writer->WriteArrayEnd();
}

void WriteDelegateValue(
	const TSharedRef<TJsonWriter<>>& Writer,
	const FAvidScriptBindingValueModel& Value)
{
	Writer->WriteValue(TEXT("name"), Value.Name);
	Writer->WriteValue(TEXT("direction"), Value.Direction);
	Writer->WriteValue(TEXT("has_default"), false);
	Writer->WriteValue(TEXT("canonical_type"), Value.CanonicalType);
	Writer->WriteValue(TEXT("type_id"), Value.TypeId);
	Writer->WriteValue(TEXT("kind"), Value.Kind);
	Writer->WriteValue(TEXT("cpp_type"), Value.CppType);
	WriteDelegateStringArray(Writer, TEXT("abi_types"), Value.AbiTypes);
}

bool SerializeDelegatePackage(
	FAvidScriptBindingPackageModel& Package,
	FString& OutJson)
{
	Package.SelectionHash =
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
	Package.PackageHash =
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);
	OutJson.Reset();
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), Package.SchemaVersion);
	Writer->WriteValue(TEXT("generator_version"), Package.GeneratorVersion);
	Writer->WriteValue(TEXT("engine_version"), Package.EngineVersion);
	Writer->WriteValue(TEXT("source"), Package.Source);
	Writer->WriteValue(TEXT("package_name"), Package.PackageName);
	Writer->WriteValue(TEXT("package_hash"), Package.PackageHash);
	Writer->WriteValue(TEXT("selection_hash"), Package.SelectionHash);
	Writer->WriteValue(TEXT("self_type_id"), Package.SelfTypeId);

	Writer->WriteArrayStart(TEXT("types"));
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Type.StableId);
		Writer->WriteValue(TEXT("canonical_type"), Type.CanonicalType);
		Writer->WriteValue(TEXT("kind"), Type.Kind);
		Writer->WriteValue(TEXT("cpp_type"), Type.CppType);
		Writer->WriteValue(TEXT("size"), Type.Size);
		Writer->WriteValue(TEXT("alignment"), Type.Alignment);
		WriteDelegateStringArray(Writer, TEXT("abi_types"), Type.AbiTypes);
		Writer->WriteValue(TEXT("object_type_ordinal"), Type.ObjectTypeOrdinal);
		Writer->WriteValue(TEXT("class_path"), Type.ClassPath);
		Writer->WriteValue(TEXT("base_type_id"), Type.BaseTypeId);
		if (Type.Kind == TEXT("enum"))
		{
			Writer->WriteArrayStart(TEXT("enum_values"));
			for (const FAvidScriptBindingEnumValue& EnumValue : Type.EnumValues)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("name"), EnumValue.Name);
				Writer->WriteValue(TEXT("value"), EnumValue.Value);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
		}
		if (Type.Kind == TEXT("struct_wire"))
		{
			Writer->WriteArrayStart(TEXT("fields"));
			for (const FAvidScriptBindingStructFieldModel& Field : Type.StructFields)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("name"), Field.Name);
				Writer->WriteValue(TEXT("type_id"), Field.TypeId);
				Writer->WriteValue(TEXT("wire_offset"), Field.WireOffset);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
		}
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	Writer->WriteArrayStart(TEXT("bindings"));
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("delegate_events"));
	for (const FAvidScriptBindingDelegateEventModel& Event : Package.DelegateEvents)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Event.StableId);
		Writer->WriteValue(TEXT("canonical_identity"), Event.CanonicalIdentity);
		Writer->WriteValue(TEXT("ordinal"), Event.Ordinal);
		Writer->WriteValue(TEXT("owner_class"), Event.OwnerClass);
		Writer->WriteValue(TEXT("ue_member"), Event.UeMember);
		Writer->WriteValue(TEXT("script_name"), Event.ScriptName);
		Writer->WriteValue(TEXT("delegate_kind"), Event.DelegateKind);
		Writer->WriteValue(TEXT("source_mode"), Event.SourceMode);
		Writer->WriteValue(TEXT("export_name"), Event.ExportName);
		Writer->WriteArrayStart(TEXT("parameters"));
		for (const FAvidScriptBindingValueModel& Parameter : Event.Parameters)
		{
			Writer->WriteObjectStart();
			WriteDelegateValue(Writer, Parameter);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("class_references"));
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("object_factories"));
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	return Writer->Close();
}

FAvidScriptBindingPackageModel MakePreparedDelegatePackage()
{
	FAvidScriptBindingPackageModel Package;
	Package.SchemaVersion = 11;
	Package.GeneratorVersion = TEXT("P58.C.BindingsTest");
	Package.EngineVersion =
		FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = TEXT("PreparedDelegateCodecTest");

	FAvidScriptBindingTypeModel BoolType = MakeDelegateLeafType(
		TEXT("scalar:bool"), TEXT("scalar"), TEXT("bool"), 4, 4, { TEXT("i") });
	FAvidScriptBindingTypeModel Int32Type = MakeDelegateLeafType(
		TEXT("scalar:i32"), TEXT("scalar"), TEXT("int32"), 4, 4, { TEXT("i") });
	FAvidScriptBindingTypeModel FloatType = MakeDelegateLeafType(
		TEXT("scalar:f32"), TEXT("scalar"), TEXT("float"), 4, 4, { TEXT("f") });
	FAvidScriptBindingTypeModel Int64Type = MakeDelegateLeafType(
		TEXT("scalar:i64"), TEXT("scalar"), TEXT("int64"), 8, 8, { TEXT("I") });
	FAvidScriptBindingTypeModel DoubleType = MakeDelegateLeafType(
		TEXT("scalar:f64"), TEXT("scalar"), TEXT("double"), 8, 8, { TEXT("F") });

	const UEnum* ModeEnum = StaticEnum<EAvidScriptBindingsStructMode>();
	FAvidScriptBindingTypeModel EnumType;
	EnumType.CanonicalType = TEXT("enum:") + ModeEnum->GetPathName();
	EnumType.Kind = TEXT("enum");
	EnumType.CppType = TEXT("EAvidScriptBindingsStructMode");
	EnumType.Size = 4;
	EnumType.Alignment = 4;
	EnumType.AbiTypes = { TEXT("i") };
	for (int32 Index = 0; Index < ModeEnum->NumEnums(); ++Index)
	{
		const FString Name = ModeEnum->GetNameStringByIndex(Index);
		bool bHidden = false;
#if WITH_METADATA
		bHidden = ModeEnum->HasMetaData(TEXT("Hidden"), Index);
#endif
		if (!Name.IsEmpty()
			&& !bHidden
			&& !Name.EndsWith(TEXT("_MAX"), ESearchCase::CaseSensitive))
		{
			EnumType.EnumValues.Add({ Name, ModeEnum->GetValueByIndex(Index) });
		}
	}
	EnumType.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		EnumType.CanonicalType,
		EnumType.EnumValues);

	FAvidScriptBindingTypeModel ObjectType = MakeDelegateLeafType(
		TEXT("object:/Script/CoreUObject.Object"),
		TEXT("object_handle"),
		TEXT("UObject*"),
		8,
		4,
		{ TEXT("i"), TEXT("i") });
	ObjectType.ObjectTypeOrdinal = 0;
	ObjectType.ClassPath = TEXT("/Script/CoreUObject.Object");
	ObjectType.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		ObjectType.CanonicalType,
		ObjectType.EnumValues);

	FAvidScriptBindingTypeModel NestedType;
	NestedType.CanonicalType = TEXT("struct_wire:")
		+ FAvidScriptBindingsNestedStruct::StaticStruct()->GetPathName();
	NestedType.Kind = TEXT("struct_wire");
	NestedType.CppType = TEXT("FAvidScriptBindingsNestedStruct");
	NestedType.Size = 8;
	NestedType.Alignment = 4;
	NestedType.AbiTypes = { TEXT("i") };
	NestedType.StructFields = {
		{ TEXT("Count"), Int32Type.StableId, 0 },
		{ TEXT("Ratio"), FloatType.StableId, 4 }
	};
	FinalizeDelegateStructType(NestedType);

	FAvidScriptBindingTypeModel PayloadType;
	PayloadType.CanonicalType = TEXT("struct_wire:")
		+ FAvidScriptBindingsDelegatePayload::StaticStruct()->GetPathName();
	PayloadType.Kind = TEXT("struct_wire");
	PayloadType.CppType = TEXT("FAvidScriptBindingsDelegatePayload");
	PayloadType.Size = 16;
	PayloadType.Alignment = 4;
	PayloadType.AbiTypes = { TEXT("i") };
	PayloadType.StructFields = {
		{ TEXT("bEnabled"), BoolType.StableId, 0 },
		{ TEXT("Mode"), EnumType.StableId, 4 },
		{ TEXT("Nested"), NestedType.StableId, 8 }
	};
	FinalizeDelegateStructType(PayloadType);

	Package.Types = {
		BoolType,
		Int32Type,
		FloatType,
		Int64Type,
		DoubleType,
		EnumType,
		ObjectType,
		NestedType,
		PayloadType
	};
	Package.DelegateEvents.Add(MakeDelegateEvent(
		0,
		TEXT("PreparedDelegate"),
		TEXT("Prepared"),
		{
			MakeDelegateValue(
				TEXT("Target"),
				GetPreparedDelegateParameterDirection(TEXT("PreparedDelegate"), TEXT("Target")),
				ObjectType),
			MakeDelegateValue(
				TEXT("Payload"),
				GetPreparedDelegateParameterDirection(TEXT("PreparedDelegate"), TEXT("Payload")),
				PayloadType),
			MakeDelegateValue(
				TEXT("Sequence"),
				GetPreparedDelegateParameterDirection(TEXT("PreparedDelegate"), TEXT("Sequence")),
				Int64Type)
		}));
	Package.DelegateEvents.Add(MakeDelegateEvent(
		1,
		TEXT("PreparedWideDelegate"),
		TEXT("PreparedWide"),
		{
			MakeDelegateValue(
				TEXT("Precision"),
				GetPreparedDelegateParameterDirection(TEXT("PreparedWideDelegate"), TEXT("Precision")),
				DoubleType),
			MakeDelegateValue(
				TEXT("Token"),
				GetPreparedDelegateParameterDirection(TEXT("PreparedWideDelegate"), TEXT("Token")),
				Int64Type)
		}));
	Package.DelegateEvents.Add(MakeDelegateEvent(
		2,
		TEXT("PreparedOutputDelegate"),
		TEXT("PreparedOutput"),
		{
			MakeDelegateValue(
				TEXT("Value"),
				GetPreparedDelegateParameterDirection(TEXT("PreparedOutputDelegate"), TEXT("Value")),
				Int32Type),
			MakeDelegateValue(
				TEXT("Result"),
				GetPreparedDelegateParameterDirection(TEXT("PreparedOutputDelegate"), TEXT("Result")),
				Int32Type)
		}));
	return Package;
}

void SetPreparedDelegateObject(
	UFunction& Signature,
	void* Parameters,
	const TCHAR* Name,
	UObject* Object)
{
	FindPreparedDelegatePropertyChecked<FObjectPropertyBase>(&Signature, Name)
		->SetObjectPropertyValue_InContainer(Parameters, Object);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPreparedDelegateCodecTest,
	"AvidScript.Bindings.Delegate.PreparedCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPreparedDelegateCodecTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingPackageModel Model = MakePreparedDelegatePackage();
	FString DescriptorJson;
	if (!TestTrue(
			TEXT("Prepared delegate descriptor serializes"),
			SerializeDelegatePackage(Model, DescriptorJson)))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Prepared delegate descriptor loads"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				Package,
				LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	TArray<FAvidScriptPreparedDelegateEvent> Events;
	FString BuildError;
	if (!TestTrue(
			TEXT("Prepared delegate plans publish"),
			Package->BuildPreparedDelegateEvents(Events, BuildError)))
	{
		AddError(BuildError);
		return false;
	}
	TestEqual(TEXT("Three prepared delegate plans"), Events.Num(), 3);
	if (Events.Num() != 3)
	{
		return false;
	}
	TestEqual(TEXT("Recursive payload fills eight cells"), Events[0].Signature.ParameterCellCount, 8u);
	TestEqual(TEXT("Wide payload fills four cells"), Events[1].Signature.ParameterCellCount, 4u);
	TestEqual(TEXT("Ref/out payload prepends one transaction cell"), Events[2].Signature.ParameterCellCount, 2u);
	TestEqual(TEXT("Ref/out payload exposes two output ordinals"), Events[2].Signature.OutputValueCount, 2u);
	TestNotNull(TEXT("Delegate property is resolved at load"), Events[0].Signature.MulticastProperty);
	TestNotNull(TEXT("Delegate signature is resolved at load"), Events[0].Signature.SignatureFunction);
	TestNotNull(TEXT("Delegate codec is immutable and published"), Events[0].Signature.ImmutableCodecIdentity);
	TestNotNull(TEXT("Delegate encode thunk is published"), Events[0].Signature.Encode);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	UObject* Target = NewObject<UAvidScriptBindingsTestObject>();
	FStructOnScope NativeParameters(Events[0].Signature.SignatureFunction);
	void* NativeFrame = NativeParameters.GetStructMemory();
	SetPreparedDelegateObject(
		*Events[0].Signature.SignatureFunction,
		NativeFrame,
		TEXT("Target"),
		Target);
	FAvidScriptBindingsDelegatePayload Payload;
	Payload.bEnabled = true;
	Payload.Mode = EAvidScriptBindingsStructMode::Secondary;
	Payload.Nested.Count = -17;
	Payload.Nested.Ratio = 3.25f;
	*FindPreparedDelegatePropertyChecked<FStructProperty>(
		Events[0].Signature.SignatureFunction,
		TEXT("Payload"))->ContainerPtrToValuePtr<FAvidScriptBindingsDelegatePayload>(
		NativeFrame) = Payload;
	const int64 Sequence = -0x102030405060708LL;
	FindPreparedDelegatePropertyChecked<FInt64Property>(
		Events[0].Signature.SignatureFunction,
		TEXT("Sequence"))->SetPropertyValue_InContainer(NativeFrame, Sequence);

	FAvidScriptVmCallFrame Frame;
	TArray<FAvidScriptObjectHandle> BorrowedHandles;
	FString ErrorCategory;
	FString ErrorDetails;
	if (!TestTrue(
			TEXT("Prepared delegate frame encodes"),
			Events[0].Signature.Encode(
				Events[0].Signature.ImmutableCodecIdentity,
				NativeFrame,
				Context,
				0,
				Frame,
				BorrowedHandles,
				ErrorCategory,
				ErrorDetails)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorDetails);
		return false;
	}
	TestEqual(TEXT("Encoded frame has eight cells"), Frame.CellCount, 8u);
	TestEqual(TEXT("One UObject lease is returned"), BorrowedHandles.Num(), 1);
	if (BorrowedHandles.Num() == 1)
	{
		TestEqual(TEXT("Object slot cell"), Frame.Cells[0], BorrowedHandles[0].Slot);
		TestEqual(TEXT("Object generation cell"), Frame.Cells[1], BorrowedHandles[0].Generation);
	}
	TestEqual(TEXT("Bool cell"), Frame.Cells[2], 1u);
	TestEqual(
		TEXT("Enum cell"),
		Frame.Cells[3],
		static_cast<uint32>(EAvidScriptBindingsStructMode::Secondary));
	TestEqual(TEXT("Nested int cell"), Frame.Cells[4], static_cast<uint32>(-17));
	uint32 RatioBits = 0;
	FMemory::Memcpy(&RatioBits, &Payload.Nested.Ratio, sizeof(RatioBits));
	TestEqual(TEXT("Nested float cell"), Frame.Cells[5], RatioBits);
	uint32 SequenceCells[2] = {};
	FMemory::Memcpy(SequenceCells, &Sequence, sizeof(Sequence));
	TestEqual(TEXT("i64 low cell"), Frame.Cells[6], SequenceCells[0]);
	TestEqual(TEXT("i64 high cell"), Frame.Cells[7], SequenceCells[1]);

	for (const FAvidScriptObjectHandle& Handle : BorrowedHandles)
	{
		FAvidScriptObjectHandleResult ReleaseResult;
		TestTrue(
			TEXT("Runtime can release every returned delegate lease"),
			Registry.ReleaseBorrowedHandle(Handle, ReleaseResult, false));
	}
	TestEqual(TEXT("Delegate leases leave no live registry slot"), Registry.GetLiveHandleCount(), 0);

	FStructOnScope WideParameters(Events[1].Signature.SignatureFunction);
	void* WideFrame = WideParameters.GetStructMemory();
	const double Precision = -1234.5;
	const int64 Token = 0x7edcba9876543210LL;
	FindPreparedDelegatePropertyChecked<FDoubleProperty>(
		Events[1].Signature.SignatureFunction,
		TEXT("Precision"))->SetPropertyValue_InContainer(WideFrame, Precision);
	FindPreparedDelegatePropertyChecked<FInt64Property>(
		Events[1].Signature.SignatureFunction,
		TEXT("Token"))->SetPropertyValue_InContainer(WideFrame, Token);
	if (!TestTrue(
			TEXT("Prepared f64/i64 frame encodes"),
			Events[1].Signature.Encode(
				Events[1].Signature.ImmutableCodecIdentity,
				WideFrame,
				Context,
				0,
				Frame,
				BorrowedHandles,
				ErrorCategory,
				ErrorDetails)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorDetails);
		return false;
	}
	uint32 ExpectedWideCells[4] = {};
	FMemory::Memcpy(ExpectedWideCells, &Precision, sizeof(Precision));
	FMemory::Memcpy(ExpectedWideCells + 2, &Token, sizeof(Token));
	for (uint32 Index = 0; Index < 4; ++Index)
	{
		TestEqual(TEXT("Wide scalar cell"), Frame.Cells[Index], ExpectedWideCells[Index]);
	}

	TestEqual(
		TEXT("UPARAM(ref) remains a ref contract"),
		Model.DelegateEvents[2].Parameters[0].Direction,
		FString(TEXT("ref")));
	TestEqual(
		TEXT("Plain non-const reference remains an out contract"),
		Model.DelegateEvents[2].Parameters[1].Direction,
		FString(TEXT("out")));
	FStructOnScope OutputParameters(Events[2].Signature.SignatureFunction);
	void* OutputFrame = OutputParameters.GetStructMemory();
	FIntProperty* const ValueProperty =
		FindPreparedDelegatePropertyChecked<FIntProperty>(
			Events[2].Signature.SignatureFunction,
			TEXT("Value"));
	FIntProperty* const ResultProperty =
		FindPreparedDelegatePropertyChecked<FIntProperty>(
			Events[2].Signature.SignatureFunction,
			TEXT("Result"));
	ValueProperty->SetPropertyValue_InContainer(OutputFrame, 41);
	ResultProperty->SetPropertyValue_InContainer(OutputFrame, -7);

	TUniquePtr<FAvidScriptPreparedDelegateOutputTransaction> OutputTransaction;
	if (!TestTrue(
			TEXT("Prepared ref/out transaction initializes"),
			Package->BeginPreparedDelegateOutputTransaction(
				Events[2],
				OutputFrame,
				OutputTransaction,
				ErrorDetails)))
	{
		AddError(ErrorDetails);
		return false;
	}
	constexpr uint32 OutputTransactionToken = 0x80000001u;
	if (!TestTrue(
			TEXT("Prepared ref/out frame encodes"),
			Events[2].Signature.Encode(
				Events[2].Signature.ImmutableCodecIdentity,
				OutputFrame,
				Context,
				OutputTransactionToken,
				Frame,
				BorrowedHandles,
				ErrorCategory,
				ErrorDetails)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorDetails);
		return false;
	}
	TestEqual(TEXT("Output transaction token is the first ABI cell"), Frame.Cells[0], OutputTransactionToken);
	TestEqual(TEXT("Ref input is encoded after the token"), Frame.Cells[1], 41u);

	FAvidScriptPreparedDelegateGuestMemory GuestMemory;
	GuestMemory.StoreInt32(16, 101);
	GuestMemory.StoreInt32(32, 202);
	TestTrue(
		TEXT("Ref output stages into transaction-owned memory"),
		OutputTransaction->StageOutput(
			0,
			16,
			GuestMemory,
			Context,
			ErrorDetails));
	TestEqual(
		TEXT("Staging one output does not mutate the native ref value"),
		ValueProperty->GetPropertyValue_InContainer(OutputFrame),
		41);
	TestFalse(
		TEXT("Duplicate output staging fails closed"),
		OutputTransaction->StageOutput(
			0,
			16,
			GuestMemory,
			Context,
			ErrorDetails));
	TestTrue(
		TEXT("Out value stages into transaction-owned memory"),
		OutputTransaction->StageOutput(
			1,
			32,
			GuestMemory,
			Context,
			ErrorDetails));
	TestTrue(TEXT("All prepared outputs are staged"), OutputTransaction->IsComplete());
	TestTrue(TEXT("Complete output transaction commits"), OutputTransaction->Commit(ErrorDetails));
	TestEqual(
		TEXT("Commit atomically updates the native ref value"),
		ValueProperty->GetPropertyValue_InContainer(OutputFrame),
		101);
	TestEqual(
		TEXT("Commit atomically updates the native out value"),
		ResultProperty->GetPropertyValue_InContainer(OutputFrame),
		202);

	FStructOnScope IncompleteParameters(Events[2].Signature.SignatureFunction);
	void* IncompleteFrame = IncompleteParameters.GetStructMemory();
	ValueProperty->SetPropertyValue_InContainer(IncompleteFrame, 5);
	ResultProperty->SetPropertyValue_InContainer(IncompleteFrame, 6);
	TUniquePtr<FAvidScriptPreparedDelegateOutputTransaction> IncompleteTransaction;
	TestTrue(
		TEXT("Second output transaction initializes"),
		Package->BeginPreparedDelegateOutputTransaction(
			Events[2],
			IncompleteFrame,
			IncompleteTransaction,
			ErrorDetails));
	GuestMemory.StoreInt32(48, 303);
	TestTrue(
		TEXT("Incomplete transaction stages one output"),
		IncompleteTransaction->StageOutput(
			0,
			48,
			GuestMemory,
			Context,
			ErrorDetails));
	TestFalse(
		TEXT("Incomplete output transaction cannot commit"),
		IncompleteTransaction->Commit(ErrorDetails));
	TestEqual(
		TEXT("Failed commit preserves the original ref value"),
		ValueProperty->GetPropertyValue_InContainer(IncompleteFrame),
		5);
	TestEqual(
		TEXT("Failed commit preserves the original out value"),
		ResultProperty->GetPropertyValue_InContainer(IncompleteFrame),
		6);

	FAvidScriptBindingPackageModel UnsupportedModel = MakePreparedDelegatePackage();
	FAvidScriptBindingTypeModel StringType = MakeDelegateLeafType(
		TEXT("string:fstring"),
		TEXT("string_utf8"),
		TEXT("FString"),
		4,
		4,
		{ TEXT("i") });
	UnsupportedModel.Types.Add(StringType);
	UnsupportedModel.DelegateEvents.Reset();
	UnsupportedModel.DelegateEvents.Add(MakeDelegateEvent(
		0,
		TEXT("UnsupportedDelegate"),
		TEXT("Unsupported"),
		{ MakeDelegateValue(
			TEXT("Text"),
			GetPreparedDelegateParameterDirection(
				TEXT("UnsupportedDelegate"),
				TEXT("Text")),
			StringType) }));
	FString UnsupportedJson;
	TestTrue(
		TEXT("Unsupported delegate descriptor serializes"),
		SerializeDelegatePackage(UnsupportedModel, UnsupportedJson));
	TSharedPtr<const FAvidScriptBindingPackage> UnsupportedPackage;
	FAvidScriptBindingPackageLoadResult UnsupportedResult;
	TestFalse(
		TEXT("String delegate parameters fail closed during package load"),
		FAvidScriptBindingPackage::LoadDescriptor(
			UnsupportedJson,
			UnsupportedPackage,
			UnsupportedResult));
	TestEqual(
		TEXT("Unsupported delegate category is stable"),
		UnsupportedResult.ErrorCategory,
		FString(TEXT("binding_delegate_parameter_unsupported")));
	return true;
}

#endif
