#include "Invocation/AvidScriptBindingCodecProgram.h"

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectOwnership.h"
#include "Containers/StringConv.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::AvidScript::BindingPrivate
{
namespace
{
bool ReadFiniteF32(const uint64 Cell, float& OutValue)
{
	const uint32 Bits = static_cast<uint32>(Cell);
	FMemory::Memcpy(&OutValue, &Bits, sizeof(OutValue));
	return FMath::IsFinite(OutValue);
}

bool ReadFiniteF64(const uint64 Cell, double& OutValue)
{
	FMemory::Memcpy(&OutValue, &Cell, sizeof(OutValue));
	return FMath::IsFinite(OutValue);
}

bool SetNumericValue(
	const FValueCodecProgram& Program,
	void* Frame,
	const uint64 Cell,
	FString& OutDetails)
{
	void* Value = Program.Property->ContainerPtrToValuePtr<void>(Frame);
	if (Program.Kind == EValueCodecKind::Bool)
	{
		CastFieldChecked<FBoolProperty>(Program.Property)->SetPropertyValue(
			Value,
			static_cast<uint32>(Cell) != 0);
		return true;
	}
	if (Program.Kind == EValueCodecKind::Float)
	{
		float Number = 0.0f;
		if (!ReadFiniteF32(Cell, Number))
		{
			OutDetails = TEXT("The binding call supplied a non-finite float.");
			return false;
		}
		CastFieldChecked<FFloatProperty>(Program.Property)->SetPropertyValue(
			Value,
			Number);
		return true;
	}
	if (Program.Kind == EValueCodecKind::Double)
	{
		double Number = 0.0;
		if (!ReadFiniteF64(Cell, Number))
		{
			OutDetails = TEXT("The binding call supplied a non-finite double.");
			return false;
		}
		CastFieldChecked<FDoubleProperty>(Program.Property)->SetPropertyValue(
			Value,
			Number);
		return true;
	}

	FNumericProperty* NumericProperty = CastField<FNumericProperty>(Program.Property);
	if (Program.Kind == EValueCodecKind::Enum)
	{
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Program.Property))
		{
			NumericProperty = EnumProperty->GetUnderlyingProperty();
		}
	}
	if (NumericProperty == nullptr)
	{
		OutDetails = TEXT("The cached numeric property program is invalid.");
		return false;
	}

	uint64 ValueBits = Cell;
	switch (Program.Kind)
	{
	case EValueCodecKind::Int8:
		ValueBits = static_cast<uint64>(
			static_cast<int64>(static_cast<int8>(Cell)));
		break;
	case EValueCodecKind::UInt8:
		ValueBits = static_cast<uint8>(Cell);
		break;
	case EValueCodecKind::Int16:
		ValueBits = static_cast<uint64>(
			static_cast<int64>(static_cast<int16>(Cell)));
		break;
	case EValueCodecKind::UInt16:
		ValueBits = static_cast<uint16>(Cell);
		break;
	case EValueCodecKind::Int32:
	case EValueCodecKind::Enum:
		ValueBits = static_cast<uint64>(
			static_cast<int64>(static_cast<int32>(Cell)));
		break;
	case EValueCodecKind::UInt32:
		ValueBits = static_cast<uint32>(Cell);
		break;
	case EValueCodecKind::Int64:
		ValueBits = static_cast<uint64>(static_cast<int64>(Cell));
		break;
	case EValueCodecKind::UInt64:
		break;
	default:
		OutDetails = TEXT("The cached numeric kind is unsupported.");
		return false;
	}
	NumericProperty->SetIntPropertyValue(Value, ValueBits);
	return true;
}

bool SetNameValue(
	const FValueCodecProgram& Program,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	void* Frame,
	FString& OutDetails)
{
	uint8 LengthBytes[sizeof(int32)] = {};
	if (GuestAddress > MAX_uint32 - sizeof(LengthBytes)
		|| !GuestMemory.ReadBytes(
			GuestAddress,
			MakeArrayView(LengthBytes),
			OutDetails))
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
	const uint64 PayloadAddress64 =
		static_cast<uint64>(GuestAddress) + sizeof(LengthBytes);
	if (PayloadAddress64 + StoredSize > static_cast<uint64>(MAX_uint32) + 1)
	{
		OutDetails = TEXT("The FName payload address overflows guest memory.");
		return false;
	}

	TArray<uint8, TInlineAllocator<256>> Payload;
	Payload.SetNumUninitialized(StoredSize);
	if (!GuestMemory.ReadBytes(
			static_cast<uint32>(PayloadAddress64),
			MakeArrayView(Payload),
			OutDetails))
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
			&& FMemory::Memcmp(
				RoundTrip.Get(),
				Payload.GetData(),
				PayloadLength) != 0))
	{
		OutDetails = TEXT("The FName payload is not canonical valid UTF-8.");
		return false;
	}

	void* Value = Program.Property->ContainerPtrToValuePtr<void>(Frame);
	const FName Name = Converted.Length() == 0
		? NAME_None
		: FName(Converted.Length(), Converted.Get(), FNAME_Add);
	CastFieldChecked<FNameProperty>(Program.Property)->SetPropertyValue(
		Value,
		Name);
	return true;
}
} // namespace

bool ResolveObjectHandle(
	const uint32 Slot,
	const uint32 Generation,
	UClass* ExpectedClass,
	const FAvidScriptBindingInvocationContext& Context,
	const bool bAllowNull,
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
		OutDetails =
			TEXT("The binding call supplied an invalid UObject handle or has no object registry.");
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

bool SetValueFromCells(
	const FValueCodecProgram& Program,
	const TConstArrayView<uint64> Cells,
	IAvidScriptVmGuestMemory* GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails)
{
	if (Cells.Num() != Program.ArgumentWidth || Program.Property == nullptr)
	{
		OutDetails =
			TEXT("The dynamic binding frame width does not match the cached value program.");
		return false;
	}
	if (Program.Kind <= EValueCodecKind::Enum)
	{
		return SetNumericValue(Program, Frame, Cells[0], OutDetails);
	}
	if (Program.Kind == EValueCodecKind::Name)
	{
		if (GuestMemory == nullptr)
		{
			OutDetails = TEXT("The FName input requires guest memory.");
			return false;
		}
		if (Cells[0] > MAX_uint32)
		{
			OutDetails =
				TEXT("The FName guest address does not fit the 32-bit guest address space.");
			return false;
		}
		return SetNameValue(
			Program,
			static_cast<uint32>(Cells[0]),
			*GuestMemory,
			Frame,
			OutDetails);
	}
	if (Program.Kind == EValueCodecKind::Object)
	{
		UObject* Object = nullptr;
		if (!ResolveObjectHandle(
				static_cast<uint32>(Cells[0]),
				static_cast<uint32>(Cells[1]),
				Program.ObjectClass,
				Context,
				true,
				Object,
				OutDetails))
		{
			return false;
		}
		CastFieldChecked<FObjectPropertyBase>(Program.Property)
			->SetObjectPropertyValue_InContainer(Frame, Object);
		return true;
	}

	TArray<float, TInlineAllocator<9>> Components;
	Components.SetNumUninitialized(Cells.Num());
	for (int32 Index = 0; Index < Cells.Num(); ++Index)
	{
		if (!ReadFiniteF32(Cells[Index], Components[Index]))
		{
			OutDetails =
				TEXT("The binding call supplied a non-finite struct component.");
			return false;
		}
	}
	void* Value = Program.Property->ContainerPtrToValuePtr<void>(Frame);
	if (Program.Kind == EValueCodecKind::Vector)
	{
		*static_cast<FVector*>(Value) =
			FVector(Components[0], Components[1], Components[2]);
		return true;
	}
	if (Program.Kind == EValueCodecKind::Rotator)
	{
		*static_cast<FRotator*>(Value) =
			FRotator(Components[0], Components[1], Components[2]);
		return true;
	}
	if (Program.Kind == EValueCodecKind::Transform)
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

bool SetValueFromGuest(
	const FValueCodecProgram& Program,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails)
{
	uint8 Bytes[36] = {};
	if (Program.GuestStorageSize <= 0
		|| Program.GuestStorageSize > UE_ARRAY_COUNT(Bytes)
		|| !GuestMemory.ReadBytes(
			GuestAddress,
			MakeArrayView(Bytes, Program.GuestStorageSize),
			OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The cached guest storage size is invalid.");
		}
		return false;
	}

	TArray<uint64, TInlineAllocator<9>> Cells;
	if (Program.Kind == EValueCodecKind::Object)
	{
		uint32 Slot = 0;
		uint32 Generation = 0;
		FMemory::Memcpy(&Slot, Bytes, sizeof(Slot));
		FMemory::Memcpy(
			&Generation,
			Bytes + sizeof(Slot),
			sizeof(Generation));
		Cells = { Slot, Generation };
	}
	else if (Program.Kind == EValueCodecKind::Vector
		|| Program.Kind == EValueCodecKind::Rotator
		|| Program.Kind == EValueCodecKind::Transform)
	{
		const int32 ComponentCount =
			Program.GuestStorageSize / sizeof(float);
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			uint32 Bits = 0;
			FMemory::Memcpy(
				&Bits,
				Bytes + Index * sizeof(float),
				sizeof(Bits));
			Cells.Add(Bits);
		}
	}
	else
	{
		uint64 Cell = 0;
		FMemory::Memcpy(&Cell, Bytes, Program.GuestStorageSize);
		Cells.Add(Cell);
	}

	FValueCodecProgram ValueProgram = Program;
	ValueProgram.ArgumentWidth = Cells.Num();
	return SetValueFromCells(
		ValueProgram,
		Cells,
		&GuestMemory,
		Context,
		Frame,
		OutDetails);
}

bool WriteValueToGuest(
	const FValueCodecProgram& Program,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails)
{
	uint8 Bytes[36] = {};
	const void* Value = Program.Property->ContainerPtrToValuePtr<void>(Frame);
	if (Program.Kind == EValueCodecKind::Bool)
	{
		const int32 Stored = CastFieldChecked<FBoolProperty>(Program.Property)
			->GetPropertyValue(Value)
			? 1
			: 0;
		FMemory::Memcpy(Bytes, &Stored, sizeof(Stored));
	}
	else if (Program.Kind == EValueCodecKind::Float)
	{
		const float Stored = CastFieldChecked<FFloatProperty>(Program.Property)
			->GetPropertyValue(Value);
		FMemory::Memcpy(Bytes, &Stored, sizeof(Stored));
	}
	else if (Program.Kind == EValueCodecKind::Double)
	{
		const double Stored = CastFieldChecked<FDoubleProperty>(Program.Property)
			->GetPropertyValue(Value);
		FMemory::Memcpy(Bytes, &Stored, sizeof(Stored));
	}
	else if (Program.Kind == EValueCodecKind::Object)
	{
		UObject* Object = CastFieldChecked<FObjectPropertyBase>(Program.Property)
			->GetObjectPropertyValue_InContainer(Frame);
		FAvidScriptObjectHandle Handle;
		if (Object != nullptr)
		{
			if (Context.ObjectRegistry == nullptr
				|| Context.ObjectOwnership == nullptr)
			{
				OutDetails = TEXT(
					"The binding call cannot publish a UObject result without registry and ownership services.");
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
		FMemory::Memcpy(
			Bytes + sizeof(Handle.Slot),
			&Handle.Generation,
			sizeof(Handle.Generation));
	}
	else if (Program.Kind == EValueCodecKind::Vector)
	{
		const FVector& Stored = *static_cast<const FVector*>(Value);
		const float Components[3] = {
			static_cast<float>(Stored.X),
			static_cast<float>(Stored.Y),
			static_cast<float>(Stored.Z)
		};
		FMemory::Memcpy(Bytes, Components, sizeof(Components));
	}
	else if (Program.Kind == EValueCodecKind::Rotator)
	{
		const FRotator& Stored = *static_cast<const FRotator*>(Value);
		const float Components[3] = {
			static_cast<float>(Stored.Pitch),
			static_cast<float>(Stored.Yaw),
			static_cast<float>(Stored.Roll)
		};
		FMemory::Memcpy(Bytes, Components, sizeof(Components));
	}
	else if (Program.Kind == EValueCodecKind::Transform)
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
		const FNumericProperty* NumericProperty =
			CastField<FNumericProperty>(Program.Property);
		if (Program.Kind == EValueCodecKind::Enum)
		{
			if (const FEnumProperty* EnumProperty =
					CastField<FEnumProperty>(Program.Property))
			{
				NumericProperty = EnumProperty->GetUnderlyingProperty();
			}
		}
		if (NumericProperty == nullptr)
		{
			OutDetails =
				TEXT("The cached numeric output property program is invalid.");
			return false;
		}
		const uint64 Stored =
			NumericProperty->GetUnsignedIntPropertyValue(Value);
		FMemory::Memcpy(Bytes, &Stored, Program.GuestStorageSize);
	}

	return GuestMemory.WriteBytes(
		GuestAddress,
		MakeArrayView(
			static_cast<const uint8*>(Bytes),
			Program.GuestStorageSize),
		OutDetails);
}
} // namespace UE::AvidScript::BindingPrivate
