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

bool ReadLinearUtf8Payload(
	const TCHAR* ValueLabel,
	const uint32 GuestAddress,
	const uint32 MaxUtf8Bytes,
	IAvidScriptVmGuestMemory& GuestMemory,
	TConstArrayView<uint8>& OutPayload,
	TArray<uint8, TInlineAllocator<256>>& Storage,
	FString& OutDetails)
{
	OutPayload = TConstArrayView<uint8>();
	uint8 LengthBytes[sizeof(int32)] = {};
	if (GuestAddress > MAX_uint32 - sizeof(LengthBytes)
		|| !GuestMemory.ReadBytes(
			GuestAddress,
			MakeArrayView(LengthBytes),
			OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = FString::Printf(
				TEXT("The %s length prefix is outside guest memory."),
				ValueLabel);
		}
		return false;
	}

	const uint32 UnsignedLength = static_cast<uint32>(LengthBytes[0])
		| (static_cast<uint32>(LengthBytes[1]) << 8)
		| (static_cast<uint32>(LengthBytes[2]) << 16)
		| (static_cast<uint32>(LengthBytes[3]) << 24);
	if (UnsignedLength > MaxUtf8Bytes)
	{
		OutDetails = FString::Printf(
			TEXT("The %s UTF-8 byte length must be between 0 and %u."),
			ValueLabel,
			MaxUtf8Bytes);
		return false;
	}
	const uint32 StoredSize = UnsignedLength + 1u;
	const uint64 PayloadAddress64 =
		static_cast<uint64>(GuestAddress) + sizeof(LengthBytes);
	if (PayloadAddress64 + StoredSize > static_cast<uint64>(MAX_uint32) + 1)
	{
		OutDetails = FString::Printf(
			TEXT("The %s payload address overflows guest memory."),
			ValueLabel);
		return false;
	}

	Storage.SetNumUninitialized(static_cast<int32>(StoredSize));
	if (!GuestMemory.ReadBytes(
			static_cast<uint32>(PayloadAddress64),
			MakeArrayView(Storage),
			OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = FString::Printf(
				TEXT("The %s payload is outside guest memory."),
				ValueLabel);
		}
		return false;
	}
	if (Storage[static_cast<int32>(UnsignedLength)] != 0)
	{
		OutDetails = FString::Printf(
			TEXT("The %s payload is not followed by a zero terminator."),
			ValueLabel);
		return false;
	}
	OutPayload = MakeArrayView(Storage).Left(
		static_cast<int32>(UnsignedLength));
	return true;
}

bool ResolveUtf8Payload(
	const TCHAR* ValueLabel,
	const uint32 ValueReference,
	const uint32 MaxUtf8Bytes,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	TConstArrayView<uint8>& OutPayload,
	TArray<uint8, TInlineAllocator<256>>& Storage,
	FString& OutDetails)
{
	if (!FAvidScriptUtf8ValueHeap::IsHeapToken(ValueReference))
	{
		return ReadLinearUtf8Payload(
			ValueLabel,
			ValueReference,
			MaxUtf8Bytes,
			GuestMemory,
			OutPayload,
			Storage,
			OutDetails);
	}
	if (Context.Utf8ValueHeap == nullptr)
	{
		OutDetails = FString::Printf(
			TEXT("The %s token has no UTF-8 value heap in this runtime session."),
			ValueLabel);
		return false;
	}
	if (!Context.Utf8ValueHeap->Resolve(
			ValueReference,
			OutPayload,
			OutDetails))
	{
		return false;
	}
	if (OutPayload.Num() < 0
		|| static_cast<uint32>(OutPayload.Num()) > MaxUtf8Bytes)
	{
		OutDetails = FString::Printf(
			TEXT("The %s heap value exceeds its UTF-8 byte limit."),
			ValueLabel);
		return false;
	}
	return true;
}

bool SetUtf8Value(
	const FValueCodecProgram& Program,
	const uint32 ValueReference,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails)
{
	const bool bName = Program.Kind == EValueCodecKind::Name;
	if ((!bName && Program.Kind != EValueCodecKind::String)
		|| Program.Property == nullptr)
	{
		OutDetails = TEXT("The cached UTF-8 value program is invalid.");
		return false;
	}
	const TCHAR* ValueLabel = bName ? TEXT("FName") : TEXT("FString");
	const uint32 MaxUtf8Bytes = bName
		? static_cast<uint32>(NAME_SIZE * 4)
		: FAvidScriptUtf8ValueHeap::MaxValueBytes;
	TConstArrayView<uint8> Payload;
	TArray<uint8, TInlineAllocator<256>> Storage;
	if (!ResolveUtf8Payload(
			ValueLabel,
			ValueReference,
			MaxUtf8Bytes,
			GuestMemory,
			Context,
			Payload,
			Storage,
			OutDetails))
	{
		return false;
	}
	if (bName)
	{
		for (const uint8 Byte : Payload)
		{
			if (Byte == 0)
			{
				OutDetails = TEXT("The FName UTF-8 payload contains an embedded NUL byte.");
				return false;
			}
		}
	}

	static constexpr ANSICHAR Empty[] = "";
	const ANSICHAR* Utf8 = Payload.IsEmpty()
		? Empty
		: reinterpret_cast<const ANSICHAR*>(Payload.GetData());
	const FUTF8ToTCHAR Converted(Utf8, Payload.Num());
	if (bName && Converted.Length() >= NAME_SIZE)
	{
		OutDetails = FString::Printf(
			TEXT("The decoded FName must contain fewer than %d TCHAR code units."),
			NAME_SIZE);
		return false;
	}
	const FTCHARToUTF8 RoundTrip(Converted.Get(), Converted.Length());
	if (RoundTrip.Length() != Payload.Num()
		|| (!Payload.IsEmpty()
			&& FMemory::Memcmp(
				RoundTrip.Get(),
				Payload.GetData(),
				Payload.Num()) != 0))
	{
		OutDetails = FString::Printf(
			TEXT("The %s payload is not canonical valid UTF-8."),
			ValueLabel);
		return false;
	}

	void* Value = Program.Property->ContainerPtrToValuePtr<void>(Frame);
	if (bName)
	{
		const FName Name = Converted.Length() == 0
			? NAME_None
			: FName(Converted.Length(), Converted.Get(), FNAME_Add);
		CastFieldChecked<FNameProperty>(Program.Property)->SetPropertyValue(
			Value,
			Name);
	}
	else
	{
		CastFieldChecked<FStrProperty>(Program.Property)->SetPropertyValue(
			Value,
			FString(Converted.Length(), Converted.Get()));
	}
	return true;
}

bool DecodeWireValue(
	const FValueCodecProgram& Program,
	const TConstArrayView<uint8> Wire,
	const FAvidScriptBindingInvocationContext& Context,
	void* Container,
	FString& OutDetails);

bool DecodeStructChildren(
	const FValueCodecProgram& Program,
	const TConstArrayView<uint8> Wire,
	const FAvidScriptBindingInvocationContext& Context,
	void* StructValue,
	FString& OutDetails)
{
	if (Program.Kind != EValueCodecKind::StructWire
		|| Program.StructType == nullptr
		|| Wire.Num() != Program.WireSize)
	{
		OutDetails = TEXT("The cached struct wire decode program is invalid.");
		return false;
	}
	for (const FValueCodecProgram& Child : Program.Children)
	{
		if (Child.WireOffset < 0 || Child.WireSize <= 0
			|| Child.WireOffset > Wire.Num()
			|| Child.WireSize > Wire.Num() - Child.WireOffset
			|| !DecodeWireValue(
				Child,
				Wire.Slice(Child.WireOffset, Child.WireSize),
				Context,
				StructValue,
				OutDetails))
		{
			return false;
		}
	}
	return true;
}

bool DecodeWireValue(
	const FValueCodecProgram& Program,
	const TConstArrayView<uint8> Wire,
	const FAvidScriptBindingInvocationContext& Context,
	void* Container,
	FString& OutDetails)
{
	if (Program.Property == nullptr || Wire.Num() != Program.WireSize)
	{
		OutDetails = TEXT("The cached wire decode leaf is invalid.");
		return false;
	}
	if (Program.Kind == EValueCodecKind::StructWire)
	{
		return DecodeStructChildren(
			Program,
			Wire,
			Context,
			Program.Property->ContainerPtrToValuePtr<void>(Container),
			OutDetails);
	}
	if (Program.Kind <= EValueCodecKind::Enum)
	{
		uint64 Cell = 0;
		if (Wire.Num() > static_cast<int32>(sizeof(Cell)))
		{
			OutDetails = TEXT("The numeric wire leaf is wider than one value cell.");
			return false;
		}
		FMemory::Memcpy(&Cell, Wire.GetData(), Wire.Num());
		return SetNumericValue(Program, Container, Cell, OutDetails);
	}
	if (Program.Kind == EValueCodecKind::Object)
	{
		if (Wire.Num() != 8)
		{
			OutDetails = TEXT("The object wire leaf must be eight bytes.");
			return false;
		}
		uint32 Slot = 0;
		uint32 Generation = 0;
		FMemory::Memcpy(&Slot, Wire.GetData(), sizeof(Slot));
		FMemory::Memcpy(&Generation, Wire.GetData() + sizeof(Slot), sizeof(Generation));
		UObject* Object = nullptr;
		if (!ResolveObjectHandle(
			Slot,
			Generation,
			Program.ObjectClass,
			Context,
			true,
			Object,
			OutDetails))
		{
			return false;
		}
		CastFieldChecked<FObjectPropertyBase>(Program.Property)
			->SetObjectPropertyValue_InContainer(Container, Object);
		return true;
	}
	const int32 ComponentCount = Program.Kind == EValueCodecKind::Transform ? 9 : 3;
	if ((Program.Kind != EValueCodecKind::Vector
			&& Program.Kind != EValueCodecKind::Rotator
			&& Program.Kind != EValueCodecKind::Transform)
		|| Wire.Num() != ComponentCount * static_cast<int32>(sizeof(float)))
	{
		OutDetails = TEXT("The cached struct wire leaf is unsupported.");
		return false;
	}
	float Components[9] = {};
	FMemory::Memcpy(Components, Wire.GetData(), Wire.Num());
	for (int32 Index = 0; Index < ComponentCount; ++Index)
	{
		if (!FMath::IsFinite(Components[Index]))
		{
			OutDetails = TEXT("The struct wire leaf contains a non-finite component.");
			return false;
		}
	}
	void* Value = Program.Property->ContainerPtrToValuePtr<void>(Container);
	if (Program.Kind == EValueCodecKind::Vector)
	{
		*static_cast<FVector*>(Value) = FVector(Components[0], Components[1], Components[2]);
	}
	else if (Program.Kind == EValueCodecKind::Rotator)
	{
		*static_cast<FRotator*>(Value) = FRotator(Components[0], Components[1], Components[2]);
	}
	else
	{
		*static_cast<FTransform*>(Value) = FTransform(
			FRotator(Components[3], Components[4], Components[5]),
			FVector(Components[0], Components[1], Components[2]),
			FVector(Components[6], Components[7], Components[8]));
	}
	return true;
}

bool ReadWireBlob(
	const uint32 GuestAddress,
	const FValueCodecProgram& Program,
	IAvidScriptVmGuestMemory& GuestMemory,
	TConstArrayView<uint8>& OutWire,
	TArray<uint8, TInlineAllocator<4096>>& Fallback,
	FString& OutDetails)
{
	if (Program.WireSize <= 0 || Program.WireSize > 4096)
	{
		OutDetails = TEXT("The cached wire blob size is invalid.");
		return false;
	}
	FString BorrowError;
	if (GuestMemory.BorrowReadOnlyBytes(
		GuestAddress,
		static_cast<uint32>(Program.WireSize),
		static_cast<uint32>(FMath::Max(1, Program.WireAlignment)),
		OutWire,
		BorrowError))
	{
		return OutWire.Num() == Program.WireSize;
	}
	Fallback.SetNumUninitialized(Program.WireSize);
	if (!GuestMemory.ReadBytes(GuestAddress, MakeArrayView(Fallback), OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = BorrowError;
		}
		return false;
	}
	OutWire = MakeArrayView(Fallback);
	return true;
}

bool EncodeWireValue(
	const FValueCodecProgram& Program,
	TArrayView<uint8> Wire,
	const FAvidScriptBindingInvocationContext& Context,
	void* Container,
	FCodecOutputTransaction& Transaction,
	FString& OutDetails)
{
	if (Program.Property == nullptr || Wire.Num() != Program.WireSize)
	{
		OutDetails = TEXT("The cached wire encode leaf is invalid.");
		return false;
	}
	const void* Value = Program.Property->ContainerPtrToValuePtr<void>(Container);
	if (Program.Kind == EValueCodecKind::StructWire)
	{
		for (const FValueCodecProgram& Child : Program.Children)
		{
			if (Child.WireOffset < 0 || Child.WireSize <= 0
				|| Child.WireOffset > Wire.Num()
				|| Child.WireSize > Wire.Num() - Child.WireOffset
				|| !EncodeWireValue(
					Child,
					Wire.Slice(Child.WireOffset, Child.WireSize),
					Context,
					const_cast<void*>(Value),
					Transaction,
					OutDetails))
			{
				return false;
			}
		}
		return true;
	}
	if (Program.Kind == EValueCodecKind::Bool)
	{
		const int32 Stored = CastFieldChecked<FBoolProperty>(Program.Property)->GetPropertyValue(Value) ? 1 : 0;
		FMemory::Memcpy(Wire.GetData(), &Stored, sizeof(Stored));
		return true;
	}
	if (Program.Kind == EValueCodecKind::Float)
	{
		const float Stored = CastFieldChecked<FFloatProperty>(Program.Property)->GetPropertyValue(Value);
		FMemory::Memcpy(Wire.GetData(), &Stored, sizeof(Stored));
		return true;
	}
	if (Program.Kind == EValueCodecKind::Double)
	{
		const double Stored = CastFieldChecked<FDoubleProperty>(Program.Property)->GetPropertyValue(Value);
		FMemory::Memcpy(Wire.GetData(), &Stored, sizeof(Stored));
		return true;
	}
	if (Program.Kind == EValueCodecKind::Name
		|| Program.Kind == EValueCodecKind::String)
	{
		if (Wire.Num() != static_cast<int32>(sizeof(uint32)))
		{
			OutDetails = TEXT("The cached UTF-8 output wire must contain one i32 value reference.");
			return false;
		}
		FString Stored;
		if (Program.Kind == EValueCodecKind::Name)
		{
			CastFieldChecked<FNameProperty>(Program.Property)
				->GetPropertyValue(Value).ToString(Stored);
		}
		else
		{
			Stored = CastFieldChecked<FStrProperty>(Program.Property)
				->GetPropertyValue(Value);
		}
		const FTCHARToUTF8 Utf8(*Stored, Stored.Len());
		if (Utf8.Length() < 0
			|| static_cast<uint32>(Utf8.Length())
				> FAvidScriptUtf8ValueHeap::MaxValueBytes)
		{
			OutDetails = TEXT("The UTF-8 output exceeds the 1 MiB session value limit.");
			return false;
		}
		const TConstArrayView<uint8> Bytes(
			reinterpret_cast<const uint8*>(Utf8.Get()),
			Utf8.Length());
		uint32 Token = 0;
		if (!Transaction.InternNextUtf8Value(
				Bytes,
				Context,
				Token,
				OutDetails))
		{
			return false;
		}
		FMemory::Memcpy(Wire.GetData(), &Token, sizeof(Token));
		return true;
	}
	if (Program.Kind == EValueCodecKind::Object)
	{
		UObject* Object = CastFieldChecked<FObjectPropertyBase>(Program.Property)
			->GetObjectPropertyValue_InContainer(Container);
		FAvidScriptObjectHandle Handle;
		if (Object != nullptr)
		{
			if (Program.ObjectClass == nullptr || !Object->IsA(Program.ObjectClass)
				|| Context.ObjectRegistry == nullptr || Context.ObjectOwnership == nullptr)
			{
				OutDetails = TEXT("The UObject output does not satisfy the cached property class or capability services.");
				return false;
			}
			FAvidScriptObjectHandleResult BorrowResult;
			if (!Context.ObjectOwnership->Borrow(*Context.ObjectRegistry, *Object, BorrowResult))
			{
				OutDetails = BorrowResult.ErrorMessage;
				return false;
			}
			Handle = BorrowResult.Handle;
			Transaction.BorrowedHandles.Add(Handle);
		}
		FMemory::Memcpy(Wire.GetData(), &Handle.Slot, sizeof(Handle.Slot));
		FMemory::Memcpy(Wire.GetData() + sizeof(Handle.Slot), &Handle.Generation, sizeof(Handle.Generation));
		return true;
	}
	if (Program.Kind == EValueCodecKind::Vector
		|| Program.Kind == EValueCodecKind::Rotator
		|| Program.Kind == EValueCodecKind::Transform)
	{
		float Components[9] = {};
		if (Program.Kind == EValueCodecKind::Vector)
		{
			const FVector& Stored = *static_cast<const FVector*>(Value);
			Components[0] = static_cast<float>(Stored.X); Components[1] = static_cast<float>(Stored.Y); Components[2] = static_cast<float>(Stored.Z);
		}
		else if (Program.Kind == EValueCodecKind::Rotator)
		{
			const FRotator& Stored = *static_cast<const FRotator*>(Value);
			Components[0] = static_cast<float>(Stored.Pitch); Components[1] = static_cast<float>(Stored.Yaw); Components[2] = static_cast<float>(Stored.Roll);
		}
		else
		{
			const FTransform& Stored = *static_cast<const FTransform*>(Value);
			const FVector Translation = Stored.GetTranslation(); const FRotator Rotation = Stored.Rotator(); const FVector Scale = Stored.GetScale3D();
			Components[0] = static_cast<float>(Translation.X); Components[1] = static_cast<float>(Translation.Y); Components[2] = static_cast<float>(Translation.Z);
			Components[3] = static_cast<float>(Rotation.Pitch); Components[4] = static_cast<float>(Rotation.Yaw); Components[5] = static_cast<float>(Rotation.Roll);
			Components[6] = static_cast<float>(Scale.X); Components[7] = static_cast<float>(Scale.Y); Components[8] = static_cast<float>(Scale.Z);
		}
		FMemory::Memcpy(Wire.GetData(), Components, Wire.Num());
		return true;
	}
	const FNumericProperty* Numeric = CastField<FNumericProperty>(Program.Property);
	if (Program.Kind == EValueCodecKind::Enum)
	{
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(Program.Property))
		{
			Numeric = Enum->GetUnderlyingProperty();
		}
	}
	if (Numeric == nullptr || Wire.Num() > static_cast<int32>(sizeof(uint64)))
	{
		OutDetails = TEXT("The cached numeric wire output leaf is invalid.");
		return false;
	}
	const uint64 Stored = Numeric->GetUnsignedIntPropertyValue(Value);
	FMemory::Memcpy(Wire.GetData(), &Stored, Wire.Num());
	return true;
}
} // namespace

void FCodecOutputTransaction::Commit()
{
	if (Utf8ValueHeap != nullptr)
	{
		for (FAvidScriptUtf8ValueReservation& Reservation : Utf8Reservations)
		{
			Utf8ValueHeap->ReleaseReservation(Reservation);
		}
	}
	BorrowedHandles.Reset();
	Utf8Reservations.Reset();
	CreatedUtf8Tokens.Reset();
	Utf8ValueHeap = nullptr;
	NextUtf8Reservation = 0;
}

bool FCodecOutputTransaction::ReserveUtf8Value(
	const FAvidScriptBindingInvocationContext& Context,
	FString& OutDetails)
{
	if (Context.Utf8ValueHeap == nullptr)
	{
		OutDetails = TEXT("The UTF-8 output requires a session value heap.");
		return false;
	}
	if (Utf8ValueHeap != nullptr && Utf8ValueHeap != Context.Utf8ValueHeap)
	{
		OutDetails = TEXT("The UTF-8 output transaction cannot span runtime sessions.");
		return false;
	}
	Utf8ValueHeap = Context.Utf8ValueHeap;
	FAvidScriptUtf8ValueReservation& Reservation =
		Utf8Reservations.AddDefaulted_GetRef();
	if (!Utf8ValueHeap->Reserve(Reservation, OutDetails))
	{
		Utf8Reservations.Pop(EAllowShrinking::No);
		return false;
	}
	return true;
}

bool FCodecOutputTransaction::InternNextUtf8Value(
	const TConstArrayView<uint8> Bytes,
	const FAvidScriptBindingInvocationContext& Context,
	uint32& OutToken,
	FString& OutDetails)
{
	OutToken = 0;
	if (Utf8ValueHeap == nullptr
		|| Utf8ValueHeap != Context.Utf8ValueHeap
		|| !Utf8Reservations.IsValidIndex(NextUtf8Reservation))
	{
		OutDetails = TEXT("The UTF-8 output has no matching preflight reservation.");
		return false;
	}
	FAvidScriptUtf8ValueReservation& Reservation =
		Utf8Reservations[NextUtf8Reservation++];
	bool bCreated = false;
	if (!Utf8ValueHeap->InternReserved(
			Reservation,
			Bytes,
			OutToken,
			bCreated,
			OutDetails))
	{
		return false;
	}
	if (bCreated)
	{
		CreatedUtf8Tokens.Add(OutToken);
	}
	return true;
}

void FCodecOutputTransaction::Rollback(
	const FAvidScriptBindingInvocationContext& Context)
{
	if (Context.ObjectRegistry != nullptr && Context.ObjectOwnership != nullptr)
	{
		for (int32 Index = BorrowedHandles.Num() - 1; Index >= 0; --Index)
		{
			FAvidScriptObjectHandleResult Result;
			Context.ObjectOwnership->Release(BorrowedHandles[Index], *Context.ObjectRegistry, Result);
		}
	}
	if (Utf8ValueHeap != nullptr)
	{
		for (int32 Index = CreatedUtf8Tokens.Num() - 1; Index >= 0; --Index)
		{
			Utf8ValueHeap->RemoveCreatedValue(CreatedUtf8Tokens[Index]);
		}
		for (FAvidScriptUtf8ValueReservation& Reservation : Utf8Reservations)
		{
			Utf8ValueHeap->ReleaseReservation(Reservation);
		}
	}
	BorrowedHandles.Reset();
	Utf8Reservations.Reset();
	CreatedUtf8Tokens.Reset();
	Utf8ValueHeap = nullptr;
	NextUtf8Reservation = 0;
}

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
	if (Program.Kind == EValueCodecKind::StructWire)
	{
		uint32 GuestAddress = 0;
		if (GuestMemory == nullptr
			|| !ResolveGuestAddress(
				Cells[0],
				static_cast<uint32>(Program.WireSize),
				GuestAddress,
				OutDetails))
		{
			if (OutDetails.IsEmpty())
			{
				OutDetails = TEXT("The struct wire input requires guest memory.");
			}
			return false;
		}
		return SetValueFromGuest(
			Program,
			GuestAddress,
			*GuestMemory,
			Context,
			Frame,
			OutDetails);
	}
	if (Program.Kind == EValueCodecKind::Name
		|| Program.Kind == EValueCodecKind::String)
	{
		if (GuestMemory == nullptr)
		{
			OutDetails = TEXT("The UTF-8 input requires guest memory.");
			return false;
		}
		if (Cells[0] > MAX_uint32)
		{
			OutDetails =
				TEXT("The UTF-8 value reference does not fit the 32-bit guest address space.");
			return false;
		}
		return SetUtf8Value(
			Program,
			static_cast<uint32>(Cells[0]),
			*GuestMemory,
			Context,
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
	TConstArrayView<uint8> Wire;
	TArray<uint8, TInlineAllocator<4096>> Fallback;
	if (!ReadWireBlob(GuestAddress, Program, GuestMemory, Wire, Fallback, OutDetails))
	{
		return false;
	}
	if (Program.Kind == EValueCodecKind::Name
		|| Program.Kind == EValueCodecKind::String)
	{
		if (Wire.Num() != static_cast<int32>(sizeof(uint32)))
		{
			OutDetails = TEXT("The cached UTF-8 guest storage must contain one i32 value reference.");
			return false;
		}
		uint32 ValueReference = 0;
		FMemory::Memcpy(&ValueReference, Wire.GetData(), sizeof(ValueReference));
		return SetUtf8Value(
			Program,
			ValueReference,
			GuestMemory,
			Context,
			Frame,
			OutDetails);
	}
	return DecodeWireValue(Program, Wire, Context, Frame, OutDetails);
}

bool SetStructValueFromGuest(
	const FValueCodecProgram& Program,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* StructValue,
	FString& OutDetails)
{
	TConstArrayView<uint8> Wire;
	TArray<uint8, TInlineAllocator<4096>> Fallback;
	return ReadWireBlob(GuestAddress, Program, GuestMemory, Wire, Fallback, OutDetails)
		&& DecodeStructChildren(Program, Wire, Context, StructValue, OutDetails);
}

bool ResolveGuestAddress(
	const uint64 Cell,
	const uint32 ByteCount,
	uint32& OutGuestAddress,
	FString& OutDetails)
{
	OutGuestAddress = 0;
	if (Cell > MAX_uint32
		|| ByteCount == 0
		|| Cell + static_cast<uint64>(ByteCount) > static_cast<uint64>(MAX_uint32) + 1)
	{
		OutDetails = TEXT("The guest address range does not fit the 32-bit guest address space.");
		return false;
	}
	OutGuestAddress = static_cast<uint32>(Cell);
	return true;
}

bool PreflightValueOutput(
	const FValueCodecProgram& Program,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	FCodecOutputTransaction& Transaction,
	FPreparedValueOutput& OutPreparedOutput,
	FString& OutDetails)
{
	OutPreparedOutput = FPreparedValueOutput();
	if (Program.WireSize <= 0 || Program.WireSize > 4096)
	{
		OutDetails = TEXT("The cached output wire size is invalid.");
		return false;
	}
	if (!GuestMemory.BorrowMutableBytes(
			GuestAddress,
			static_cast<uint32>(Program.WireSize),
			static_cast<uint32>(FMath::Max(1, Program.WireAlignment)),
			OutPreparedOutput.GuestBytes,
			OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The guest output provider cannot secure a mutable range for atomic publication.");
		}
		return false;
	}
	if (OutPreparedOutput.GuestBytes.Num() != Program.WireSize)
	{
		OutDetails = TEXT("The borrowed guest output range has the wrong size.");
		return false;
	}
	OutPreparedOutput.EncodedBytes.SetNumZeroed(Program.WireSize);
	return (Program.Kind != EValueCodecKind::Name
			&& Program.Kind != EValueCodecKind::String)
		|| Transaction.ReserveUtf8Value(Context, OutDetails);
}

bool WriteValueToGuest(
	const FValueCodecProgram& Program,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FCodecOutputTransaction& Transaction,
	FPreparedValueOutput& PreparedOutput,
	FString& OutDetails)
{
	if (Program.WireSize <= 0
		|| PreparedOutput.GuestBytes.Num() != Program.WireSize
		|| PreparedOutput.EncodedBytes.Num() != Program.WireSize)
	{
		OutDetails = TEXT("The prepared guest output does not match the cached wire size.");
		return false;
	}
	return EncodeWireValue(
		Program,
		MakeArrayView(PreparedOutput.EncodedBytes),
		Context,
		Frame,
		Transaction,
		OutDetails);
}

void PublishValueOutput(FPreparedValueOutput& PreparedOutput)
{
	check(PreparedOutput.GuestBytes.Num() == PreparedOutput.EncodedBytes.Num());
	FMemory::Memcpy(
		PreparedOutput.GuestBytes.GetData(),
		PreparedOutput.EncodedBytes.GetData(),
		PreparedOutput.EncodedBytes.Num());
}
} // namespace UE::AvidScript::BindingPrivate
