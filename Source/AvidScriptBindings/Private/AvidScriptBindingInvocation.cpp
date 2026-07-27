#include "AvidScriptBindingInvocation.h"

#include "AvidScriptBindingFastPath.h"
#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptHash.h"
#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptObjectTypeBinding.h"
#include "AvidScriptSceneAttachmentBinding.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Containers/StringConv.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
enum class EAvidScriptRuntimeBindingDirection : uint8
{
	Value,
	ConstRef,
	Ref,
	Out,
	Return
};

enum class EAvidScriptRuntimeBindingKind : uint8
{
	Void,
	Bool,
	Int8,
	UInt8,
	Int16,
	UInt16,
	Int32,
	UInt32,
	Int64,
	UInt64,
	Float,
	Double,
	Enum,
	Name,
	Object,
	Vector,
	Rotator,
	Transform
};

struct FAvidScriptRuntimeBindingValuePlan
{
	FProperty* Property = nullptr;
	UClass* ObjectClass = nullptr;
	EAvidScriptRuntimeBindingDirection Direction = EAvidScriptRuntimeBindingDirection::Value;
	EAvidScriptRuntimeBindingKind Kind = EAvidScriptRuntimeBindingKind::Void;
	int32 ArgumentOffset = INDEX_NONE;
	int32 ArgumentWidth = 0;
	int32 GuestStorageSize = 0;
	FString Name;
};

struct FAvidScriptRuntimeBindingInvocationPlan
{
	EAvidScriptBindingInvocationKind Kind = EAvidScriptBindingInvocationKind::ReflectedFunction;
	UClass* OwnerClass = nullptr;
	UFunction* Function = nullptr;
	FProperty* ReflectedProperty = nullptr;
	FString DebugPath;
	bool bStatic = false;
	bool bRequiresWriteAccess = false;
	EAvidScriptBindingReloadEffect ReloadEffect = EAvidScriptBindingReloadEffect::Unsupported;
	bool bRequiresGuestMemory = false;
	int32 FrameSize = 0;
	int32 FrameAlignment = 1;
	int32 RequiredScratchSize = 0;
	int32 ExpectedArgumentCount = 0;
	TArray<FAvidScriptRuntimeBindingValuePlan> Parameters;
	FAvidScriptRuntimeBindingValuePlan ReturnValue;
	UE::AvidScript::BindingPrivate::FFastPathPlan FastPath;
};

void SetAvidScriptBindingLoadFailure(
	FAvidScriptBindingPackageLoadResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& Details)
{
	OutResult = FAvidScriptBindingPackageLoadResult();
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.ErrorDetails = Details;
}

void SetAvidScriptBindingDispatchFailure(
	FAvidScriptDynamicHostCallResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& Details)
{
	OutResult = FAvidScriptDynamicHostCallResult();
	OutResult.Details = FString::Printf(
		TEXT("%s | source=%s | %s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*Details);
}

bool IsAvidScriptRuntimeFunctionAllowed(const UFunction* Function)
{
	return Function != nullptr
		&& Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)
		&& !Function->HasAnyFunctionFlags(
			FUNC_EditorOnly
			| FUNC_Delegate
			| FUNC_MulticastDelegate
			| FUNC_NetRequest
			| FUNC_NetResponse)
		&& !Function->HasMetaData(TEXT("Latent"))
		&& !Function->HasMetaData(TEXT("CustomThunk"));
}

bool IsAvidScriptRuntimePropertyReadable(const FProperty* Property)
{
	return Property != nullptr
		&& Property->HasAnyPropertyFlags(CPF_BlueprintVisible)
		&& !Property->HasAnyPropertyFlags(CPF_Parm | CPF_EditorOnly | CPF_Deprecated);
}

bool IsAvidScriptRuntimePropertyWritable(const FProperty* Property)
{
	const EPropertyFlags RejectedFlags =
		CPF_BlueprintReadOnly
		| CPF_EditConst
		| CPF_Config
		| CPF_GlobalConfig
		| CPF_EditorOnly
		| CPF_Deprecated
		| CPF_InstancedReference
		| CPF_ContainsInstancedReference
		| CPF_Net;
	return IsAvidScriptRuntimePropertyReadable(Property)
		&& !Property->HasAnyPropertyFlags(RejectedFlags);
}

FString GetAvidScriptRuntimePropertyDirection(const FProperty* Property)
{
	if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
	{
		return TEXT("return");
	}
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

bool ParseAvidScriptRuntimeDirection(
	const FString& Direction,
	EAvidScriptRuntimeBindingDirection& OutDirection)
{
	if (Direction == TEXT("value")) { OutDirection = EAvidScriptRuntimeBindingDirection::Value; return true; }
	if (Direction == TEXT("const_ref")) { OutDirection = EAvidScriptRuntimeBindingDirection::ConstRef; return true; }
	if (Direction == TEXT("ref")) { OutDirection = EAvidScriptRuntimeBindingDirection::Ref; return true; }
	if (Direction == TEXT("out")) { OutDirection = EAvidScriptRuntimeBindingDirection::Out; return true; }
	if (Direction == TEXT("return")) { OutDirection = EAvidScriptRuntimeBindingDirection::Return; return true; }
	return false;
}

bool MatchesAvidScriptRuntimeScalarModel(
	const FAvidScriptBindingValueModel& Model,
	const TCHAR* CanonicalType,
	const TCHAR* CppType,
	const TCHAR* AbiType)
{
	return Model.Kind == TEXT("scalar")
		&& Model.CanonicalType == CanonicalType
		&& Model.CppType == CppType
		&& Model.AbiTypes.Num() == 1
		&& Model.AbiTypes[0] == AbiType;
}

bool ResolveAvidScriptRuntimeKind(
	const FProperty* Property,
	const FAvidScriptBindingValueModel& Model,
	const FAvidScriptBindingTypeModel* DeclaredType,
	EAvidScriptRuntimeBindingKind& OutKind,
	UClass*& OutObjectClass)
{
	OutObjectClass = nullptr;
	if (Model.CanonicalType == TEXT("void"))
	{
		OutKind = EAvidScriptRuntimeBindingKind::Void;
		return Property == nullptr
			&& Model.Kind == TEXT("void")
			&& Model.CppType == TEXT("void")
			&& Model.AbiTypes.IsEmpty();
	}
	if (Property == nullptr)
	{
		return false;
	}
	if (Model.CanonicalType == TEXT("name:fname"))
	{
		if (!Property->IsA<FNameProperty>()
			|| (Model.Direction != TEXT("value") && Model.Direction != TEXT("const_ref"))
			|| Model.Kind != TEXT("name_utf8")
			|| Model.CppType != TEXT("FName")
			|| Model.AbiTypes != TArray<FString>({ TEXT("i") })
			|| DeclaredType == nullptr
			|| DeclaredType->CanonicalType != TEXT("name:fname")
			|| DeclaredType->Kind != TEXT("name_utf8")
			|| DeclaredType->CppType != TEXT("FName")
			|| DeclaredType->Size != 4
			|| DeclaredType->Alignment != 4
			|| DeclaredType->AbiTypes != TArray<FString>({ TEXT("i") }))
		{
			return false;
		}
		OutKind = EAvidScriptRuntimeBindingKind::Name;
		return true;
	}

	if (Model.Kind == TEXT("object_handle"))
	{
		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		if (ObjectProperty == nullptr
			|| ObjectProperty->PropertyClass == nullptr
			|| Model.CanonicalType != TEXT("object:") + ObjectProperty->PropertyClass->GetPathName()
			|| Model.AbiTypes != TArray<FString>({ TEXT("i"), TEXT("i") }))
		{
			return false;
		}
		OutKind = EAvidScriptRuntimeBindingKind::Object;
		OutObjectClass = ObjectProperty->PropertyClass;
		return true;
	}

	if (Model.Kind == TEXT("struct"))
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		if (StructProperty == nullptr || StructProperty->Struct == nullptr
			|| Model.CanonicalType != TEXT("struct:") + StructProperty->Struct->GetPathName())
		{
			return false;
		}
		if (StructProperty->Struct == TBaseStructure<FVector>::Get()
			&& Model.CppType == TEXT("FVector")
			&& Model.AbiTypes == TArray<FString>({ TEXT("f"), TEXT("f"), TEXT("f") }))
		{
			OutKind = EAvidScriptRuntimeBindingKind::Vector;
			return true;
		}
		if (StructProperty->Struct == TBaseStructure<FRotator>::Get()
			&& Model.CppType == TEXT("FRotator")
			&& Model.AbiTypes == TArray<FString>({ TEXT("f"), TEXT("f"), TEXT("f") }))
		{
			OutKind = EAvidScriptRuntimeBindingKind::Rotator;
			return true;
		}
		if (StructProperty->Struct == TBaseStructure<FTransform>::Get()
			&& Model.CppType == TEXT("FTransform")
			&& Model.AbiTypes == TArray<FString>({
				TEXT("f"), TEXT("f"), TEXT("f"),
				TEXT("f"), TEXT("f"), TEXT("f"),
				TEXT("f"), TEXT("f"), TEXT("f") }))
		{
			OutKind = EAvidScriptRuntimeBindingKind::Transform;
			return true;
		}
		return false;
	}

	if (Model.Kind == TEXT("enum"))
	{
		const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property);
		const FByteProperty* ByteProperty = CastField<FByteProperty>(Property);
		const UEnum* Enum = EnumProperty != nullptr
			? EnumProperty->GetEnum()
			: (ByteProperty != nullptr ? ByteProperty->Enum.Get() : nullptr);
		if (Enum == nullptr
			|| Model.CanonicalType != TEXT("enum:") + Enum->GetPathName()
			|| Model.AbiTypes != TArray<FString>({ TEXT("i") }))
		{
			return false;
		}
		OutKind = EAvidScriptRuntimeBindingKind::Enum;
		return true;
	}

	if (Model.Kind != TEXT("scalar") || Model.AbiTypes.Num() != 1)
	{
		return false;
	}
	if (Property->IsA<FBoolProperty>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:bool"), TEXT("bool"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::Bool; return true; }
	if (Property->IsA<FInt8Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:i8"), TEXT("int8"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::Int8; return true; }
	if (const FByteProperty* Byte = CastField<FByteProperty>(Property); Byte != nullptr && Byte->Enum == nullptr && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:u8"), TEXT("uint8"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::UInt8; return true; }
	if (Property->IsA<FInt16Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:i16"), TEXT("int16"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::Int16; return true; }
	if (Property->IsA<FUInt16Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:u16"), TEXT("uint16"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::UInt16; return true; }
	if (Property->IsA<FIntProperty>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:i32"), TEXT("int32"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::Int32; return true; }
	if (Property->IsA<FUInt32Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:u32"), TEXT("uint32"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::UInt32; return true; }
	if (Property->IsA<FInt64Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:i64"), TEXT("int64"), TEXT("I"))) { OutKind = EAvidScriptRuntimeBindingKind::Int64; return true; }
	if (Property->IsA<FUInt64Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:u64"), TEXT("uint64"), TEXT("I"))) { OutKind = EAvidScriptRuntimeBindingKind::UInt64; return true; }
	if (Property->IsA<FFloatProperty>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:f32"), TEXT("float"), TEXT("f"))) { OutKind = EAvidScriptRuntimeBindingKind::Float; return true; }
	if (Property->IsA<FDoubleProperty>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:f64"), TEXT("double"), TEXT("F"))) { OutKind = EAvidScriptRuntimeBindingKind::Double; return true; }
	return false;
}

int32 GetAvidScriptRuntimeGuestStorageSize(EAvidScriptRuntimeBindingKind Kind)
{
	switch (Kind)
	{
	case EAvidScriptRuntimeBindingKind::Bool:
	case EAvidScriptRuntimeBindingKind::Int32:
	case EAvidScriptRuntimeBindingKind::UInt32:
	case EAvidScriptRuntimeBindingKind::Float:
	case EAvidScriptRuntimeBindingKind::Enum:
		return 4;
	case EAvidScriptRuntimeBindingKind::Int8:
	case EAvidScriptRuntimeBindingKind::UInt8:
		return 1;
	case EAvidScriptRuntimeBindingKind::Int16:
	case EAvidScriptRuntimeBindingKind::UInt16:
		return 2;
	case EAvidScriptRuntimeBindingKind::Int64:
	case EAvidScriptRuntimeBindingKind::UInt64:
	case EAvidScriptRuntimeBindingKind::Double:
	case EAvidScriptRuntimeBindingKind::Object:
		return 8;
	case EAvidScriptRuntimeBindingKind::Vector:
	case EAvidScriptRuntimeBindingKind::Rotator:
		return 12;
	case EAvidScriptRuntimeBindingKind::Transform:
		return 36;
	default:
		return 0;
	}
}

int32 GetAvidScriptRuntimeArgumentWidth(
	const FAvidScriptBindingValueModel& Model,
	EAvidScriptRuntimeBindingDirection Direction)
{
	if (Model.CanonicalType == TEXT("void"))
	{
		return 0;
	}
	return Direction == EAvidScriptRuntimeBindingDirection::Ref
		|| Direction == EAvidScriptRuntimeBindingDirection::Out
		|| Direction == EAvidScriptRuntimeBindingDirection::Return
		? 1
		: Model.AbiTypes.Num();
}

FString MakeAvidScriptRuntimeExpectedSignature(const FAvidScriptBindingFunctionModel& Binding)
{
	FString Parameters;
	if (!Binding.bStatic)
	{
		Parameters += TEXT("ii");
	}
	for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
	{
		if (Parameter.Direction == TEXT("ref") || Parameter.Direction == TEXT("out"))
		{
			Parameters += TEXT("i");
		}
		else
		{
			Parameters += FString::Join(Parameter.AbiTypes, TEXT(""));
		}
	}
	if (Binding.ReturnValue.CanonicalType != TEXT("void"))
	{
		Parameters += TEXT("i");
	}
	return TEXT("(") + Parameters + TEXT(")i");
}

FString MakeAvidScriptRuntimeCanonicalIdentity(
	const UClass* OwnerClass,
	const UFunction* Function,
	const FAvidScriptBindingFunctionModel& Binding)
{
	FString Identity = OwnerClass->GetPathName()
		+ TEXT("::")
		+ Function->GetName()
		+ TEXT("(")
		+ Binding.ReturnValue.CanonicalType;
	for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
	{
		Identity += TEXT(";")
			+ Parameter.Name
			+ TEXT(":")
			+ Parameter.Direction
			+ TEXT(":")
			+ Parameter.CanonicalType;
	}
	Identity += TEXT(")");
	return Identity;
}

FString MakeAvidScriptRuntimePropertyGetCanonicalIdentity(
	const UClass* OwnerClass,
	const FProperty* Property,
	const FAvidScriptBindingFunctionModel& Binding)
{
	return OwnerClass->GetPathName()
		+ TEXT("::property_get:") + Property->GetName()
		+ TEXT("(") + Binding.ReturnValue.CanonicalType + TEXT(")");
}

FString MakeAvidScriptRuntimePropertySetCanonicalIdentity(
	const UClass* OwnerClass,
	const FProperty* Property,
	const FAvidScriptBindingFunctionModel& Binding)
{
	return FAvidScriptBindingDescriptorIdentity::MakePropertySetCanonicalIdentity(
		OwnerClass->GetPathName(),
		Property->GetName(),
		Binding.Parameters[0].CanonicalType,
		Binding.UeFunction);
}

FString MakeAvidScriptRuntimeSelectionHash(const FAvidScriptBindingPackageModel& Package)
{
	return FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
}

FString MakeAvidScriptRuntimePackageHash(const FAvidScriptBindingPackageModel& Package)
{
	return FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);
}

bool ReadAvidScriptRuntimeF32(uint64 Cell, float& OutValue)
{
	const uint32 Bits = static_cast<uint32>(Cell);
	FMemory::Memcpy(&OutValue, &Bits, sizeof(OutValue));
	return FMath::IsFinite(OutValue);
}

bool ReadAvidScriptRuntimeF64(uint64 Cell, double& OutValue)
{
	FMemory::Memcpy(&OutValue, &Cell, sizeof(OutValue));
	return FMath::IsFinite(OutValue);
}

bool ResolveAvidScriptRuntimeHandle(
	uint32 Slot,
	uint32 Generation,
	UClass* ExpectedClass,
	const FAvidScriptBindingInvocationContext& Context,
	bool bAllowNull,
	UObject*& OutObject,
	FString& OutDetails)
{
	OutObject = nullptr;
	if (Slot == 0 && Generation == 0 && bAllowNull)
	{
		return true;
	}
	if (Slot == 0 || Generation == 0 || Context.ObjectRegistry == nullptr)
	{
		OutDetails = TEXT("The binding call supplied an invalid UObject handle or has no object registry.");
		return false;
	}
	FAvidScriptObjectHandleResult ResolveResult;
	OutObject = Context.ObjectRegistry->ResolveObject(
		{ Slot, Generation },
		ResolveResult,
		false);
	if (OutObject == nullptr)
	{
		Context.ObjectRegistry->ResolveObject(
			{ Slot, Generation },
			ResolveResult,
			true);
		OutDetails = ResolveResult.ErrorMessage;
		return false;
	}
	if (ExpectedClass != nullptr && !OutObject->IsA(ExpectedClass))
	{
		OutDetails = FString::Printf(
			TEXT("Resolved object '%s' is not a '%s'."),
			*OutObject->GetPathName(),
			*ExpectedClass->GetPathName());
		OutObject = nullptr;
		return false;
	}
	return true;
}

bool SetAvidScriptRuntimeNumericValue(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	void* Frame,
	uint64 Cell,
	FString& OutDetails)
{
	void* Value = Plan.Property->ContainerPtrToValuePtr<void>(Frame);
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Bool)
	{
		CastFieldChecked<FBoolProperty>(Plan.Property)->SetPropertyValue(Value, static_cast<uint32>(Cell) != 0);
		return true;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Float)
	{
		float Number = 0.0f;
		if (!ReadAvidScriptRuntimeF32(Cell, Number))
		{
			OutDetails = TEXT("The binding call supplied a non-finite float.");
			return false;
		}
		CastFieldChecked<FFloatProperty>(Plan.Property)->SetPropertyValue(Value, Number);
		return true;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Double)
	{
		double Number = 0.0;
		if (!ReadAvidScriptRuntimeF64(Cell, Number))
		{
			OutDetails = TEXT("The binding call supplied a non-finite double.");
			return false;
		}
		CastFieldChecked<FDoubleProperty>(Plan.Property)->SetPropertyValue(Value, Number);
		return true;
	}

	FNumericProperty* NumericProperty = CastField<FNumericProperty>(Plan.Property);
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Enum)
	{
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Plan.Property))
		{
			NumericProperty = EnumProperty->GetUnderlyingProperty();
		}
	}
	if (NumericProperty == nullptr)
	{
		OutDetails = TEXT("The cached numeric property plan is invalid.");
		return false;
	}
	uint64 ValueBits = Cell;
	switch (Plan.Kind)
	{
	case EAvidScriptRuntimeBindingKind::Int8: ValueBits = static_cast<uint64>(static_cast<int64>(static_cast<int8>(Cell))); break;
	case EAvidScriptRuntimeBindingKind::UInt8: ValueBits = static_cast<uint8>(Cell); break;
	case EAvidScriptRuntimeBindingKind::Int16: ValueBits = static_cast<uint64>(static_cast<int64>(static_cast<int16>(Cell))); break;
	case EAvidScriptRuntimeBindingKind::UInt16: ValueBits = static_cast<uint16>(Cell); break;
	case EAvidScriptRuntimeBindingKind::Int32:
	case EAvidScriptRuntimeBindingKind::Enum:
		ValueBits = static_cast<uint64>(static_cast<int64>(static_cast<int32>(Cell)));
		break;
	case EAvidScriptRuntimeBindingKind::UInt32: ValueBits = static_cast<uint32>(Cell); break;
	case EAvidScriptRuntimeBindingKind::Int64: ValueBits = static_cast<uint64>(static_cast<int64>(Cell)); break;
	case EAvidScriptRuntimeBindingKind::UInt64: break;
	default:
		OutDetails = TEXT("The cached numeric kind is unsupported.");
		return false;
	}
	NumericProperty->SetIntPropertyValue(Value, ValueBits);
	return true;
}

bool SetAvidScriptRuntimeNameValue(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	void* Frame,
	FString& OutDetails)
{
	uint8 LengthBytes[sizeof(int32)] = {};
	if (GuestAddress > MAX_uint32 - sizeof(LengthBytes)
		|| !GuestMemory.ReadBytes(GuestAddress, MakeArrayView(LengthBytes), OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The FName length prefix is outside guest memory.");
		}
		return false;
	}

	const uint32 UnsignedLength = static_cast<uint32>(LengthBytes[0])
		| (static_cast<uint32>(LengthBytes[1]) << 8)
		| (static_cast<uint32>(LengthBytes[2]) << 16)
		| (static_cast<uint32>(LengthBytes[3]) << 24);
	static constexpr int32 MaxUtf8Bytes = NAME_SIZE * 4;
	if (UnsignedLength > static_cast<uint32>(MaxUtf8Bytes))
	{
		OutDetails = FString::Printf(
			TEXT("The FName UTF-8 byte length must be between 0 and %d."),
			MaxUtf8Bytes);
		return false;
	}
	const int32 PayloadLength = static_cast<int32>(UnsignedLength);

	const uint32 StoredSize = static_cast<uint32>(PayloadLength) + 1;
	const uint64 PayloadAddress64 = static_cast<uint64>(GuestAddress) + sizeof(LengthBytes);
	const uint64 PayloadEnd64 = PayloadAddress64 + StoredSize;
	if (PayloadEnd64 > static_cast<uint64>(MAX_uint32) + 1)
	{
		OutDetails = TEXT("The FName payload address overflows guest memory.");
		return false;
	}
	const uint32 PayloadAddress = static_cast<uint32>(PayloadAddress64);
	TArray<uint8, TInlineAllocator<256>> Payload;
	Payload.SetNumUninitialized(StoredSize);
	if (!GuestMemory.ReadBytes(PayloadAddress, MakeArrayView(Payload), OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The FName payload is outside guest memory.");
		}
		return false;
	}
	if (Payload[PayloadLength] != 0)
	{
		OutDetails = TEXT("The FName payload is not followed by a zero terminator.");
		return false;
	}
	for (int32 Index = 0; Index < PayloadLength; ++Index)
	{
		if (Payload[Index] == 0)
		{
			OutDetails = TEXT("The FName UTF-8 payload contains an embedded NUL byte.");
			return false;
		}
	}

	const ANSICHAR* Utf8 = reinterpret_cast<const ANSICHAR*>(Payload.GetData());
	const FUTF8ToTCHAR Converted(Utf8, PayloadLength);
	if (Converted.Length() >= NAME_SIZE)
	{
		OutDetails = FString::Printf(
			TEXT("The decoded FName must contain fewer than %d TCHAR code units."),
			NAME_SIZE);
		return false;
	}
	const FTCHARToUTF8 RoundTrip(Converted.Get(), Converted.Length());
	if (RoundTrip.Length() != PayloadLength
		|| (PayloadLength > 0
			&& FMemory::Memcmp(RoundTrip.Get(), Payload.GetData(), PayloadLength) != 0))
	{
		OutDetails = TEXT("The FName payload is not canonical valid UTF-8.");
		return false;
	}

	void* Value = Plan.Property->ContainerPtrToValuePtr<void>(Frame);
	const FName Name = Converted.Length() == 0
		? NAME_None
		: FName(Converted.Length(), Converted.Get(), FNAME_Add);
	CastFieldChecked<FNameProperty>(Plan.Property)->SetPropertyValue(Value, Name);
	return true;
}

bool SetAvidScriptRuntimeValueFromCells(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	TConstArrayView<uint64> Cells,
	IAvidScriptVmGuestMemory* GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails)
{
	if (Cells.Num() != Plan.ArgumentWidth || Plan.Property == nullptr)
	{
		OutDetails = TEXT("The dynamic binding frame width does not match the cached value plan.");
		return false;
	}
	if (Plan.Kind <= EAvidScriptRuntimeBindingKind::Enum)
	{
		return SetAvidScriptRuntimeNumericValue(Plan, Frame, Cells[0], OutDetails);
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Name)
	{
		if (GuestMemory == nullptr)
		{
			OutDetails = TEXT("The FName input requires guest memory.");
			return false;
		}
		if (Cells[0] > MAX_uint32)
		{
			OutDetails = TEXT("The FName guest address does not fit the 32-bit guest address space.");
			return false;
		}
		return SetAvidScriptRuntimeNameValue(
			Plan,
			static_cast<uint32>(Cells[0]),
			*GuestMemory,
			Frame,
			OutDetails);
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Object)
	{
		UObject* Object = nullptr;
		if (!ResolveAvidScriptRuntimeHandle(
			static_cast<uint32>(Cells[0]),
			static_cast<uint32>(Cells[1]),
			Plan.ObjectClass,
			Context,
			true,
			Object,
			OutDetails))
		{
			return false;
		}
		CastFieldChecked<FObjectPropertyBase>(Plan.Property)->SetObjectPropertyValue_InContainer(Frame, Object);
		return true;
	}

	TArray<float, TInlineAllocator<9>> Components;
	Components.SetNumUninitialized(Cells.Num());
	for (int32 Index = 0; Index < Cells.Num(); ++Index)
	{
		if (!ReadAvidScriptRuntimeF32(Cells[Index], Components[Index]))
		{
			OutDetails = TEXT("The binding call supplied a non-finite struct component.");
			return false;
		}
	}
	void* Value = Plan.Property->ContainerPtrToValuePtr<void>(Frame);
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Vector)
	{
		*static_cast<FVector*>(Value) = FVector(Components[0], Components[1], Components[2]);
		return true;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Rotator)
	{
		*static_cast<FRotator*>(Value) = FRotator(Components[0], Components[1], Components[2]);
		return true;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Transform)
	{
		*static_cast<FTransform*>(Value) = FTransform(
			FRotator(Components[3], Components[4], Components[5]),
			FVector(Components[0], Components[1], Components[2]),
			FVector(Components[6], Components[7], Components[8]));
		return true;
	}
	OutDetails = TEXT("The cached struct kind is unsupported.");
	return false;
}

bool SetAvidScriptRuntimeValueFromGuest(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails)
{
	uint8 Bytes[36] = {};
	if (Plan.GuestStorageSize <= 0 || Plan.GuestStorageSize > UE_ARRAY_COUNT(Bytes)
		|| !GuestMemory.ReadBytes(GuestAddress, MakeArrayView(Bytes, Plan.GuestStorageSize), OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The cached guest storage size is invalid.");
		}
		return false;
	}

	TArray<uint64, TInlineAllocator<9>> Cells;
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Object)
	{
		uint32 Slot = 0;
		uint32 Generation = 0;
		FMemory::Memcpy(&Slot, Bytes, sizeof(Slot));
		FMemory::Memcpy(&Generation, Bytes + sizeof(Slot), sizeof(Generation));
		Cells = { Slot, Generation };
	}
	else if (Plan.Kind == EAvidScriptRuntimeBindingKind::Vector
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Rotator
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Transform)
	{
		const int32 ComponentCount = Plan.GuestStorageSize / sizeof(float);
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			uint32 Bits = 0;
			FMemory::Memcpy(&Bits, Bytes + Index * sizeof(float), sizeof(Bits));
			Cells.Add(Bits);
		}
	}
	else
	{
		uint64 Cell = 0;
		FMemory::Memcpy(&Cell, Bytes, Plan.GuestStorageSize);
		Cells.Add(Cell);
	}

	FAvidScriptRuntimeBindingValuePlan ValuePlan = Plan;
	ValuePlan.ArgumentWidth = Cells.Num();
	return SetAvidScriptRuntimeValueFromCells(
		ValuePlan,
		Cells,
		&GuestMemory,
		Context,
		Frame,
		OutDetails);
}

bool WriteAvidScriptRuntimeValueToGuest(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails)
{
	uint8 Bytes[36] = {};
	const void* Value = Plan.Property->ContainerPtrToValuePtr<void>(Frame);
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Bool)
	{
		const int32 Stored = CastFieldChecked<FBoolProperty>(Plan.Property)->GetPropertyValue(Value) ? 1 : 0;
		FMemory::Memcpy(Bytes, &Stored, sizeof(Stored));
	}
	else if (Plan.Kind == EAvidScriptRuntimeBindingKind::Float)
	{
		const float Stored = CastFieldChecked<FFloatProperty>(Plan.Property)->GetPropertyValue(Value);
		FMemory::Memcpy(Bytes, &Stored, sizeof(Stored));
	}
	else if (Plan.Kind == EAvidScriptRuntimeBindingKind::Double)
	{
		const double Stored = CastFieldChecked<FDoubleProperty>(Plan.Property)->GetPropertyValue(Value);
		FMemory::Memcpy(Bytes, &Stored, sizeof(Stored));
	}
	else if (Plan.Kind == EAvidScriptRuntimeBindingKind::Object)
	{
		UObject* Object = CastFieldChecked<FObjectPropertyBase>(Plan.Property)->GetObjectPropertyValue_InContainer(Frame);
		FAvidScriptObjectHandle Handle;
		if (Object != nullptr)
		{
			if (Context.ObjectRegistry == nullptr || Context.ObjectOwnership == nullptr)
			{
				OutDetails = TEXT("The binding call cannot publish a UObject result without registry and ownership services.");
				return false;
			}
			FAvidScriptObjectHandleResult BorrowResult;
			if (!Context.ObjectOwnership->Borrow(
					*Context.ObjectRegistry,
					*Object,
					BorrowResult))
			{
				OutDetails = BorrowResult.ErrorMessage;
				return false;
			}
			Handle = BorrowResult.Handle;
		}
		FMemory::Memcpy(Bytes, &Handle.Slot, sizeof(Handle.Slot));
		FMemory::Memcpy(Bytes + sizeof(Handle.Slot), &Handle.Generation, sizeof(Handle.Generation));
	}
	else if (Plan.Kind == EAvidScriptRuntimeBindingKind::Vector)
	{
		const FVector& Stored = *static_cast<const FVector*>(Value);
		const float Components[3] = {
			static_cast<float>(Stored.X),
			static_cast<float>(Stored.Y),
			static_cast<float>(Stored.Z)
		};
		FMemory::Memcpy(Bytes, Components, sizeof(Components));
	}
	else if (Plan.Kind == EAvidScriptRuntimeBindingKind::Rotator)
	{
		const FRotator& Stored = *static_cast<const FRotator*>(Value);
		const float Components[3] = {
			static_cast<float>(Stored.Pitch),
			static_cast<float>(Stored.Yaw),
			static_cast<float>(Stored.Roll)
		};
		FMemory::Memcpy(Bytes, Components, sizeof(Components));
	}
	else if (Plan.Kind == EAvidScriptRuntimeBindingKind::Transform)
	{
		const FTransform& Stored = *static_cast<const FTransform*>(Value);
		const FVector Translation = Stored.GetTranslation();
		const FRotator Rotation = Stored.Rotator();
		const FVector Scale = Stored.GetScale3D();
		const float Components[9] = {
			static_cast<float>(Translation.X),
			static_cast<float>(Translation.Y),
			static_cast<float>(Translation.Z),
			static_cast<float>(Rotation.Pitch),
			static_cast<float>(Rotation.Yaw),
			static_cast<float>(Rotation.Roll),
			static_cast<float>(Scale.X),
			static_cast<float>(Scale.Y),
			static_cast<float>(Scale.Z)
		};
		FMemory::Memcpy(Bytes, Components, sizeof(Components));
	}
	else
	{
		const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Plan.Property);
		if (Plan.Kind == EAvidScriptRuntimeBindingKind::Enum)
		{
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Plan.Property))
			{
				NumericProperty = EnumProperty->GetUnderlyingProperty();
			}
		}
		if (NumericProperty == nullptr)
		{
			OutDetails = TEXT("The cached numeric output property plan is invalid.");
			return false;
		}
		const uint64 Stored = NumericProperty->GetUnsignedIntPropertyValue(Value);
		FMemory::Memcpy(Bytes, &Stored, Plan.GuestStorageSize);
	}

	return GuestMemory.WriteBytes(
		GuestAddress,
		MakeArrayView(static_cast<const uint8*>(Bytes), Plan.GuestStorageSize),
		OutDetails);
}

bool BuildAvidScriptRuntimeValuePlan(
	FProperty* Property,
	const FAvidScriptBindingValueModel& Model,
	const FAvidScriptBindingTypeModel* DeclaredType,
	int32 ArgumentOffset,
	FAvidScriptRuntimeBindingValuePlan& OutPlan,
	FString& OutDetails)
{
	OutPlan = FAvidScriptRuntimeBindingValuePlan();
	OutPlan.Property = Property;
	OutPlan.ArgumentOffset = ArgumentOffset;
	OutPlan.Name = Model.Name;
	if (!ParseAvidScriptRuntimeDirection(Model.Direction, OutPlan.Direction)
		|| !ResolveAvidScriptRuntimeKind(
			Property,
			Model,
			DeclaredType,
			OutPlan.Kind,
			OutPlan.ObjectClass))
	{
		OutDetails = FString::Printf(
			TEXT("Reflected property '%s' no longer matches descriptor type '%s'."),
			*Model.Name,
			*Model.CanonicalType);
		return false;
	}
	OutPlan.ArgumentWidth = GetAvidScriptRuntimeArgumentWidth(Model, OutPlan.Direction);
	OutPlan.GuestStorageSize = GetAvidScriptRuntimeGuestStorageSize(OutPlan.Kind);
	return OutPlan.ArgumentWidth > 0 || OutPlan.Kind == EAvidScriptRuntimeBindingKind::Void;
}

bool ResolveAvidScriptLifecycleWorld(
	const FAvidScriptBindingInvocationContext& Context,
	const FString& Source,
	UWorld*& OutWorld,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	OutWorld = Context.World.Get();
	if (!IsValid(OutWorld) || OutWorld->bIsTearingDown)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_world_invalid"),
			Source,
			TEXT("The Runtime Session has no live World for object lifecycle operations."));
		return false;
	}
	return true;
}

bool ResolveAvidScriptLifecycleClass(
	const FAvidScriptBindingPackage& Package,
	const uint64 RawOrdinal,
	const FString& Source,
	UClass*& OutClass,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	UClass* BaseClass = nullptr;
	if (RawOrdinal > MAX_uint32
		|| !Package.TryResolveClassReference(static_cast<uint32>(RawOrdinal), OutClass, BaseClass))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_class_ordinal_invalid"),
			Source,
			TEXT("The class reference ordinal is outside the immutable package class plan."));
		return false;
	}
	return true;
}

bool ReadAvidScriptLifecycleTransform(
	const uint64 RawAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	FTransform& OutTransform,
	FString& OutDetails)
{
	constexpr int32 ComponentCount = 9;
	constexpr int32 ByteCount = ComponentCount * sizeof(float);
	if (RawAddress > MAX_uint32)
	{
		OutDetails = TEXT("The FTransform address exceeds the 32-bit Guest address space.");
		return false;
	}

	uint8 Bytes[ByteCount] = {};
	if (!GuestMemory.ReadBytes(
		static_cast<uint32>(RawAddress),
		MakeArrayView(Bytes),
		OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The fixed 36-byte FTransform is outside Guest memory.");
		}
		return false;
	}

	float Components[ComponentCount] = {};
	FMemory::Memcpy(Components, Bytes, ByteCount);
	for (const float Component : Components)
	{
		if (!FMath::IsFinite(Component))
		{
			OutDetails = TEXT("The Guest FTransform contains a non-finite component.");
			return false;
		}
	}

	OutTransform = FTransform(
		FRotator(Components[3], Components[4], Components[5]),
		FVector(Components[0], Components[1], Components[2]),
		FVector(Components[6], Components[7], Components[8]));
	return true;
}

bool WriteAvidScriptLifecycleHandle(
	const uint64 RawAddress,
	const FAvidScriptObjectHandle& Handle,
	IAvidScriptVmGuestMemory& GuestMemory,
	FString& OutDetails)
{
	if (RawAddress > MAX_uint32)
	{
		OutDetails = TEXT("The object handle output address exceeds the 32-bit Guest address space.");
		return false;
	}

	uint8 Bytes[sizeof(uint32) * 2] = {};
	FMemory::Memcpy(Bytes, &Handle.Slot, sizeof(Handle.Slot));
	FMemory::Memcpy(Bytes + sizeof(Handle.Slot), &Handle.Generation, sizeof(Handle.Generation));
	if (!GuestMemory.WriteBytes(static_cast<uint32>(RawAddress), MakeArrayView(Bytes), OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The object handle output is outside Guest memory.");
		}
		return false;
	}
	return true;
}

bool DispatchAvidScriptObjectLifecycle(
	const FAvidScriptBindingPackage& Package,
	const FAvidScriptRuntimeBindingInvocationPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (Plan.bRequiresWriteAccess && Context.WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_write_denied"),
			Plan.DebugPath,
			TEXT("The object lifecycle operation requires an explicitly writable host context."));
		return false;
	}
	if (Context.HostEffectJournal != nullptr
		&& Plan.Kind != EAvidScriptBindingInvocationKind::ObjectIsA)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_reload_effect_unsupported"),
			Plan.DebugPath,
			TEXT("Candidate reload cannot roll back SpawnActor or DestroyActor side effects."));
		return false;
	}
	if (Context.ObjectRegistry == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_registry_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object registry for lifecycle handles."));
		return false;
	}

	UWorld* World = nullptr;
	if (!ResolveAvidScriptLifecycleWorld(Context, Plan.DebugPath, World, OutResult))
	{
		return false;
	}

	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor)
	{
		UClass* ActorClass = nullptr;
		if (!ResolveAvidScriptLifecycleClass(Package, Call.Arguments[0], Plan.DebugPath, ActorClass, OutResult))
		{
			return false;
		}

		FTransform Transform = FTransform::Identity;
		FString Details;
		if (!ReadAvidScriptLifecycleTransform(Call.Arguments[1], *Call.GuestMemory, Transform, Details))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_guest_read_failed"),
				Plan.DebugPath,
				Details);
			return false;
		}

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(ActorClass, Transform, SpawnParameters);
		if (!IsValid(Actor) || Actor->GetWorld() != World)
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_spawn_failed"),
				Plan.DebugPath,
				TEXT("UWorld::SpawnActor did not return a live Actor in the injected World."));
			return false;
		}

		FAvidScriptObjectHandleResult RegisterResult;
		const FAvidScriptObjectHandle Handle = Context.ObjectRegistry->RegisterObject(Actor, RegisterResult, false);
		if (!RegisterResult.bSucceeded || !Handle.IsValid())
		{
			Actor->Destroy();
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_handle_registration_failed"),
				Plan.DebugPath,
				RegisterResult.ErrorMessage);
			return false;
		}

		if (!WriteAvidScriptLifecycleHandle(Call.Arguments[2], Handle, *Call.GuestMemory, Details))
		{
			FAvidScriptObjectHandleResult ReleaseResult;
			Context.ObjectRegistry->ReleaseHandle(Handle, ReleaseResult, false);
			Actor->Destroy();
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_guest_write_failed"),
				Plan.DebugPath,
				Details);
			return false;
		}

		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}

	if (Call.Arguments[0] > MAX_uint32 || Call.Arguments[1] > MAX_uint32)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			TEXT("The object handle cells exceed the 32-bit slot/generation ABI."));
		return false;
	}
	const FAvidScriptObjectHandle Handle{
		static_cast<uint32>(Call.Arguments[0]),
		static_cast<uint32>(Call.Arguments[1])
	};
	FAvidScriptObjectHandleResult ResolveResult;
	AActor* Actor = Context.ObjectRegistry->ResolveObject<AActor>(Handle, ResolveResult, false);
	if (Actor == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			ResolveResult.ErrorMessage);
		return false;
	}
	if (Actor->GetWorld() != World)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_cross_world"),
			Plan.DebugPath,
			TEXT("The Actor handle belongs to a different World than the active Runtime Session."));
		return false;
	}

	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectDestroyActor)
	{
		if (Handle == Context.OwnerHandle)
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_destroy_owner_unsupported"),
				Plan.DebugPath,
				TEXT("Destroying the Runtime owner requires a deferred component shutdown path."));
			return false;
		}
		if (!Actor->Destroy())
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_destroy_failed"),
				Plan.DebugPath,
				TEXT("AActor::Destroy rejected the lifecycle request; the handle remains live."));
			return false;
		}

		FAvidScriptObjectHandleResult ReleaseResult;
		if (!Context.ObjectRegistry->ReleaseHandle(Handle, ReleaseResult, false))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_handle_release_failed"),
				Plan.DebugPath,
				ReleaseResult.ErrorMessage);
			return false;
		}

		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}

	UClass* Class = nullptr;
	if (!ResolveAvidScriptLifecycleClass(Package, Call.Arguments[2], Plan.DebugPath, Class, OutResult))
	{
		return false;
	}
	OutResult.bSucceeded = true;
	OutResult.ReturnValue = Actor->IsA(Class) ? 1 : 0;
	return true;
}

bool DispatchAvidScriptObjectType(
	const FAvidScriptBindingPackage& Package,
	const FAvidScriptRuntimeBindingInvocationPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (Context.ObjectRegistry == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_registry_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object registry for object type handles."));
		return false;
	}
	if (Call.Arguments[0] > MAX_uint32
		|| Call.Arguments[1] > MAX_uint32
		|| Call.Arguments[2] > MAX_uint32)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			TEXT("The object handle or object type ordinal exceeds the 32-bit ABI."));
		return false;
	}

	const FAvidScriptObjectHandle Handle{
		static_cast<uint32>(Call.Arguments[0]),
		static_cast<uint32>(Call.Arguments[1])
	};
	FAvidScriptObjectHandleResult ResolveResult;
	UObject* Object = Context.ObjectRegistry->ResolveObject(Handle, ResolveResult, false);
	if (Object == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			ResolveResult.ErrorMessage);
		return false;
	}

	UClass* CachedClass = nullptr;
	if (!Package.TryResolveObjectType(static_cast<uint32>(Call.Arguments[2]), CachedClass))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("object_type_ordinal_out_of_range"),
			Plan.DebugPath,
			TEXT("The object type ordinal is outside the immutable package type plan."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ReturnValue = Object->IsA(CachedClass) ? 1 : 0;
	return true;
}

bool DispatchAvidScriptObjectFactory(
	const FAvidScriptBindingPackage& Package,
	const FAvidScriptRuntimeBindingInvocationPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (Plan.bRequiresWriteAccess
		&& Context.WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_write_denied"),
			Plan.DebugPath,
			TEXT("The object factory operation requires an explicitly writable host context."));
		return false;
	}
	if (Context.HostEffectJournal != nullptr
		&& Plan.Kind != EAvidScriptBindingInvocationKind::ActorFindComponent)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_reload_effect_unsupported"),
			Plan.DebugPath,
			TEXT("Candidate reload cannot roll back Construct or Release side effects."));
		return false;
	}
	if (Context.ObjectRegistry == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_registry_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object registry for factory handles."));
		return false;
	}
	if (Context.ObjectOwnership == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_ownership_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object ownership domain."));
		return false;
	}
	if (Call.Arguments.ContainsByPredicate(
		[](const uint64 Argument)
		{
			return Argument > MAX_uint32;
		}))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_argument_invalid"),
			Plan.DebugPath,
			TEXT("A factory ordinal or object handle cell exceeds the 32-bit ABI."));
		return false;
	}

	FAvidScriptObjectHandleResult BindingResult;
	bool bSucceeded = false;
	switch (Plan.Kind)
	{
	case EAvidScriptBindingInvocationKind::ObjectConstruct:
	{
		const FAvidScriptObjectFactoryPlan* FactoryPlan = nullptr;
		if (!Package.TryResolveObjectFactory(
			static_cast<uint32>(Call.Arguments[0]),
			FactoryPlan))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("object_factory_ordinal_out_of_range"),
				Plan.DebugPath,
				TEXT("The factory ordinal is outside the immutable package factory plan."));
			return false;
		}
		const FAvidScriptObjectHandle OuterHandle{
			static_cast<uint32>(Call.Arguments[1]),
			static_cast<uint32>(Call.Arguments[2])
		};
		bSucceeded = FAvidScriptObjectFactoryBinding::Construct(
			*FactoryPlan,
			*Context.ObjectRegistry,
			*Context.ObjectOwnership,
			OuterHandle,
			BindingResult);
		break;
	}
	case EAvidScriptBindingInvocationKind::ObjectRelease:
	{
		const FAvidScriptObjectHandle Handle{
			static_cast<uint32>(Call.Arguments[0]),
			static_cast<uint32>(Call.Arguments[1])
		};
		bSucceeded = FAvidScriptObjectFactoryBinding::Release(
			*Context.ObjectRegistry,
			*Context.ObjectOwnership,
			Handle,
			BindingResult);
		break;
	}
	case EAvidScriptBindingInvocationKind::ActorFindComponent:
	{
		UClass* ComponentClass = nullptr;
		if (!Package.TryResolveObjectType(
			static_cast<uint32>(Call.Arguments[2]),
			ComponentClass))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("object_type_ordinal_out_of_range"),
				Plan.DebugPath,
				TEXT("The object type ordinal is outside the immutable package type plan."));
			return false;
		}
		const FAvidScriptObjectHandle ActorHandle{
			static_cast<uint32>(Call.Arguments[0]),
			static_cast<uint32>(Call.Arguments[1])
		};
		bSucceeded = FAvidScriptObjectFactoryBinding::FindComponent(
			*Context.ObjectRegistry,
			*Context.ObjectOwnership,
			ActorHandle,
			*ComponentClass,
			BindingResult);
		break;
	}
	default:
		checkNoEntry();
		break;
	}

	if (!bSucceeded || !BindingResult.bSucceeded)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			BindingResult.ErrorCategory.IsEmpty()
				? FString(TEXT("binding_object_factory_failed"))
				: BindingResult.ErrorCategory,
			Plan.DebugPath,
			BindingResult.ErrorMessage.IsEmpty()
				? FString(TEXT("The object factory binding rejected the operation."))
				: BindingResult.ErrorMessage);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ReturnValue = 1;
	OutResult.ReturnValueI64 = static_cast<int64>(BindingResult.Handle.ToUInt64());
	return true;
}

bool DispatchAvidScriptSceneAttachment(
	const FAvidScriptRuntimeBindingInvocationPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (Plan.bRequiresWriteAccess
		&& Context.WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_write_denied"),
			Plan.DebugPath,
			TEXT("Scene attachment requires an explicitly writable host context."));
		return false;
	}
	if (Context.HostEffectJournal != nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_reload_effect_unsupported"),
			Plan.DebugPath,
			TEXT("Candidate reload cannot roll back Attach or Detach side effects."));
		return false;
	}
	if (Context.ObjectRegistry == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_registry_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object registry for scene component handles."));
		return false;
	}
	if (Call.Arguments.ContainsByPredicate(
		[](const uint64 Argument)
		{
			return Argument > MAX_uint32;
		}))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_argument_invalid"),
			Plan.DebugPath,
			TEXT("A scene component handle cell or rules field exceeds the 32-bit ABI."));
		return false;
	}

	const FAvidScriptObjectHandle ChildHandle{
		static_cast<uint32>(Call.Arguments[0]),
		static_cast<uint32>(Call.Arguments[1])
	};
	FAvidScriptObjectHandleResult BindingResult;
	bool bSucceeded = false;
	switch (Plan.Kind)
	{
	case EAvidScriptBindingInvocationKind::SceneComponentAttach:
	{
		const FAvidScriptObjectHandle ParentHandle{
			static_cast<uint32>(Call.Arguments[2]),
			static_cast<uint32>(Call.Arguments[3])
		};
		bSucceeded = FAvidScriptSceneAttachmentBinding::Attach(
			*Context.ObjectRegistry,
			ChildHandle,
			ParentHandle,
			static_cast<uint32>(Call.Arguments[4]),
			BindingResult);
		break;
	}
	case EAvidScriptBindingInvocationKind::SceneComponentDetach:
		bSucceeded = FAvidScriptSceneAttachmentBinding::Detach(
			*Context.ObjectRegistry,
			ChildHandle,
			static_cast<uint32>(Call.Arguments[2]),
			BindingResult);
		break;
	default:
		checkNoEntry();
		break;
	}

	if (!bSucceeded || !BindingResult.bSucceeded)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			BindingResult.ErrorCategory.IsEmpty()
				? FString(TEXT("binding_scene_attachment_failed"))
				: BindingResult.ErrorCategory,
			Plan.DebugPath,
			BindingResult.ErrorMessage.IsEmpty()
				? FString(TEXT("The scene attachment binding rejected the operation."))
				: BindingResult.ErrorMessage);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ReturnValue = 1;
	return true;
}
} // namespace

struct FAvidScriptBindingPackage::FImpl
{
	struct FClassReferencePlan
	{
		UClass* Class = nullptr;
		UClass* BaseClass = nullptr;
	};

	FString PackageName;
	FString PackageHash;
	int32 DescriptorSchemaVersion = 0;
	FAvidScriptVmBindingPackage VmPackage;
	TArray<FAvidScriptRuntimeBindingInvocationPlan> Plans;
	TArray<UClass*> ObjectTypePlans;
	UClass* ExpectedSelfClass = nullptr;
	TArray<FClassReferencePlan> ClassReferencePlans;
	TArray<FAvidScriptObjectFactoryPlan> ObjectFactoryPlans;
	TArray<TStrongObjectPtr<UClass>> LoadedClasses;
	FAvidScriptBindingPackageInstrumentation Instrumentation;
	int32 RequiredScratchSize = 0;
};

FAvidScriptBindingPackage::FAvidScriptBindingPackage()
	: Impl(MakeUnique<FImpl>())
{
}

FAvidScriptBindingPackage::~FAvidScriptBindingPackage() = default;

bool FAvidScriptBindingPackage::LoadDescriptor(
	const FString& DescriptorJson,
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FAvidScriptBindingPackageLoadResult& OutResult)
{
	OutPackage.Reset();
	OutResult = FAvidScriptBindingPackageLoadResult();
	FAvidScriptBindingPackageModel Model;
	FString ErrorCategory;
	FString ErrorSource;
	if (!FAvidScriptBindingDescriptorParser::Parse(
		DescriptorJson,
		Model,
		ErrorCategory,
		ErrorSource))
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			ErrorCategory,
			ErrorSource,
			TEXT("The binding descriptor failed its shared schema contract."));
		return false;
	}

	const FString CurrentEngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	if (Model.EngineVersion != CurrentEngineVersion)
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("binding_engine_mismatch"),
			Model.EngineVersion,
			FString::Printf(TEXT("Expected UE %s."), *CurrentEngineVersion));
		return false;
	}
	if (Model.SelectionHash != MakeAvidScriptRuntimeSelectionHash(Model)
		|| Model.PackageHash != MakeAvidScriptRuntimePackageHash(Model))
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("binding_package_hash_mismatch"),
			Model.PackageName,
			TEXT("Selection or package identity does not match the descriptor contents."));
		return false;
	}
	TMap<FString, const FAvidScriptBindingTypeModel*> DeclaredTypesByCanonical;
	TMap<FString, const FAvidScriptBindingTypeModel*> DeclaredTypesById;
	TMap<FString, const FAvidScriptBindingTypeModel*> DeclaredTypesByClassPath;
	DeclaredTypesByCanonical.Reserve(Model.Types.Num());
	DeclaredTypesById.Reserve(Model.Types.Num());
	DeclaredTypesByClassPath.Reserve(Model.Types.Num());
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		DeclaredTypesByCanonical.Add(Type.CanonicalType, &Type);
		DeclaredTypesById.Add(Type.StableId, &Type);
		if (!Type.ClassPath.IsEmpty())
		{
			DeclaredTypesByClassPath.Add(Type.ClassPath, &Type);
		}
	}

	TSharedPtr<FAvidScriptBindingPackage> Package = MakeShareable(new FAvidScriptBindingPackage());
	Package->Impl->PackageName = Model.PackageName;
	Package->Impl->PackageHash = Model.PackageHash;
	Package->Impl->DescriptorSchemaVersion = Model.SchemaVersion;
	Package->Impl->VmPackage.PackageName = Model.PackageName;
	Package->Impl->VmPackage.PackageHash = Model.PackageHash;
	int32 ObjectTypeCount = 0;
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		if (Type.ObjectTypeOrdinal != INDEX_NONE)
		{
			++ObjectTypeCount;
		}
	}
	const int32 ObjectTypeBindingCount = ObjectTypeCount == 0
		? 0
		: FAvidScriptObjectTypeBindings::GetSpecs().Num();
	Package->Impl->ObjectTypePlans.SetNumZeroed(ObjectTypeCount);
	Package->Impl->ClassReferencePlans.Reserve(Model.ClassReferences.Num());
	Package->Impl->ObjectFactoryPlans.SetNum(Model.ObjectFactories.Num());

	TMap<FString, UClass*> LoadedClassesByPath;
	TSet<FString> AttemptedClassPaths;
	const auto LoadClass = [&LoadedClassesByPath, &AttemptedClassPaths, &Package](
		const FString& ClassPath) -> UClass*
	{
		if (AttemptedClassPaths.Contains(ClassPath))
		{
			return LoadedClassesByPath.FindRef(ClassPath);
		}
		AttemptedClassPaths.Add(ClassPath);
		++Package->Impl->Instrumentation.ClassLoadCount;
		UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (LoadedClass != nullptr && LoadedClass->GetPathName() == ClassPath)
		{
			LoadedClassesByPath.Add(ClassPath, LoadedClass);
			Package->Impl->LoadedClasses.Emplace(LoadedClass);
			return LoadedClass;
		}
		LoadedClassesByPath.Add(ClassPath, nullptr);
		return nullptr;
	};

	TMap<FString, int32> ObjectTypeOrdinalsById;
	TMap<FString, int32> ObjectTypeOrdinalsByClassPath;
	TMap<int32, const FAvidScriptBindingTypeModel*> ObjectTypesByOrdinal;
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		if (Type.ObjectTypeOrdinal == INDEX_NONE)
		{
			continue;
		}
		ObjectTypeOrdinalsById.Add(Type.StableId, Type.ObjectTypeOrdinal);
		ObjectTypeOrdinalsByClassPath.Add(Type.ClassPath, Type.ObjectTypeOrdinal);
		ObjectTypesByOrdinal.Add(Type.ObjectTypeOrdinal, &Type);
	}

	TSet<FString> ActiveObjectTypeIds;
	TArray<FString> PendingObjectTypeIds;
	FString MissingRequiredTypeId;
	const auto RequireObjectType = [
		&DeclaredTypesById,
		&ActiveObjectTypeIds,
		&PendingObjectTypeIds,
		&MissingRequiredTypeId](const FString& TypeId)
	{
		if (TypeId.IsEmpty() || !MissingRequiredTypeId.IsEmpty())
		{
			return;
		}
		const FAvidScriptBindingTypeModel* Type = DeclaredTypesById.FindRef(TypeId);
		if (Type == nullptr)
		{
			MissingRequiredTypeId = TypeId;
			return;
		}
		if (Type->ObjectTypeOrdinal != INDEX_NONE
			&& !ActiveObjectTypeIds.Contains(TypeId))
		{
			ActiveObjectTypeIds.Add(TypeId);
			PendingObjectTypeIds.Add(TypeId);
		}
	};

	if (Model.bHasActiveObjectTypeOrdinals)
	{
		for (const int32 Ordinal : Model.ActiveObjectTypeOrdinals)
		{
			const FAvidScriptBindingTypeModel* Type =
				ObjectTypesByOrdinal.FindRef(Ordinal);
			if (Type != nullptr)
			{
				RequireObjectType(Type->StableId);
			}
		}
	}
	else
	{
		for (const TPair<int32, const FAvidScriptBindingTypeModel*>& Pair :
			ObjectTypesByOrdinal)
		{
			RequireObjectType(Pair.Value->StableId);
		}
	}
	RequireObjectType(Model.SelfTypeId);
	for (const FAvidScriptBindingClassReferenceModel& Reference : Model.ClassReferences)
	{
		RequireObjectType(Reference.ResultTypeId);
	}
	for (const FAvidScriptBindingObjectFactoryModel& Factory : Model.ObjectFactories)
	{
		RequireObjectType(Factory.OuterTypeId);
		const FAvidScriptBindingClassReferenceModel* Reference =
			Model.ClassReferences.FindByPredicate(
				[&Factory](const FAvidScriptBindingClassReferenceModel& Candidate)
				{
					return Candidate.StableId == Factory.ClassReferenceId;
				});
		const FAvidScriptBindingTypeModel* ConcreteType = Reference == nullptr
			? nullptr
			: DeclaredTypesByClassPath.FindRef(Reference->ClassPath);
		if (ConcreteType == nullptr)
		{
			MissingRequiredTypeId = Reference == nullptr
				? Factory.ClassReferenceId
				: Reference->ClassPath;
		}
		else
		{
			RequireObjectType(ConcreteType->StableId);
		}
	}
	for (const FAvidScriptBindingFunctionModel& Binding : Model.Bindings)
	{
		if (const FAvidScriptBindingTypeModel* OwnerType =
			DeclaredTypesByClassPath.FindRef(Binding.OwnerClass))
		{
			RequireObjectType(OwnerType->StableId);
		}
		if (Binding.ReturnValue.Kind == TEXT("object_handle"))
		{
			RequireObjectType(Binding.ReturnValue.TypeId);
		}
		for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
		{
			if (Parameter.Kind == TEXT("object_handle"))
			{
				RequireObjectType(Parameter.TypeId);
			}
		}
	}
	while (!PendingObjectTypeIds.IsEmpty())
	{
		const FString TypeId = PendingObjectTypeIds.Pop(EAllowShrinking::No);
		const FAvidScriptBindingTypeModel* Type = DeclaredTypesById.FindRef(TypeId);
		if (Type != nullptr)
		{
			RequireObjectType(Type->BaseTypeId);
		}
	}
	if (!MissingRequiredTypeId.IsEmpty())
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("binding_object_type_required_missing"),
			MissingRequiredTypeId,
			TEXT("A selected binding capability references a type outside the descriptor type graph."));
		return false;
	}

	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		if (Type.ObjectTypeOrdinal == INDEX_NONE
			|| !ActiveObjectTypeIds.Contains(Type.StableId))
		{
			continue;
		}
		UClass* ObjectClass = LoadClass(Type.ClassPath);
		if (ObjectClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_object_type_class_missing"),
				Type.ClassPath,
				TEXT("A v6 object type class is unavailable or does not keep its canonical path."));
			return false;
		}
		Package->Impl->ObjectTypePlans[Type.ObjectTypeOrdinal] = ObjectClass;
	}
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		if (Type.ObjectTypeOrdinal == INDEX_NONE
			|| !ActiveObjectTypeIds.Contains(Type.StableId))
		{
			continue;
		}
		UClass* ObjectClass = Package->Impl->ObjectTypePlans[Type.ObjectTypeOrdinal];
		UClass* ExpectedBaseClass = nullptr;
		if (!Type.BaseTypeId.IsEmpty())
		{
			const int32* BaseOrdinal = ObjectTypeOrdinalsById.Find(Type.BaseTypeId);
			ExpectedBaseClass = BaseOrdinal == nullptr
				? nullptr
				: Package->Impl->ObjectTypePlans[*BaseOrdinal];
		}
		if (ObjectClass == nullptr
			|| ObjectClass->GetSuperClass() != ExpectedBaseClass)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_object_type_base_mismatch"),
				Type.ClassPath,
				TEXT("The v6 base_type_id must match the reflected direct superclass."));
			return false;
		}
	}
	if (!Model.SelfTypeId.IsEmpty())
	{
		const int32* SelfOrdinal = ObjectTypeOrdinalsById.Find(Model.SelfTypeId);
		if (SelfOrdinal == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_self_type_missing"),
				Model.SelfTypeId,
				TEXT("The v6 Self type must resolve through the immutable object type plan."));
			return false;
		}
		Package->Impl->ExpectedSelfClass = Package->Impl->ObjectTypePlans[*SelfOrdinal];
		if (Package->Impl->ExpectedSelfClass == nullptr
			|| !Package->Impl->ExpectedSelfClass->IsChildOf(AActor::StaticClass()))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_self_type_mismatch"),
				Model.SelfTypeId,
				TEXT("The v6 Self type must resolve to an Actor-derived class."));
			return false;
		}
	}

	TSet<FString> FactoryClassReferenceIds;
	for (const FAvidScriptBindingObjectFactoryModel& Factory : Model.ObjectFactories)
	{
		FactoryClassReferenceIds.Add(Factory.ClassReferenceId);
	}
	TMap<FString, UClass*> FactoryClassesByReferenceId;
	bool bHasLifecycleClassReferences = false;
	if (Model.SchemaVersion >= 7)
	{
		Package->Impl->ClassReferencePlans.SetNum(Model.ClassReferences.Num());
	}

	for (const FAvidScriptBindingClassReferenceModel& Reference : Model.ClassReferences)
	{
#if !WITH_EDITOR
		if (Reference.LoadPolicy == TEXT("EditorLoad"))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_editor_only"),
				Reference.ClassPath,
				TEXT("EditorLoad class references cannot be activated outside an Editor target."));
			return false;
		}
#endif
		const bool bFactoryClassReference =
			FactoryClassReferenceIds.Contains(Reference.StableId);
		const bool bActorLifecycleReference =
			Model.SchemaVersion < 6
			|| FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(
				Model,
				Reference.ResultTypeId,
				TEXT("/Script/Engine.Actor"));
		if (bFactoryClassReference == bActorLifecycleReference)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_capability_missing"),
				Reference.StableId,
				TEXT("Each class reference must expose exactly one of Actor lifecycle or object factory capability."));
			return false;
		}
		if (bFactoryClassReference)
		{
			UClass* Class = LoadClass(Reference.ClassPath);
			const int32* BaseOrdinal =
				ObjectTypeOrdinalsById.Find(Reference.ResultTypeId);
			UClass* BaseClass = BaseOrdinal == nullptr
				? nullptr
				: Package->Impl->ObjectTypePlans[*BaseOrdinal];
			if (Class == nullptr || BaseClass == nullptr)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					Reference.LoadPolicy == TEXT("CookRequired")
						? FString(TEXT("binding_class_cook_missing"))
						: FString(TEXT("binding_class_missing")),
					Reference.ClassPath,
					TEXT("The factory class is unavailable under the declared load policy."));
				return false;
			}
			if (!Class->IsChildOf(BaseClass))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_factory_class_inheritance_mismatch"),
					Reference.ClassPath + TEXT(" -> ")
						+ Reference.BaseClassPath,
					TEXT("The factory class must satisfy its declared base constraint."));
				return false;
			}
			FactoryClassesByReferenceId.Add(Reference.StableId, Class);
			continue;
		}

		UClass* Class = LoadClass(Reference.ClassPath);
		UClass* BaseClass = nullptr;
		if (Model.SchemaVersion >= 6)
		{
			const int32* ResultOrdinal = ObjectTypeOrdinalsById.Find(Reference.ResultTypeId);
			BaseClass = ResultOrdinal == nullptr
				? nullptr
				: Package->Impl->ObjectTypePlans[*ResultOrdinal];
		}
		else
		{
			BaseClass = LoadClass(Reference.BaseClassPath);
		}
		if (Class == nullptr || BaseClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				Reference.LoadPolicy == TEXT("CookRequired")
					? FString(TEXT("binding_class_cook_missing"))
					: FString(TEXT("binding_class_missing")),
				Class == nullptr ? Reference.ClassPath : Reference.BaseClassPath,
				TEXT("The class reference or its base constraint is unavailable under the declared load policy."));
			return false;
		}
		if (!Class->IsChildOf(BaseClass)
			|| !Class->IsChildOf(AActor::StaticClass())
			|| !BaseClass->IsChildOf(AActor::StaticClass()))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_inheritance_mismatch"),
				Reference.ClassPath + TEXT(" -> ") + Reference.BaseClassPath,
				TEXT("The resolved class must satisfy its AActor-derived base constraint."));
			return false;
		}
		if (Class->HasAnyClassFlags(
			CLASS_Abstract | CLASS_NotPlaceable | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_not_spawnable"),
				Reference.ClassPath,
				TEXT("The resolved class is abstract, deprecated, superseded, or not placeable."));
			return false;
		}
		if (Model.SchemaVersion >= 7)
		{
			Package->Impl->ClassReferencePlans[Reference.Ordinal] = { Class, BaseClass };
		}
		else
		{
			Package->Impl->ClassReferencePlans.Add({ Class, BaseClass });
		}
		bHasLifecycleClassReferences = true;
	}

	for (const FAvidScriptBindingObjectFactoryModel& Factory : Model.ObjectFactories)
	{
		UClass* ObjectClass = FactoryClassesByReferenceId.FindRef(Factory.ClassReferenceId);
		const int32* OuterOrdinal = ObjectTypeOrdinalsById.Find(Factory.OuterTypeId);
		const int32* ResultOrdinal = ObjectClass == nullptr
			? nullptr
			: ObjectTypeOrdinalsByClassPath.Find(ObjectClass->GetPathName());
		if (ObjectClass == nullptr || OuterOrdinal == nullptr || ResultOrdinal == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_result_type_missing"),
				Factory.StableId,
				TEXT("The factory class, required Outer, or concrete result type is missing from the immutable plan."));
			return false;
		}

		UClass* RequiredOuterClass = Package->Impl->ObjectTypePlans[*OuterOrdinal];
		if (RequiredOuterClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_outer_mismatch"),
				Factory.StableId,
				TEXT("The factory required Outer is unavailable from the immutable object type plan."));
			return false;
		}
		if (ObjectClass->HasAnyClassFlags(CLASS_Abstract))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_class_abstract"),
				ObjectClass->GetPathName(),
				TEXT("Factory classes must be concrete."));
			return false;
		}
		if (ObjectClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_class_deprecated"),
				ObjectClass->GetPathName(),
				TEXT("Factory classes cannot be deprecated or superseded."));
			return false;
		}

		const bool bActorClass = ObjectClass->IsChildOf(AActor::StaticClass());
		const bool bActorComponentClass = ObjectClass->IsChildOf(UActorComponent::StaticClass());
		const bool bKindMatches =
			(Factory.Kind == EAvidScriptObjectFactoryKind::NewObject
				&& Factory.Registration == EAvidScriptComponentRegistrationPolicy::None
				&& !bActorClass
				&& !bActorComponentClass)
			|| (Factory.Kind == EAvidScriptObjectFactoryKind::ActorComponent
				&& Factory.Registration
					== EAvidScriptComponentRegistrationPolicy::RegisterInstance
				&& bActorComponentClass);
		if (!bKindMatches)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_kind_mismatch"),
				Factory.StableId,
				TEXT("The factory kind, registration policy, and reflected class are incompatible."));
			return false;
		}

		UClass* ClassWithin = ObjectClass->ClassWithin;
		const bool bOuterMatchesClassWithin = ClassWithin != nullptr
			&& RequiredOuterClass->IsChildOf(ClassWithin);
		const bool bComponentOuterMatches = Factory.Kind
			!= EAvidScriptObjectFactoryKind::ActorComponent
			|| RequiredOuterClass->IsChildOf(AActor::StaticClass());
		if (!bOuterMatchesClassWithin || !bComponentOuterMatches)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_outer_mismatch"),
				Factory.StableId,
				TEXT("The factory required Outer does not satisfy the class ClassWithin contract."));
			return false;
		}

		FAvidScriptObjectFactoryPlan& Plan =
			Package->Impl->ObjectFactoryPlans[Factory.Ordinal];
		Plan.Kind = Factory.Kind;
		Plan.ObjectClass = ObjectClass;
		Plan.RequiredOuterClass = RequiredOuterClass;
		Plan.ResultObjectTypeOrdinal = *ResultOrdinal;
		Plan.Ownership = Factory.Ownership;
		Plan.Registration = Factory.Registration;
	}

	const int32 LifecycleBindingCount = bHasLifecycleClassReferences
		? FAvidScriptObjectLifecycleBindings::GetSpecs().Num()
		: 0;
	const int32 ObjectFactoryBindingCount = Model.SchemaVersion >= 7
		&& !Package->Impl->ObjectFactoryPlans.IsEmpty()
		? FAvidScriptObjectFactoryBinding::GetSpecs().Num()
		: 0;
	const bool bHasSceneComponentFactory =
		Model.SchemaVersion >= 7
		&& Package->Impl->ObjectFactoryPlans.ContainsByPredicate(
			[](const FAvidScriptObjectFactoryPlan& Factory)
			{
				return IsValid(Factory.ObjectClass)
					&& Factory.ObjectClass->IsChildOf(
						USceneComponent::StaticClass());
			});
	const int32 SceneAttachmentBindingCount = bHasSceneComponentFactory
		? FAvidScriptSceneAttachmentBinding::GetSpecs().Num()
		: 0;
	const int32 TotalImportCount =
		Model.Bindings.Num()
		+ LifecycleBindingCount
		+ ObjectTypeBindingCount
		+ ObjectFactoryBindingCount
		+ SceneAttachmentBindingCount;
	Package->Impl->Plans.Reserve(TotalImportCount);
	Package->Impl->VmPackage.Imports.Reserve(TotalImportCount);

	for (const FAvidScriptBindingFunctionModel& Binding : Model.Bindings)
	{
		UClass* OwnerClass = LoadClass(Binding.OwnerClass);
		if (OwnerClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_missing"),
				Binding.OwnerClass,
				TEXT("The reflected owner class is unavailable in this runtime build."));
			return false;
		}
		if (Binding.BindingKind == TEXT("property_set"))
		{
			++Package->Impl->Instrumentation.ReflectedNameLookupCount;
			FProperty* Property = FindFProperty<FProperty>(
				OwnerClass,
				FName(*Binding.UeMember));
			if (!IsAvidScriptRuntimePropertyWritable(Property)
				|| Property->GetOwnerStruct() != OwnerClass
				|| Binding.Parameters.Num() != 1)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_write_missing"),
					Binding.OwnerClass + TEXT(".") + Binding.UeMember,
					TEXT("The reflected property is missing or no longer satisfies runtime write policy."));
				return false;
			}

			const FString BlueprintSetterName =
				Property->GetMetaData(TEXT("BlueprintSetter"));
			UFunction* BlueprintSetter = nullptr;
			TArray<FProperty*> SetterParameters;
			if (Binding.UeFunction != BlueprintSetterName)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_blueprint_setter_mismatch"),
					Binding.CanonicalIdentity,
					TEXT("The authorized BlueprintSetter name no longer matches the property metadata."));
				return false;
			}
			if (BlueprintSetterName.IsEmpty())
			{
				if (Binding.DispatchMode != TEXT("cached_property_set")
					|| Binding.WritePolicy != TEXT("direct"))
				{
					SetAvidScriptBindingLoadFailure(
						OutResult,
						TEXT("binding_property_write_policy_mismatch"),
						Binding.CanonicalIdentity,
						TEXT("Direct reflected property writes require the cached direct policy."));
					return false;
				}
			}
			else
			{
				++Package->Impl->Instrumentation.ReflectedNameLookupCount;
				BlueprintSetter = OwnerClass->FindFunctionByName(
					FName(*Binding.UeFunction));
				if (BlueprintSetter != nullptr)
				{
					for (TFieldIterator<FProperty> It(BlueprintSetter); It; ++It)
					{
						FProperty* Parameter = *It;
						if (Parameter->HasAnyPropertyFlags(CPF_Parm)
							&& !Parameter->HasAnyPropertyFlags(CPF_ReturnParm))
						{
							SetterParameters.Add(Parameter);
						}
					}
				}
				if (Binding.DispatchMode != TEXT("cached_blueprint_setter")
					|| Binding.WritePolicy != TEXT("blueprint_setter")
					|| !IsAvidScriptRuntimeFunctionAllowed(BlueprintSetter)
					|| BlueprintSetter->GetOwnerClass() != OwnerClass
					|| BlueprintSetter->HasAnyFunctionFlags(FUNC_Static)
					|| BlueprintSetter->GetReturnProperty() != nullptr
					|| SetterParameters.Num() != 1)
				{
					SetAvidScriptBindingLoadFailure(
						OutResult,
						TEXT("binding_property_blueprint_setter_mismatch"),
						Binding.CanonicalIdentity,
						TEXT("The cached BlueprintSetter no longer matches the authorized property."));
					return false;
				}
			}

			FAvidScriptRuntimeBindingValuePlan PropertyValuePlan;
			FString ValueDetails;
			if (!BuildAvidScriptRuntimeValuePlan(
					Property,
					Binding.Parameters[0],
					DeclaredTypesByCanonical.FindRef(
						Binding.Parameters[0].CanonicalType),
					2,
					PropertyValuePlan,
					ValueDetails))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_contract_mismatch"),
					Binding.CanonicalIdentity,
					ValueDetails);
				return false;
			}

			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
			Plan.OwnerClass = OwnerClass;
			Plan.Function = BlueprintSetter;
			Plan.ReflectedProperty = Property;
			Plan.DebugPath = Property->GetPathName();
			Plan.bRequiresWriteAccess = true;
			Plan.ReloadEffect = EAvidScriptBindingReloadEffect::ReflectedProperty;
			if (BlueprintSetter != nullptr)
			{
				Plan.DebugPath += TEXT(":") + BlueprintSetter->GetName();
				Plan.FrameSize = BlueprintSetter->GetStructureSize();
				Plan.FrameAlignment = FMath::Max(
					1,
					BlueprintSetter->GetMinAlignment());
				if (Plan.FrameSize < BlueprintSetter->ParmsSize
					|| !FMath::IsPowerOfTwo(Plan.FrameAlignment)
					|| Plan.FrameSize > MAX_int32 - (Plan.FrameAlignment - 1))
				{
					SetAvidScriptBindingLoadFailure(
						OutResult,
						TEXT("binding_frame_layout_invalid"),
						Binding.CanonicalIdentity,
						TEXT("The BlueprintSetter frame size or alignment is invalid."));
					return false;
				}
				Plan.RequiredScratchSize = Plan.FrameSize
					+ Plan.FrameAlignment - 1;
				if (!BuildAvidScriptRuntimeValuePlan(
						SetterParameters[0],
						Binding.Parameters[0],
						DeclaredTypesByCanonical.FindRef(
							Binding.Parameters[0].CanonicalType),
						2,
						Plan.Parameters.AddDefaulted_GetRef(),
						ValueDetails))
				{
					SetAvidScriptBindingLoadFailure(
						OutResult,
						TEXT("binding_property_blueprint_setter_mismatch"),
						Binding.CanonicalIdentity,
						ValueDetails);
					return false;
				}
			}
			else
			{
				Plan.Parameters.Add(MoveTemp(PropertyValuePlan));
			}

			FString ReturnDetails;
			const int32 ReturnOffset = 2 + Plan.Parameters[0].ArgumentWidth;
			if (!BuildAvidScriptRuntimeValuePlan(
					nullptr,
					Binding.ReturnValue,
					nullptr,
					ReturnOffset,
					Plan.ReturnValue,
					ReturnDetails))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_return_contract_mismatch"),
					Binding.CanonicalIdentity,
					ReturnDetails);
				return false;
			}
			Plan.ExpectedArgumentCount = ReturnOffset;
			Plan.bRequiresGuestMemory =
				Plan.Parameters[0].Kind == EAvidScriptRuntimeBindingKind::Name;
			const FString ExpectedIdentity =
				MakeAvidScriptRuntimePropertySetCanonicalIdentity(
					OwnerClass,
					Property,
					Binding);
			if (Binding.CanonicalIdentity != ExpectedIdentity
				|| Binding.StableId
					!= FAvidScriptHash::Sha256HexUtf8(ExpectedIdentity)
				|| Binding.HostImport.Signature
					!= MakeAvidScriptRuntimeExpectedSignature(Binding))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_identity_mismatch"),
					Binding.CanonicalIdentity,
					TEXT("The writable property descriptor no longer matches the active reflection snapshot."));
				return false;
			}

			Package->Impl->RequiredScratchSize = FMath::Max(
				Package->Impl->RequiredScratchSize,
				Plan.RequiredScratchSize);
			Package->Impl->VmPackage.Imports.Add({
				Binding.StableId,
				static_cast<uint32>(Binding.Ordinal),
				Binding.HostImport.Module,
				Binding.HostImport.Name,
				Binding.HostImport.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
			continue;
		}
		if (Binding.BindingKind == TEXT("property_get"))
		{
			++Package->Impl->Instrumentation.ReflectedNameLookupCount;
			FProperty* Property = FindFProperty<FProperty>(OwnerClass, FName(*Binding.UeMember));
			if (!IsAvidScriptRuntimePropertyReadable(Property)
				|| Property->GetOwnerStruct() != OwnerClass)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_missing"),
					Binding.OwnerClass + TEXT(".") + Binding.UeMember,
					TEXT("The reflected property is missing or no longer satisfies runtime read policy."));
				return false;
			}

			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyRead;
			Plan.OwnerClass = OwnerClass;
			Plan.ReflectedProperty = Property;
			Plan.DebugPath = Property->GetPathName();
			Plan.bRequiresGuestMemory = true;
			Plan.ExpectedArgumentCount = 3;
			FString ReturnDetails;
			if (!BuildAvidScriptRuntimeValuePlan(
				Property,
				Binding.ReturnValue,
				DeclaredTypesByCanonical.FindRef(Binding.ReturnValue.CanonicalType),
				2,
				Plan.ReturnValue,
				ReturnDetails))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_contract_mismatch"),
					Binding.CanonicalIdentity,
					ReturnDetails);
				return false;
			}
			const FString ExpectedIdentity = MakeAvidScriptRuntimePropertyGetCanonicalIdentity(
				OwnerClass,
				Property,
				Binding);
			if (Binding.CanonicalIdentity != ExpectedIdentity
				|| Binding.StableId != FAvidScriptHash::Sha256HexUtf8(ExpectedIdentity)
				|| Binding.HostImport.Signature != TEXT("(iii)i"))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_identity_mismatch"),
					Binding.CanonicalIdentity,
					TEXT("The property descriptor no longer matches the active reflection snapshot."));
				return false;
			}

			Package->Impl->VmPackage.Imports.Add({
				Binding.StableId,
				static_cast<uint32>(Binding.Ordinal),
				Binding.HostImport.Module,
				Binding.HostImport.Name,
				Binding.HostImport.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
			continue;
		}

		++Package->Impl->Instrumentation.ReflectedNameLookupCount;
		UFunction* Function = OwnerClass->FindFunctionByName(FName(*Binding.UeFunction));
		if (!IsAvidScriptRuntimeFunctionAllowed(Function))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_function_missing"),
				Binding.OwnerClass + TEXT(".") + Binding.UeFunction,
				TEXT("The reflected function is missing or no longer satisfies runtime policy."));
			return false;
		}
		if (Binding.bStatic != Function->HasAnyFunctionFlags(FUNC_Static)
			|| Binding.bConst != Function->HasAnyFunctionFlags(FUNC_Const)
			|| Binding.HostImport.Signature != MakeAvidScriptRuntimeExpectedSignature(Binding))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_function_contract_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("Function flags or ABI signature changed since descriptor generation."));
			return false;
		}
		const bool bRuntimeReadOnly = Function->HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure);
		if ((Binding.ReloadEffect == EAvidScriptBindingReloadEffect::None && !bRuntimeReadOnly)
			|| ((Binding.ReloadEffect == EAvidScriptBindingReloadEffect::ActorTransform
					|| Binding.ReloadEffect == EAvidScriptBindingReloadEffect::SceneComponentTransform)
				&& bRuntimeReadOnly))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_reload_effect_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("The descriptor reload effect conflicts with the reflected function flags."));
			return false;
		}

		TArray<FProperty*> ReflectedParameters;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Parm)
				&& !Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReflectedParameters.Add(Property);
			}
		}
		if (ReflectedParameters.Num() != Binding.Parameters.Num())
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_parameter_count_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("Reflected parameter count changed since descriptor generation."));
			return false;
		}

		FAvidScriptRuntimeBindingInvocationPlan Plan;
		Plan.OwnerClass = OwnerClass;
		Plan.Function = Function;
		Plan.DebugPath = Function->GetPathName();
		Plan.bStatic = Binding.bStatic;
		Plan.ReloadEffect = Binding.ReloadEffect;
		Plan.bRequiresWriteAccess = Binding.ReloadEffect != EAvidScriptBindingReloadEffect::None;
		Plan.FrameSize = Function->GetStructureSize();
		Plan.FrameAlignment = FMath::Max(1, Function->GetMinAlignment());
		if (Plan.FrameSize < Function->ParmsSize
			|| !FMath::IsPowerOfTwo(Plan.FrameAlignment)
			|| Plan.FrameSize > MAX_int32 - (Plan.FrameAlignment - 1))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_frame_layout_invalid"),
				Binding.CanonicalIdentity,
				TEXT("The reflected function frame size or alignment is invalid."));
			return false;
		}
		Plan.RequiredScratchSize = Plan.FrameSize + Plan.FrameAlignment - 1;
		int32 ArgumentOffset = Binding.bStatic ? 0 : 2;
		for (int32 Index = 0; Index < Binding.Parameters.Num(); ++Index)
		{
			FProperty* Property = ReflectedParameters[Index];
			const FAvidScriptBindingValueModel& Parameter = Binding.Parameters[Index];
			if (Property->GetName() != Parameter.Name
				|| GetAvidScriptRuntimePropertyDirection(Property) != Parameter.Direction)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_parameter_contract_mismatch"),
					Binding.CanonicalIdentity + TEXT(":") + Parameter.Name,
					TEXT("Reflected parameter name or direction changed since descriptor generation."));
				return false;
			}
			FAvidScriptRuntimeBindingValuePlan ValuePlan;
			FString Details;
			if (!BuildAvidScriptRuntimeValuePlan(
				Property,
				Parameter,
				DeclaredTypesByCanonical.FindRef(Parameter.CanonicalType),
				ArgumentOffset,
				ValuePlan,
				Details))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_contract_mismatch"),
					Binding.CanonicalIdentity + TEXT(":") + Parameter.Name,
					Details);
				return false;
			}
			ArgumentOffset += ValuePlan.ArgumentWidth;
			Plan.bRequiresGuestMemory |= ValuePlan.Direction == EAvidScriptRuntimeBindingDirection::Ref
				|| ValuePlan.Direction == EAvidScriptRuntimeBindingDirection::Out
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::Name;
			Plan.Parameters.Add(MoveTemp(ValuePlan));
		}

		FProperty* ReturnProperty = Function->GetReturnProperty();
		FString ReturnDetails;
		if (!BuildAvidScriptRuntimeValuePlan(
			ReturnProperty,
			Binding.ReturnValue,
			DeclaredTypesByCanonical.FindRef(Binding.ReturnValue.CanonicalType),
			ArgumentOffset,
			Plan.ReturnValue,
			ReturnDetails))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_return_contract_mismatch"),
				Binding.CanonicalIdentity,
				ReturnDetails);
			return false;
		}
		ArgumentOffset += Plan.ReturnValue.ArgumentWidth;
		Plan.bRequiresGuestMemory |= Plan.ReturnValue.Kind != EAvidScriptRuntimeBindingKind::Void;
		Plan.ExpectedArgumentCount = ArgumentOffset;
		if (Binding.CanonicalIdentity != MakeAvidScriptRuntimeCanonicalIdentity(OwnerClass, Function, Binding)
			|| Binding.StableId != FAvidScriptHash::Sha256HexUtf8(Binding.CanonicalIdentity))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_identity_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("The descriptor identity no longer matches the active reflection snapshot."));
			return false;
		}

		TArray<
			UE::AvidScript::BindingPrivate::FFastPathValueSpec,
			TInlineAllocator<2>> FastPathParameters;
		FastPathParameters.Reserve(Plan.Parameters.Num());
		for (const FAvidScriptRuntimeBindingValuePlan& Parameter : Plan.Parameters)
		{
			FastPathParameters.Add({
				Parameter.Property,
				Parameter.ArgumentOffset,
				Parameter.Direction == EAvidScriptRuntimeBindingDirection::Value,
				Parameter.Kind == EAvidScriptRuntimeBindingKind::Int32
			});
		}
		UE::AvidScript::BindingPrivate::FFastPathBuildSpec FastPathSpec;
		FastPathSpec.Function = Plan.Function;
		FastPathSpec.FrameSize = Plan.FrameSize;
		FastPathSpec.FrameAlignment = Plan.FrameAlignment;
		FastPathSpec.ExpectedArgumentCount = Plan.ExpectedArgumentCount;
		FastPathSpec.bStatic = Plan.bStatic;
		FastPathSpec.bRequiresWriteAccess = Plan.bRequiresWriteAccess;
		FastPathSpec.bHasReloadEffect =
			Plan.ReloadEffect != EAvidScriptBindingReloadEffect::None;
		FastPathSpec.Parameters = FastPathParameters;
		FastPathSpec.ReturnValue = {
			Plan.ReturnValue.Property,
			Plan.ReturnValue.ArgumentOffset,
			false,
			Plan.ReturnValue.Kind == EAvidScriptRuntimeBindingKind::Int32
		};
		if (UE::AvidScript::BindingPrivate::TryBuildFastPath(
			FastPathSpec,
			Plan.FastPath))
		{
			++Package->Impl->Instrumentation.TypedThunkPlanCount;
		}
		else
		{
			++Package->Impl->Instrumentation.ReflectionFallbackPlanCount;
		}

		Package->Impl->RequiredScratchSize = FMath::Max(
			Package->Impl->RequiredScratchSize,
			Plan.RequiredScratchSize);
		Package->Impl->VmPackage.Imports.Add({
			Binding.StableId,
			static_cast<uint32>(Binding.Ordinal),
			Binding.HostImport.Module,
			Binding.HostImport.Name,
			Binding.HostImport.Signature
		});
		Package->Impl->Plans.Add(MoveTemp(Plan));
	}

	if (bHasLifecycleClassReferences)
	{
		for (const FAvidScriptObjectLifecycleBindingSpec& Spec : FAvidScriptObjectLifecycleBindings::GetSpecs())
		{
			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = Spec.Kind;
			Plan.DebugPath = Spec.ModuleName + TEXT(".") + Spec.ImportName;
			Plan.bRequiresWriteAccess = Spec.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor
				|| Spec.Kind == EAvidScriptBindingInvocationKind::ObjectDestroyActor;
			Plan.bRequiresGuestMemory = Spec.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor;
			switch (Spec.Kind)
			{
			case EAvidScriptBindingInvocationKind::ObjectSpawnActor:
				Plan.ExpectedArgumentCount = 3;
				break;
			case EAvidScriptBindingInvocationKind::ObjectDestroyActor:
				Plan.ExpectedArgumentCount = 2;
				break;
			case EAvidScriptBindingInvocationKind::ObjectIsA:
				Plan.ExpectedArgumentCount = 3;
				break;
			default:
				checkNoEntry();
				break;
			}

			Package->Impl->VmPackage.Imports.Add({
				Spec.StableId,
				static_cast<uint32>(Package->Impl->Plans.Num()),
				Spec.ModuleName,
				Spec.ImportName,
				Spec.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
		}
	}

	if (!Package->Impl->ObjectTypePlans.IsEmpty())
	{
		for (const FAvidScriptObjectTypeBindingSpec& Spec : FAvidScriptObjectTypeBindings::GetSpecs())
		{
			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = Spec.Kind;
			Plan.DebugPath = Spec.ModuleName + TEXT(".") + Spec.ImportName;
			Plan.ExpectedArgumentCount = 3;

			Package->Impl->VmPackage.Imports.Add({
				Spec.StableId,
				static_cast<uint32>(Package->Impl->Plans.Num()),
				Spec.ModuleName,
				Spec.ImportName,
				Spec.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
		}
	}

	if (ObjectFactoryBindingCount > 0)
	{
		for (const FAvidScriptObjectFactoryBindingSpec& Spec :
			FAvidScriptObjectFactoryBinding::GetSpecs())
		{
			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = Spec.Kind;
			Plan.DebugPath = Spec.ModuleName + TEXT(".") + Spec.ImportName;
			Plan.bRequiresWriteAccess =
				Spec.Kind == EAvidScriptBindingInvocationKind::ObjectConstruct
				|| Spec.Kind == EAvidScriptBindingInvocationKind::ObjectRelease;
			Plan.ExpectedArgumentCount = Spec.Kind
				== EAvidScriptBindingInvocationKind::ObjectRelease
				? 2
				: 3;

			Package->Impl->VmPackage.Imports.Add({
				Spec.StableId,
				static_cast<uint32>(Package->Impl->Plans.Num()),
				Spec.ModuleName,
				Spec.ImportName,
				Spec.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
		}
	}

	if (SceneAttachmentBindingCount > 0)
	{
		for (const FAvidScriptSceneAttachmentBindingSpec& Spec :
			FAvidScriptSceneAttachmentBinding::GetSpecs())
		{
			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = Spec.Kind;
			Plan.DebugPath = Spec.ModuleName + TEXT(".") + Spec.ImportName;
			Plan.bRequiresWriteAccess = true;
			Plan.ExpectedArgumentCount = Spec.Kind
				== EAvidScriptBindingInvocationKind::SceneComponentAttach
				? 5
				: 3;

			Package->Impl->VmPackage.Imports.Add({
				Spec.StableId,
				static_cast<uint32>(Package->Impl->Plans.Num()),
				Spec.ModuleName,
				Spec.ImportName,
				Spec.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
		}
	}

	OutResult.bSucceeded = true;
	OutResult.BindingCount = Model.Bindings.Num();
	OutResult.ClassReferenceCount = Package->Impl->ClassReferencePlans.Num();
	OutResult.ObjectFactoryCount = Package->Impl->ObjectFactoryPlans.Num();
	OutResult.RequiredScratchSize = Package->Impl->RequiredScratchSize;
	OutResult.PackageName = Package->Impl->PackageName;
	OutResult.PackageHash = Package->Impl->PackageHash;
	OutPackage = Package;
	return true;
}

const FString& FAvidScriptBindingPackage::GetPackageName() const
{
	return Impl->PackageName;
}

const FString& FAvidScriptBindingPackage::GetPackageHash() const
{
	return Impl->PackageHash;
}

int32 FAvidScriptBindingPackage::GetDescriptorSchemaVersion() const
{
	return Impl->DescriptorSchemaVersion;
}

const FAvidScriptVmBindingPackage& FAvidScriptBindingPackage::GetVmPackage() const
{
	return Impl->VmPackage;
}

const FAvidScriptBindingPackageInstrumentation& FAvidScriptBindingPackage::GetInstrumentation() const
{
	return Impl->Instrumentation;
}

bool FAvidScriptBindingPackage::TryGetFastPathKind(
	const uint32 Ordinal,
	EAvidScriptBindingFastPathKind& OutKind) const
{
	OutKind = EAvidScriptBindingFastPathKind::None;
	if (!Impl->Plans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	OutKind = Impl->Plans[Ordinal].FastPath.Kind;
	return true;
}

int32 FAvidScriptBindingPackage::GetRequiredScratchSize() const
{
	return Impl->RequiredScratchSize;
}

int32 FAvidScriptBindingPackage::GetObjectTypeCount() const
{
	return Impl->ObjectTypePlans.Num();
}

bool FAvidScriptBindingPackage::TryResolveObjectType(
	const uint32 Ordinal,
	UClass*& OutClass) const
{
	OutClass = nullptr;
	if (!Impl->ObjectTypePlans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	OutClass = Impl->ObjectTypePlans[static_cast<int32>(Ordinal)];
	return OutClass != nullptr;
}

UClass* FAvidScriptBindingPackage::GetExpectedSelfClass() const
{
	return Impl->ExpectedSelfClass;
}

int32 FAvidScriptBindingPackage::GetClassReferenceCount() const
{
	return Impl->ClassReferencePlans.Num();
}

bool FAvidScriptBindingPackage::TryResolveClassReference(
	const uint32 Ordinal,
	UClass*& OutClass,
	UClass*& OutBaseClass) const
{
	OutClass = nullptr;
	OutBaseClass = nullptr;
	if (!Impl->ClassReferencePlans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	const FImpl::FClassReferencePlan& Plan =
		Impl->ClassReferencePlans[static_cast<int32>(Ordinal)];
	OutClass = Plan.Class;
	OutBaseClass = Plan.BaseClass;
	return OutClass != nullptr && OutBaseClass != nullptr;
}

int32 FAvidScriptBindingPackage::GetObjectFactoryCount() const
{
	return Impl->ObjectFactoryPlans.Num();
}

bool FAvidScriptBindingPackage::TryResolveObjectFactory(
	const uint32 Ordinal,
	const FAvidScriptObjectFactoryPlan*& OutPlan) const
{
	OutPlan = nullptr;
	if (!Impl->ObjectFactoryPlans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	OutPlan = &Impl->ObjectFactoryPlans[static_cast<int32>(Ordinal)];
	return OutPlan->ObjectClass != nullptr
		&& OutPlan->RequiredOuterClass != nullptr
		&& OutPlan->ResultObjectTypeOrdinal != INDEX_NONE;
}

bool FAvidScriptBindingPackage::Dispatch(
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	TArray<uint8>& InvocationScratch,
	FAvidScriptDynamicHostCallResult& OutResult) const
{
	OutResult = FAvidScriptDynamicHostCallResult();
	if (!Impl->Plans.IsValidIndex(static_cast<int32>(Call.BindingOrdinal)))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_ordinal_invalid"),
			FString::FromInt(Call.BindingOrdinal),
			TEXT("The VM binding ordinal is outside the attached package."));
		return false;
	}
	const FAvidScriptRuntimeBindingInvocationPlan& Plan = Impl->Plans[Call.BindingOrdinal];
	if (Call.Arguments.Num() != Plan.ExpectedArgumentCount
		|| (Plan.bRequiresGuestMemory && Call.GuestMemory == nullptr))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_frame_mismatch"),
			Plan.DebugPath,
			TEXT("The raw argument count or guest memory contract does not match the cached invocation plan."));
		return false;
	}
	if (InvocationScratch.Num() < Plan.RequiredScratchSize)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_scratch_too_small"),
			Plan.DebugPath,
			TEXT("The runtime did not preallocate the package's required invocation scratch size."));
		return false;
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor
		|| Plan.Kind == EAvidScriptBindingInvocationKind::ObjectDestroyActor
		|| Plan.Kind == EAvidScriptBindingInvocationKind::ObjectIsA)
	{
		return DispatchAvidScriptObjectLifecycle(*this, Plan, Call, Context, OutResult);
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectTypeIsA)
	{
		return DispatchAvidScriptObjectType(*this, Plan, Call, Context, OutResult);
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectConstruct
		|| Plan.Kind == EAvidScriptBindingInvocationKind::ObjectRelease
		|| Plan.Kind == EAvidScriptBindingInvocationKind::ActorFindComponent)
	{
		return DispatchAvidScriptObjectFactory(*this, Plan, Call, Context, OutResult);
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::SceneComponentAttach
		|| Plan.Kind == EAvidScriptBindingInvocationKind::SceneComponentDetach)
	{
		return DispatchAvidScriptSceneAttachment(Plan, Call, Context, OutResult);
	}

	UObject* Target = nullptr;
	FString Details;
	if (Plan.bStatic)
	{
		Target = Plan.OwnerClass->GetDefaultObject();
	}
	else if (!ResolveAvidScriptRuntimeHandle(
		static_cast<uint32>(Call.Arguments[0]),
		static_cast<uint32>(Call.Arguments[1]),
		Plan.OwnerClass,
		Context,
		false,
		Target,
		Details))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			Details);
		return false;
	}
	if (Target == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			TEXT("The cached invocation target is null."));
		return false;
	}
	if (Plan.bRequiresWriteAccess && Context.WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_write_denied"),
			Plan.DebugPath,
			TEXT("The reflected binding requires an explicitly writable host context."));
		return false;
	}
	const auto PrepareHostEffect = [&]()
	{
		if (Context.HostEffectJournal == nullptr || !Plan.bRequiresWriteAccess)
		{
			return true;
		}
		if (Plan.Kind == EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
			&& Plan.Function != nullptr)
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_reload_effect_unsupported"),
				Plan.DebugPath,
				TEXT("BlueprintSetter candidate reload is not reversible because ProcessEvent may produce additional host effects."));
			return false;
		}
		if (Plan.ReloadEffect == EAvidScriptBindingReloadEffect::Unsupported)
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_reload_effect_unsupported"),
				Plan.DebugPath,
				TEXT("The reflected write has no reversible candidate reload adapter."));
			return false;
		}
		if (Context.ObjectRegistry == nullptr)
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_host_effect_registry_missing"),
				Plan.DebugPath,
				TEXT("The candidate host effect journal requires an object registry."));
			return false;
		}

		const FAvidScriptObjectHandle EffectHandle = Plan.bStatic
			? Context.OwnerHandle
			: FAvidScriptObjectHandle{
				static_cast<uint32>(Call.Arguments[0]),
				static_cast<uint32>(Call.Arguments[1])
			};
		FAvidScriptBindingHostEffectPrepareResult PrepareResult;
		const bool bPrepared = Plan.ReloadEffect
			== EAvidScriptBindingReloadEffect::ReflectedProperty
			? Plan.ReflectedProperty != nullptr
				&& Context.HostEffectJournal->PrepareReflectedProperty(
					*Context.ObjectRegistry,
					EffectHandle,
					*Target,
					*Plan.ReflectedProperty,
					PrepareResult)
			: Context.HostEffectJournal->PrepareEffect(
				*Context.ObjectRegistry,
				EffectHandle,
				*Target,
				Plan.ReloadEffect,
				PrepareResult);
		if (!bPrepared)
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				PrepareResult.ErrorCategory.IsEmpty()
					? FString(TEXT("binding_host_effect_prepare_failed"))
					: PrepareResult.ErrorCategory,
				PrepareResult.ErrorSource.IsEmpty()
					? Plan.DebugPath
					: PrepareResult.ErrorSource,
				PrepareResult.ErrorDetails.IsEmpty()
					? FString(TEXT("The candidate host effect could not be prepared."))
					: PrepareResult.ErrorDetails);
			return false;
		}
		return true;
	};
	if (Plan.Kind == EAvidScriptBindingInvocationKind::ReflectedPropertyRead)
	{
		if (!WriteAvidScriptRuntimeValueToGuest(
			Plan.ReturnValue,
			static_cast<uint32>(Call.Arguments[Plan.ReturnValue.ArgumentOffset]),
			*Call.GuestMemory,
			Context,
			Target,
			Details))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_property_read_failed"),
				Plan.DebugPath,
				Details);
			return false;
		}
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
		&& Plan.Function == nullptr)
	{
		if (!PrepareHostEffect()
			|| !SetAvidScriptRuntimeValueFromCells(
				Plan.Parameters[0],
				Call.Arguments.Slice(
					Plan.Parameters[0].ArgumentOffset,
					Plan.Parameters[0].ArgumentWidth),
				Call.GuestMemory,
				Context,
				Target,
				Details))
		{
			if (OutResult.Details.IsEmpty())
			{
				SetAvidScriptBindingDispatchFailure(
					OutResult,
					TEXT("binding_property_write_failed"),
					Plan.DebugPath,
					Details);
			}
			return false;
		}
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}

	if (Plan.FastPath.IsBound())
	{
		if (!PrepareHostEffect())
		{
			return false;
		}
		FString FastPathErrorCategory;
		FString FastPathErrorDetails;
		if (!UE::AvidScript::BindingPrivate::DispatchFastPath(
			Plan.FastPath,
			*Target,
			Call,
			InvocationScratch,
			FastPathErrorCategory,
			FastPathErrorDetails))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				FastPathErrorCategory.IsEmpty()
					? FString(TEXT("binding_fast_path_failed"))
					: FastPathErrorCategory,
				Plan.DebugPath,
				FastPathErrorDetails.IsEmpty()
					? FString(TEXT("The cached typed thunk rejected the invocation."))
					: FastPathErrorDetails);
			return false;
		}
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}

	const UPTRINT ScratchAddress = reinterpret_cast<UPTRINT>(InvocationScratch.GetData());
	const UPTRINT FrameAddress = Align(ScratchAddress, static_cast<UPTRINT>(Plan.FrameAlignment));
	const int32 FrameOffset = static_cast<int32>(FrameAddress - ScratchAddress);
	if (FrameOffset + Plan.FrameSize > InvocationScratch.Num())
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_scratch_alignment_failed"),
			Plan.DebugPath,
			TEXT("The runtime invocation scratch could not satisfy the cached frame alignment."));
		return false;
	}
	void* Frame = reinterpret_cast<void*>(FrameAddress);
	Plan.Function->InitializeStruct(Frame);
	ON_SCOPE_EXIT
	{
		Plan.Function->DestroyStruct(Frame);
	};

	for (const FAvidScriptRuntimeBindingValuePlan& Parameter : Plan.Parameters)
	{
		if (Parameter.Direction == EAvidScriptRuntimeBindingDirection::Ref
			|| Parameter.Direction == EAvidScriptRuntimeBindingDirection::Out)
		{
			if (Parameter.Direction == EAvidScriptRuntimeBindingDirection::Ref
				&& !SetAvidScriptRuntimeValueFromGuest(
					Parameter,
					static_cast<uint32>(Call.Arguments[Parameter.ArgumentOffset]),
					*Call.GuestMemory,
					Context,
					Frame,
					Details))
			{
				SetAvidScriptBindingDispatchFailure(
					OutResult,
					TEXT("binding_guest_read_failed"),
					Plan.DebugPath + TEXT(":") + Parameter.Name,
					Details);
				return false;
			}
			continue;
		}

		if (!SetAvidScriptRuntimeValueFromCells(
			Parameter,
			Call.Arguments.Slice(Parameter.ArgumentOffset, Parameter.ArgumentWidth),
			Call.GuestMemory,
			Context,
			Frame,
			Details))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_argument_invalid"),
				Plan.DebugPath + TEXT(":") + Parameter.Name,
				Details);
			return false;
		}
	}

	if (!PrepareHostEffect())
	{
		return false;
	}

	Target->ProcessEvent(Plan.Function, Frame);

	for (const FAvidScriptRuntimeBindingValuePlan& Parameter : Plan.Parameters)
	{
		if ((Parameter.Direction == EAvidScriptRuntimeBindingDirection::Ref
			|| Parameter.Direction == EAvidScriptRuntimeBindingDirection::Out)
			&& !WriteAvidScriptRuntimeValueToGuest(
				Parameter,
				static_cast<uint32>(Call.Arguments[Parameter.ArgumentOffset]),
				*Call.GuestMemory,
				Context,
				Frame,
				Details))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_guest_write_failed"),
				Plan.DebugPath + TEXT(":") + Parameter.Name,
				Details);
			return false;
		}
	}

	if (Plan.ReturnValue.Kind != EAvidScriptRuntimeBindingKind::Void
		&& !WriteAvidScriptRuntimeValueToGuest(
			Plan.ReturnValue,
			static_cast<uint32>(Call.Arguments[Plan.ReturnValue.ArgumentOffset]),
			*Call.GuestMemory,
			Context,
			Frame,
			Details))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_return_write_failed"),
			Plan.DebugPath,
			Details);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ReturnValue = 1;
	return true;
}
