#include "Continuation/AvidScriptContinuationResultCodec.h"

#include "AvidScriptArrayValueHeap.h"
#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptBindingLatent.h"
#include "AvidScriptObjectOwnership.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptUtf8ValueHeap.h"
#include "UObject/Class.h"

namespace
{
bool FailCodec(FString& OutError, const TCHAR* Error)
{
	OutError = Error;
	return false;
}

bool EncodeAbiCells(
	const FAvidScriptBindingLatentCompletionPayload& Payload,
	const FAvidScriptBindingTypeModel& Type,
	TArrayView<uint8> OutBytes,
	FString& OutError)
{
	if (Payload.AbiCells.Num() != Type.AbiTypes.Num()
		|| Type.AbiTypes.IsEmpty())
	{
		return FailCodec(
			OutError,
			TEXT("continuation_result_abi_shape_mismatch"));
	}

	int32 Offset = 0;
	for (int32 Index = 0; Index < Type.AbiTypes.Num(); ++Index)
	{
		const FString& AbiType = Type.AbiTypes[Index];
		int32 Width = AbiType == TEXT("I") || AbiType == TEXT("F") ? 8 : 4;
		if (Type.AbiTypes.Num() == 1 && Type.Size < Width)
		{
			Width = Type.Size;
		}
		if ((AbiType != TEXT("i")
				&& AbiType != TEXT("I")
				&& AbiType != TEXT("f")
				&& AbiType != TEXT("F"))
			|| Width <= 0
			|| Offset + Width > OutBytes.Num())
		{
			return FailCodec(
				OutError,
				TEXT("continuation_result_abi_type_invalid"));
		}
		FMemory::Memcpy(
			OutBytes.GetData() + Offset,
			&Payload.AbiCells[Index],
			Width);
		Offset += Width;
	}
	return Offset == OutBytes.Num()
		? true
		: FailCodec(OutError, TEXT("continuation_result_abi_size_mismatch"));
}

bool EncodeObjectValue(
	UObject* Object,
	const FAvidScriptBindingTypeModel& Type,
	const FAvidScriptBindingPackage& Package,
	FAvidScriptObjectRegistry* ObjectRegistry,
	IAvidScriptObjectOwnershipDomain* ObjectOwnership,
	TArrayView<uint8> OutBytes,
	FString& OutError)
{
	UClass* ExpectedClass = nullptr;
	if (Type.Kind != TEXT("object_handle")
		|| Type.Size != 8
		|| OutBytes.Num() != Type.Size
		|| ObjectRegistry == nullptr
		|| ObjectOwnership == nullptr
		|| Type.ObjectTypeOrdinal < 0
		|| !Package.TryResolveObjectType(
			static_cast<uint32>(Type.ObjectTypeOrdinal),
			ExpectedClass)
		|| ExpectedClass == nullptr
		|| (Object != nullptr
			&& (!IsValid(Object) || !Object->IsA(ExpectedClass))))
	{
		return FailCodec(
			OutError,
			TEXT("continuation_result_object_type_mismatch"));
	}

	FAvidScriptObjectHandle Handle;
	if (Object != nullptr)
	{
		FAvidScriptObjectHandleResult BorrowResult;
		if (!ObjectOwnership->Borrow(
				*ObjectRegistry,
				*Object,
				BorrowResult)
			|| !BorrowResult.Handle.IsValid())
		{
			return FailCodec(
				OutError,
				TEXT("continuation_result_object_borrow_failed"));
		}
		Handle = BorrowResult.Handle;
	}
	FMemory::Memcpy(OutBytes.GetData(), &Handle.Slot, sizeof(Handle.Slot));
	FMemory::Memcpy(
		OutBytes.GetData() + sizeof(Handle.Slot),
		&Handle.Generation,
		sizeof(Handle.Generation));
	return true;
}

bool IsFixedWirePayloadType(const FAvidScriptBindingTypeModel& Type)
{
	return Type.Kind == TEXT("scalar")
		|| Type.Kind == TEXT("enum")
		|| Type.Kind == TEXT("struct")
		|| Type.Kind == TEXT("struct_wire");
}
}

void FAvidScriptContinuationResultCodecTransaction::Commit()
{
	CreatedUtf8Tokens.Reset();
	CreatedArrayTokens.Reset();
}

void FAvidScriptContinuationResultCodecTransaction::Rollback(
	FAvidScriptUtf8ValueHeap& Utf8ValueHeap,
	FAvidScriptArrayValueHeap& ArrayValueHeap)
{
	for (const uint32 Token : CreatedUtf8Tokens)
	{
		Utf8ValueHeap.RemoveCreatedValue(Token);
	}
	for (const uint32 Token : CreatedArrayTokens)
	{
		ArrayValueHeap.RemoveCreatedValue(Token);
	}
	Commit();
}

bool FAvidScriptContinuationResultCodec::Encode(
	const FAvidScriptBindingLatentCompletionPayload& Payload,
	const FAvidScriptBindingTypeModel& Type,
	const FAvidScriptBindingPackage& Package,
	FAvidScriptObjectRegistry* ObjectRegistry,
	IAvidScriptObjectOwnershipDomain* ObjectOwnership,
	FAvidScriptUtf8ValueHeap& Utf8ValueHeap,
	FAvidScriptArrayValueHeap& ArrayValueHeap,
	TArrayView<uint8> OutBytes,
	FAvidScriptContinuationResultCodecTransaction& Transaction,
	FString& OutError)
{
	OutError.Reset();
	if (Payload.TypeId != Type.StableId
		|| Type.Size <= 0
		|| OutBytes.Num() != Type.Size)
	{
		return FailCodec(
			OutError,
			TEXT("continuation_result_descriptor_mismatch"));
	}
	FMemory::Memzero(OutBytes.GetData(), OutBytes.Num());

	switch (Payload.Kind)
	{
	case EAvidScriptBindingLatentPayloadKind::AbiCells:
		return EncodeAbiCells(Payload, Type, OutBytes, OutError);
	case EAvidScriptBindingLatentPayloadKind::FixedWire:
		if (!IsFixedWirePayloadType(Type)
			|| Payload.Bytes.Num() != Type.Size)
		{
			return FailCodec(
				OutError,
				TEXT("continuation_result_wire_shape_mismatch"));
		}
		FMemory::Memcpy(OutBytes.GetData(), Payload.Bytes.GetData(), Type.Size);
		return true;
	case EAvidScriptBindingLatentPayloadKind::Object:
		return EncodeObjectValue(
			Payload.ObjectValue.Get(),
			Type,
			Package,
			ObjectRegistry,
			ObjectOwnership,
			OutBytes,
			OutError);
	case EAvidScriptBindingLatentPayloadKind::Utf8:
	{
		if ((Type.Kind != TEXT("string_utf8")
				&& Type.Kind != TEXT("name_utf8"))
			|| Type.Size != 4)
		{
			return FailCodec(
				OutError,
				TEXT("continuation_result_utf8_type_mismatch"));
		}
		FAvidScriptUtf8ValueReservation Reservation;
		FString HeapError;
		uint32 Token = 0;
		bool bCreated = false;
		if (!Utf8ValueHeap.Reserve(Reservation, HeapError)
			|| !Utf8ValueHeap.InternReserved(
				Reservation,
				Payload.Bytes,
				Token,
				bCreated,
				HeapError))
		{
			Utf8ValueHeap.ReleaseReservation(Reservation);
			OutError = HeapError.IsEmpty()
				? TEXT("continuation_result_utf8_publish_failed")
				: MoveTemp(HeapError);
			return false;
		}
		if (bCreated)
		{
			Transaction.CreatedUtf8Tokens.Add(Token);
		}
		FMemory::Memcpy(OutBytes.GetData(), &Token, sizeof(Token));
		return true;
	}
	case EAvidScriptBindingLatentPayloadKind::Array:
	{
		if (Type.Kind != TEXT("array")
			|| Type.Size != 4
			|| Type.ElementTypeId != Payload.ElementTypeId)
		{
			return FailCodec(
				OutError,
				TEXT("continuation_result_array_type_mismatch"));
		}
		FAvidScriptArrayValueReservation Reservation;
		FString HeapError;
		uint32 Token = 0;
		if (!ArrayValueHeap.Reserve(Reservation, HeapError)
			|| !ArrayValueHeap.PublishReserved(
				Reservation,
				Type.StableId,
				Payload.ElementCount,
				Payload.ElementStride,
				Payload.ElementAlignment,
				Payload.Bytes,
				Token,
				HeapError))
		{
			ArrayValueHeap.ReleaseReservation(Reservation);
			OutError = HeapError.IsEmpty()
				? TEXT("continuation_result_array_publish_failed")
				: MoveTemp(HeapError);
			return false;
		}
		Transaction.CreatedArrayTokens.Add(Token);
		FMemory::Memcpy(OutBytes.GetData(), &Token, sizeof(Token));
		return true;
	}
	case EAvidScriptBindingLatentPayloadKind::Composite:
	{
		if (Type.Kind != TEXT("struct_wire")
			|| Type.StructFields.IsEmpty()
			|| Payload.Fields.IsEmpty())
		{
			return FailCodec(
				OutError,
				TEXT("continuation_result_composite_shape_mismatch"));
		}
		TSet<int32> WrittenOffsets;
		bool bHasDiscriminator = false;
		for (const FAvidScriptBindingLatentCompletionField& Field :
			Payload.Fields)
		{
			const FAvidScriptBindingStructFieldModel* const DescriptorField =
				Type.StructFields.FindByPredicate(
					[&Field](
						const FAvidScriptBindingStructFieldModel& Candidate)
					{
						return Candidate.WireOffset == Field.WireOffset
							&& Candidate.TypeId == Field.TypeId;
					});
			const FAvidScriptBindingTypeModel* FieldType = nullptr;
			if (DescriptorField == nullptr
				|| WrittenOffsets.Contains(Field.WireOffset)
				|| !Package.TryGetDescriptorType(Field.TypeId, FieldType)
				|| FieldType == nullptr
				|| Field.WireOffset < 0
				|| Field.WireOffset > OutBytes.Num()
				|| FieldType->Size > OutBytes.Num() - Field.WireOffset)
			{
				return FailCodec(
					OutError,
					TEXT("continuation_result_composite_field_mismatch"));
			}
			TArrayView<uint8> FieldBytes = OutBytes.Slice(
				Field.WireOffset,
				FieldType->Size);
			if (Field.Kind == EAvidScriptBindingLatentPayloadKind::FixedWire)
			{
				if (!IsFixedWirePayloadType(*FieldType)
					|| Field.Bytes.Num() != FieldType->Size)
				{
					return FailCodec(
						OutError,
						TEXT("continuation_result_composite_wire_mismatch"));
				}
				FMemory::Memcpy(
					FieldBytes.GetData(),
					Field.Bytes.GetData(),
					Field.Bytes.Num());
			}
			else if (Field.Kind
				== EAvidScriptBindingLatentPayloadKind::Object)
			{
				if (!EncodeObjectValue(
						Field.ObjectValue.Get(),
						*FieldType,
						Package,
						ObjectRegistry,
						ObjectOwnership,
						FieldBytes,
						OutError))
				{
					return false;
				}
			}
			else
			{
				return FailCodec(
					OutError,
					TEXT("continuation_result_composite_kind_unsupported"));
			}
			WrittenOffsets.Add(Field.WireOffset);
			bHasDiscriminator |= Field.WireOffset == 0
				&& DescriptorField->Name == TEXT("Outcome")
				&& FieldType->Kind == TEXT("enum");
		}
		return bHasDiscriminator
			? true
			: FailCodec(
				OutError,
				TEXT("continuation_result_composite_discriminator_missing"));
	}
	default:
		return FailCodec(
			OutError,
			TEXT("continuation_result_payload_kind_unsupported"));
	}
}
