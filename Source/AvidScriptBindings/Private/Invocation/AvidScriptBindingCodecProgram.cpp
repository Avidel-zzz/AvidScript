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
	BorrowedHandles.Reset();
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
	BorrowedHandles.Reset();
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
	TConstArrayView<uint8> Wire;
	TArray<uint8, TInlineAllocator<4096>> Fallback;
	if (!ReadWireBlob(GuestAddress, Program, GuestMemory, Wire, Fallback, OutDetails))
	{
		return false;
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
	FString& OutDetails)
{
	if (Program.WireSize <= 0 || Program.WireSize > 4096)
	{
		OutDetails = TEXT("The cached output wire size is invalid.");
		return false;
	}
	TArrayView<uint8> Borrowed;
	FString BorrowError;
	if (GuestMemory.BorrowMutableBytes(
		GuestAddress,
		static_cast<uint32>(Program.WireSize),
		static_cast<uint32>(FMath::Max(1, Program.WireAlignment)),
		Borrowed,
		BorrowError))
	{
		return Borrowed.Num() == Program.WireSize;
	}
	TArray<uint8, TInlineAllocator<4096>> Probe;
	Probe.SetNumUninitialized(Program.WireSize);
	if (!GuestMemory.ReadBytes(GuestAddress, MakeArrayView(Probe), OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = BorrowError;
		}
		return false;
	}
	return true;
}

bool WriteValueToGuest(
	const FValueCodecProgram& Program,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails,
	FCodecOutputTransaction* Transaction)
{
	FCodecOutputTransaction LocalTransaction;
	FCodecOutputTransaction& ActiveTransaction = Transaction == nullptr
		? LocalTransaction
		: *Transaction;
	TArray<uint8, TInlineAllocator<4096>> Bytes;
	if (Program.WireSize <= 0 || Program.WireSize > 4096)
	{
		OutDetails = TEXT("The cached output wire size is invalid.");
		return false;
	}
	Bytes.SetNumZeroed(Program.WireSize);
	if (!EncodeWireValue(
		Program,
		MakeArrayView(Bytes),
		Context,
		Frame,
		ActiveTransaction,
		OutDetails))
	{
		if (Transaction == nullptr)
		{
			LocalTransaction.Rollback(Context);
		}
		return false;
	}

	TArrayView<uint8> Borrowed;
	FString BorrowError;
	const bool bBorrowed = GuestMemory.BorrowMutableBytes(
		GuestAddress,
		static_cast<uint32>(Bytes.Num()),
		static_cast<uint32>(FMath::Max(1, Program.WireAlignment)),
		Borrowed,
		BorrowError);
	const bool bWritten = bBorrowed && Borrowed.Num() == Bytes.Num()
		? (FMemory::Memcpy(Borrowed.GetData(), Bytes.GetData(), Bytes.Num()), true)
		: GuestMemory.WriteBytes(GuestAddress, MakeArrayView(Bytes), OutDetails);
	if (!bWritten)
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = BorrowError;
		}
		if (Transaction == nullptr)
		{
			LocalTransaction.Rollback(Context);
		}
		return false;
	}
	if (Transaction == nullptr)
	{
		LocalTransaction.Commit();
	}
	return true;
#if 0
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
#endif
}
} // namespace UE::AvidScript::BindingPrivate
