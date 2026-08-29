#include "AvidScriptVmStaticHostImports.h"

namespace
{
constexpr int32 StaticImportMaxTransformBatchCount = 256;
constexpr int32 StaticImportMaxArrayElementCount = 4096;
constexpr int32 StaticImportMaxContinuationResultBytes = 4096;
constexpr int32 StaticImportMaxContinuationStateBytes = 4096;
constexpr uint32 StaticImportTransformBatchInputCellsPerItem = 2;
constexpr uint32 StaticImportTransformBatchOutputFloatsPerItem = 9;

const FAvidScriptVmStaticHostImport GStaticHostImports[] = {
	{ EAvidScriptHostBindingId::HostAddI32, "host_add_i32", "(i)i", true },
	{ EAvidScriptHostBindingId::HostFailI32, "host_fail_i32", "(i)i", true },
	{ EAvidScriptHostBindingId::ActorGetLocation, "actor_get_location", "(iii)i", true },
	{ EAvidScriptHostBindingId::ActorSetLocation, "actor_set_location", "(iifff)i", true },
	{ EAvidScriptHostBindingId::ActorAddLocationOffset, "actor_add_location_offset", "(iifff)i", true },
	{ EAvidScriptHostBindingId::ActorGetRotation, "actor_get_rotation", "(iii)i", true },
	{ EAvidScriptHostBindingId::ActorSetRotation, "actor_set_rotation", "(iifff)i", true },
	{ EAvidScriptHostBindingId::ActorGetScale, "actor_get_scale", "(iii)i", true },
	{ EAvidScriptHostBindingId::ActorSetScale, "actor_set_scale", "(iifff)i", true },
	{ EAvidScriptHostBindingId::ActorGetTransformBatch, "actor_get_transform_batch", "(iii)i", true },
	{ EAvidScriptHostBindingId::ActorGetRootComponent, "actor_get_root_component", "(iii)i", true },
	{ EAvidScriptHostBindingId::SceneComponentGetWorldLocation, "scene_component_get_world_location", "(iii)i", true },
	{ EAvidScriptHostBindingId::SceneComponentSetWorldLocation, "scene_component_set_world_location", "(iifff)i", true },
	{ EAvidScriptHostBindingId::OwnerGetSlot, "owner_get_slot", "()i", true },
	{ EAvidScriptHostBindingId::OwnerGetGeneration, "owner_get_generation", "()i", true },
	{ EAvidScriptHostBindingId::OwnerGetHandle, "avid_owner_get_handle", "()I", false },
	{ EAvidScriptHostBindingId::TimerSetOnce, "timer_set_once", "(fi)i", true },
	{ EAvidScriptHostBindingId::TimerCancel, "timer_cancel", "(i)i", true },
	{ EAvidScriptHostBindingId::EventSubscribe, "event_subscribe", "(iii)I", true },
	{ EAvidScriptHostBindingId::EventUnsubscribe, "event_unsubscribe", "(I)i", true },
	{ EAvidScriptHostBindingId::DataLaneGetEpoch, "avid_data_lane_epoch", "()I", false },
	{ EAvidScriptHostBindingId::DataLaneSubmit, "avid_data_lane_submit", "(ii)i", false },
	{ EAvidScriptHostBindingId::ValueArrayLength, "avid_value_array_length", "(i)i", false },
	{ EAvidScriptHostBindingId::ValueArrayLoad, "avid_value_array_load", "(iiii)i", false },
	{ EAvidScriptHostBindingId::ValueArrayStore, "avid_value_array_store", "(iiii)i", false },
	{ EAvidScriptHostBindingId::ValueArrayReadRange, "avid_value_array_read_range", "(iiiii)i", false },
	{ EAvidScriptHostBindingId::ValueArrayWriteRange, "avid_value_array_write_range", "(iiiii)i", false },
	{ EAvidScriptHostBindingId::ValueRelease, "avid_value_release", "(i)i", false },
	{ EAvidScriptHostBindingId::ContinuationDelay, "continuation_delay", "(fi)I", true },
	{ EAvidScriptHostBindingId::ContinuationCancel, "continuation_cancel", "(I)i", true },
	{ EAvidScriptHostBindingId::ContinuationLoadObject, "continuation_load_object", "(ii)I", true },
	{ EAvidScriptHostBindingId::ContinuationCancelSourceCreate, "continuation_cancel_source_create", "()I", true },
	{ EAvidScriptHostBindingId::ContinuationCancelSourceCancel, "continuation_cancel_source_cancel", "(I)i", true },
	{ EAvidScriptHostBindingId::ContinuationCancelSourceRelease, "continuation_cancel_source_release", "(I)i", true },
	{ EAvidScriptHostBindingId::ContinuationBindCancel, "continuation_bind_cancel", "(II)i", true },
	{ EAvidScriptHostBindingId::ContinuationResultRead, "continuation_result_read", "(iiiii)i", true },
	{ EAvidScriptHostBindingId::ContinuationStateStore, "continuation_state_store", "(Iii)i", true },
	{ EAvidScriptHostBindingId::ContinuationStateRead, "continuation_state_read", "(Iii)i", true }
};

static_assert(
	UE_ARRAY_COUNT(GStaticHostImports) == static_cast<uint16>(EAvidScriptHostBindingId::ContinuationStateRead),
	"Static host catalog must remain dense and ordered by binding id.");

bool FailStaticCall(FString& OutFailureDetails, const TCHAR* Details)
{
	OutFailureDetails = Details;
	return false;
}

bool ValidateStaticArguments(
	const FAvidScriptVmAbiSignature& Signature,
	TConstArrayView<FAvidScriptVmStaticValue> Arguments,
	FString& OutFailureDetails)
{
	if (Arguments.Num() != Signature.Parameters.Num())
	{
		return FailStaticCall(OutFailureDetails, TEXT("Static host import argument count does not match its catalog signature."));
	}
	for (int32 Index = 0; Index < Arguments.Num(); ++Index)
	{
		if (Arguments[Index].Kind != Signature.Parameters[Index])
		{
			OutFailureDetails = FString::Printf(
				TEXT("Static host import argument %d does not match its catalog type."),
				Index);
			return false;
		}
	}
	if (!Signature.bHasResult)
	{
		return FailStaticCall(OutFailureDetails, TEXT("Static AvidScript host imports require one result."));
	}
	return true;
}

bool DispatchStaticCall(
	const FAvidScriptVmStaticHostImport& Import,
	IAvidScriptHostDispatcher* HostDispatcher,
	FAvidScriptHostCall& Call,
	FAvidScriptHostCallResult& OutHostResult,
	FString& OutFailureDetails)
{
	if (HostDispatcher == nullptr)
	{
		return FailStaticCall(OutFailureDetails, TEXT("No host dispatcher is attached to the VM instance."));
	}
	if (!HostDispatcher->DispatchHostCall(Call, OutHostResult) || !OutHostResult.bSucceeded)
	{
		OutFailureDetails = OutHostResult.Details.IsEmpty()
			? FString::Printf(TEXT("Host dispatcher rejected avidscript.%s."), UTF8_TO_TCHAR(Import.ImportName))
			: OutHostResult.Details;
		return false;
	}
	return true;
}

bool ValidateGuestRange(
	int32 GuestAddress,
	uint64 ElementCount,
	uint64 ElementSize,
	uint32 Alignment,
	const TCHAR* RangeName,
	FString& OutFailureDetails)
{
	const uint64 ByteCount = ElementCount * ElementSize;
	if (ElementCount == 0)
	{
		return true;
	}
	if (GuestAddress <= 0
		|| Alignment == 0
		|| static_cast<uint32>(GuestAddress) % Alignment != 0
		|| ByteCount > static_cast<uint64>(MAX_int32)
		|| static_cast<uint64>(static_cast<uint32>(GuestAddress)) + ByteCount > static_cast<uint64>(MAX_uint32) + 1)
	{
		OutFailureDetails = FString::Printf(TEXT("guest_memory_invalid: invalid %s range."), RangeName);
		return false;
	}
	return true;
}

bool WriteGuestBytes(
	IAvidScriptVmGuestMemory& GuestMemory,
	int32 GuestAddress,
	const void* Source,
	int32 ByteCount,
	FString& OutFailureDetails)
{
	if (!ValidateGuestRange(GuestAddress, ByteCount, 1, 1, TEXT("output"), OutFailureDetails))
	{
		return false;
	}
	FString MemoryError;
	if (!GuestMemory.WriteBytes(
		static_cast<uint32>(GuestAddress),
		MakeArrayView(static_cast<const uint8*>(Source), ByteCount),
		MemoryError))
	{
		OutFailureDetails = MemoryError.IsEmpty() ? TEXT("guest_memory_invalid: output write failed.") : MoveTemp(MemoryError);
		return false;
	}
	return true;
}

void SetIntegerResult(
	const FAvidScriptVmAbiSignature& Signature,
	const FAvidScriptHostCallResult& HostResult,
	FAvidScriptVmStaticCallResult& OutResult)
{
	OutResult.Kind = Signature.Result;
	if (Signature.Result == EAvidScriptVmValueKind::I64)
	{
		OutResult.I64 = HostResult.ReturnValueI64;
	}
	else
	{
		OutResult.I32 = HostResult.ReturnValue;
	}
}
} // namespace

TConstArrayView<FAvidScriptVmStaticHostImport> GetAvidScriptVmStaticHostImports()
{
	return MakeArrayView(GStaticHostImports);
}

const FAvidScriptVmStaticHostImport& GetAvidScriptVmStaticHostImport(EAvidScriptHostBindingId BindingId)
{
	check(BindingId != EAvidScriptHostBindingId::Invalid);
	const int32 Index = static_cast<int32>(BindingId) - 1;
	check(GStaticHostImports[Index].BindingId == BindingId);
	return GStaticHostImports[Index];
}

bool InvokeAvidScriptVmStaticHostImport(
	const FAvidScriptVmStaticHostImport& Import,
	const FAvidScriptVmAbiSignature& Signature,
	TConstArrayView<FAvidScriptVmStaticValue> Arguments,
	IAvidScriptHostDispatcher* HostDispatcher,
	IAvidScriptVmGuestMemory& GuestMemory,
	FAvidScriptVmStaticCallResult& OutResult,
	FString& OutFailureDetails)
{
	OutResult = FAvidScriptVmStaticCallResult();
	OutFailureDetails.Reset();
	if (!ValidateStaticArguments(Signature, Arguments, OutFailureDetails))
	{
		return false;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = Import.BindingId;
	FAvidScriptHostCallResult HostResult;
	switch (Import.BindingId)
	{
	case EAvidScriptHostBindingId::HostAddI32:
	case EAvidScriptHostBindingId::HostFailI32:
	case EAvidScriptHostBindingId::TimerCancel:
	case EAvidScriptHostBindingId::ValueArrayLength:
	case EAvidScriptHostBindingId::ValueRelease:
		Call.IntArgs[0] = Arguments[0].I32;
		break;
	case EAvidScriptHostBindingId::ActorGetLocation:
	case EAvidScriptHostBindingId::ActorGetRotation:
	case EAvidScriptHostBindingId::ActorGetScale:
	case EAvidScriptHostBindingId::SceneComponentGetWorldLocation:
	case EAvidScriptHostBindingId::ActorGetRootComponent:
		Call.IntArgs[0] = Arguments[0].I32;
		Call.IntArgs[1] = Arguments[1].I32;
		Call.GuestAddress = static_cast<uint32>(Arguments[2].I32);
		break;
	case EAvidScriptHostBindingId::ActorSetLocation:
	case EAvidScriptHostBindingId::ActorAddLocationOffset:
	case EAvidScriptHostBindingId::ActorSetRotation:
	case EAvidScriptHostBindingId::ActorSetScale:
	case EAvidScriptHostBindingId::SceneComponentSetWorldLocation:
		Call.IntArgs[0] = Arguments[0].I32;
		Call.IntArgs[1] = Arguments[1].I32;
		Call.FloatArgs[0] = Arguments[2].F32;
		Call.FloatArgs[1] = Arguments[3].F32;
		Call.FloatArgs[2] = Arguments[4].F32;
		break;
	case EAvidScriptHostBindingId::TimerSetOnce:
	case EAvidScriptHostBindingId::ContinuationDelay:
		Call.FloatArgs[0] = Arguments[0].F32;
		Call.IntArgs[0] = Arguments[1].I32;
		break;
	case EAvidScriptHostBindingId::EventSubscribe:
		Call.IntArgs[0] = Arguments[0].I32;
		Call.IntArgs[1] = Arguments[1].I32;
		Call.IntArgs[2] = Arguments[2].I32;
		break;
	case EAvidScriptHostBindingId::ContinuationLoadObject:
		Call.IntArgs[0] = Arguments[0].I32;
		Call.IntArgs[1] = Arguments[1].I32;
		break;
	case EAvidScriptHostBindingId::EventUnsubscribe:
	case EAvidScriptHostBindingId::ContinuationCancel:
	case EAvidScriptHostBindingId::ContinuationCancelSourceCancel:
	case EAvidScriptHostBindingId::ContinuationCancelSourceRelease:
		Call.Int64Args[0] = Arguments[0].I64;
		break;
	case EAvidScriptHostBindingId::ContinuationBindCancel:
		Call.Int64Args[0] = Arguments[0].I64;
		Call.Int64Args[1] = Arguments[1].I64;
		break;
	case EAvidScriptHostBindingId::OwnerGetSlot:
	case EAvidScriptHostBindingId::OwnerGetGeneration:
	case EAvidScriptHostBindingId::OwnerGetHandle:
	case EAvidScriptHostBindingId::DataLaneGetEpoch:
	case EAvidScriptHostBindingId::ContinuationCancelSourceCreate:
		break;
	case EAvidScriptHostBindingId::DataLaneSubmit:
	{
		const int32 GuestAddress = Arguments[0].I32;
		const int32 ByteCount = Arguments[1].I32;
		if (ByteCount <= 0)
		{
			return FailStaticCall(
				OutFailureDetails,
				TEXT("guest_memory_invalid: command buffer byte count must be positive."));
		}
		if (!ValidateGuestRange(
				GuestAddress,
				static_cast<uint64>(ByteCount),
				1,
				alignof(uint32),
				TEXT("command buffer"),
				OutFailureDetails))
		{
			return false;
		}
		FString MemoryError;
		if (!GuestMemory.BorrowReadOnlyBytes(
				static_cast<uint32>(GuestAddress),
				static_cast<uint32>(ByteCount),
				alignof(uint32),
				Call.InputBytes,
				MemoryError))
		{
			OutFailureDetails = MemoryError.IsEmpty()
				? TEXT("guest_memory_invalid: command buffer borrow failed.")
				: MoveTemp(MemoryError);
			return false;
		}
		Call.GuestAddress = static_cast<uint32>(GuestAddress);
		Call.IntArgs[0] = ByteCount;
		break;
	}
	case EAvidScriptHostBindingId::ValueArrayLoad:
	case EAvidScriptHostBindingId::ValueArrayStore:
	{
		const int32 Token = Arguments[0].I32;
		const int32 ElementIndex = Arguments[1].I32;
		const int32 GuestAddress = Arguments[2].I32;
		const int32 ByteCount = Arguments[3].I32;
		if (ElementIndex < 0 || ByteCount <= 0 || ByteCount > 4096)
		{
			return FailStaticCall(
				OutFailureDetails,
				TEXT("value_array_invalid: element index or byte count is outside the supported range."));
		}
		if (!ValidateGuestRange(
				GuestAddress,
				static_cast<uint64>(ByteCount),
				1,
				1,
				TEXT("array element"),
				OutFailureDetails))
		{
			return false;
		}

		FString MemoryError;
		if (Import.BindingId == EAvidScriptHostBindingId::ValueArrayLoad)
		{
			if (!GuestMemory.BorrowMutableBytes(
					static_cast<uint32>(GuestAddress),
					static_cast<uint32>(ByteCount),
					1,
					Call.OutputBytes,
					MemoryError))
			{
				OutFailureDetails = MemoryError.IsEmpty()
					? TEXT("guest_memory_invalid: array element output borrow failed.")
					: MoveTemp(MemoryError);
				return false;
			}
		}
		else if (!GuestMemory.BorrowReadOnlyBytes(
				static_cast<uint32>(GuestAddress),
				static_cast<uint32>(ByteCount),
				1,
				Call.InputBytes,
				MemoryError))
		{
			OutFailureDetails = MemoryError.IsEmpty()
				? TEXT("guest_memory_invalid: array element input borrow failed.")
				: MoveTemp(MemoryError);
			return false;
		}
		Call.GuestAddress = static_cast<uint32>(GuestAddress);
		Call.IntArgs[0] = Token;
		Call.IntArgs[1] = ElementIndex;
		Call.IntArgs[2] = ByteCount;
		break;
	}
	case EAvidScriptHostBindingId::ContinuationResultRead:
	{
		const int32 BindingOrdinal = Arguments[0].I32;
		const int32 ResultSlot = Arguments[1].I32;
		const int32 ResultGeneration = Arguments[2].I32;
		const int32 GuestAddress = Arguments[3].I32;
		const int32 ByteCount = Arguments[4].I32;
		if (BindingOrdinal < 0
			|| ResultSlot < 0
			|| ResultGeneration < 0
			|| ByteCount <= 0
			|| ByteCount > StaticImportMaxContinuationResultBytes)
		{
			return FailStaticCall(
				OutFailureDetails,
				TEXT("continuation_result_invalid: result arguments are outside the supported range."));
		}
		if (!ValidateGuestRange(
				GuestAddress,
				static_cast<uint64>(ByteCount),
				1,
				1,
				TEXT("continuation result"),
				OutFailureDetails))
		{
			return false;
		}
		FString MemoryError;
		if (!GuestMemory.BorrowMutableBytes(
				static_cast<uint32>(GuestAddress),
				static_cast<uint32>(ByteCount),
				1,
				Call.OutputBytes,
				MemoryError))
		{
			OutFailureDetails = MemoryError.IsEmpty()
				? TEXT("guest_memory_invalid: continuation result output borrow failed.")
				: MoveTemp(MemoryError);
			return false;
		}
		Call.IntArgs[0] = BindingOrdinal;
		Call.IntArgs[1] = ResultSlot;
		Call.IntArgs[2] = ResultGeneration;
		Call.IntArgs[3] = ByteCount;
		Call.GuestAddress = static_cast<uint32>(GuestAddress);
		break;
	}
	case EAvidScriptHostBindingId::ContinuationStateStore:
	case EAvidScriptHostBindingId::ContinuationStateRead:
	{
		const int64 ContinuationToken = Arguments[0].I64;
		const int32 GuestAddress = Arguments[1].I32;
		const int32 ByteCount = Arguments[2].I32;
		if (ByteCount <= 0
			|| ByteCount > StaticImportMaxContinuationStateBytes)
		{
			return FailStaticCall(
				OutFailureDetails,
				TEXT("continuation_state_invalid: state arguments are outside the supported range."));
		}
		if (!ValidateGuestRange(
				GuestAddress,
				static_cast<uint64>(ByteCount),
				1,
				1,
				TEXT("continuation state"),
				OutFailureDetails))
		{
			return false;
		}

		FString MemoryError;
		if (Import.BindingId == EAvidScriptHostBindingId::ContinuationStateStore)
		{
			if (!GuestMemory.BorrowReadOnlyBytes(
					static_cast<uint32>(GuestAddress),
					static_cast<uint32>(ByteCount),
					1,
					Call.InputBytes,
					MemoryError))
			{
				OutFailureDetails = MemoryError.IsEmpty()
					? TEXT("guest_memory_invalid: continuation state input borrow failed.")
					: MoveTemp(MemoryError);
				return false;
			}
		}
		else if (!GuestMemory.BorrowMutableBytes(
				static_cast<uint32>(GuestAddress),
				static_cast<uint32>(ByteCount),
				1,
				Call.OutputBytes,
				MemoryError))
		{
			OutFailureDetails = MemoryError.IsEmpty()
				? TEXT("guest_memory_invalid: continuation state output borrow failed.")
				: MoveTemp(MemoryError);
			return false;
		}
		Call.Int64Args[0] = ContinuationToken;
		Call.IntArgs[0] = ByteCount;
		Call.GuestAddress = static_cast<uint32>(GuestAddress);
		break;
	}
	case EAvidScriptHostBindingId::ValueArrayReadRange:
	case EAvidScriptHostBindingId::ValueArrayWriteRange:
	{
		const int32 CapabilityIndex = Arguments[1].I32;
		const int32 GuestArrayReference = Arguments[2].I32;
		const int32 GuestIndex = Arguments[3].I32;
		const int32 ElementCount = Arguments[4].I32;
		if (CapabilityIndex < 0
			|| GuestArrayReference <= 0
			|| GuestIndex < 0
			|| ElementCount < 0
			|| ElementCount > StaticImportMaxArrayElementCount)
		{
			return FailStaticCall(
				OutFailureDetails,
				TEXT("value_array_range_invalid: an array range argument is outside the supported range."));
		}
		Call.IntArgs[0] = Arguments[0].I32;
		Call.IntArgs[1] = CapabilityIndex;
		Call.GuestAddress = static_cast<uint32>(GuestArrayReference);
		Call.IntArgs[2] = GuestIndex;
		Call.IntArgs[3] = ElementCount;
		break;
	}
	case EAvidScriptHostBindingId::ActorGetTransformBatch:
	{
		const int32 InputAddress = Arguments[0].I32;
		const int32 Count = Arguments[1].I32;
		const int32 OutputAddress = Arguments[2].I32;
		if (Count < 0 || Count > StaticImportMaxTransformBatchCount)
		{
			return FailStaticCall(OutFailureDetails, TEXT("Batch count is outside the supported range 0..256."));
		}

		const uint32 ItemCount = static_cast<uint32>(Count);
		const uint32 InputCellCount = ItemCount * StaticImportTransformBatchInputCellsPerItem;
		const uint32 OutputFloatCount = ItemCount * StaticImportTransformBatchOutputFloatsPerItem;
		if (!ValidateGuestRange(InputAddress, InputCellCount, sizeof(uint32), alignof(uint32), TEXT("batch input"), OutFailureDetails)
			|| !ValidateGuestRange(OutputAddress, OutputFloatCount, sizeof(float), alignof(float), TEXT("batch output"), OutFailureDetails))
		{
			return false;
		}

		TConstArrayView<uint8> InputBytes;
		TArrayView<uint8> OutputBytes;
		if (InputCellCount > 0)
		{
			FString MemoryError;
			if (!GuestMemory.BorrowReadOnlyBytes(
				static_cast<uint32>(InputAddress),
				InputCellCount * sizeof(uint32),
				alignof(uint32),
				InputBytes,
				MemoryError))
			{
				OutFailureDetails = MemoryError.IsEmpty() ? TEXT("guest_memory_invalid: batch input borrow failed.") : MoveTemp(MemoryError);
				return false;
			}
			if (!GuestMemory.BorrowMutableBytes(
				static_cast<uint32>(OutputAddress),
				OutputFloatCount * sizeof(float),
				alignof(float),
				OutputBytes,
				MemoryError))
			{
				OutFailureDetails = MemoryError.IsEmpty() ? TEXT("guest_memory_invalid: batch output borrow failed.") : MoveTemp(MemoryError);
				return false;
			}
			Call.InputCells = MakeArrayView(
				reinterpret_cast<const uint32*>(InputBytes.GetData()),
				static_cast<int32>(InputCellCount));
			Call.OutputFloats = MakeArrayView(
				reinterpret_cast<float*>(OutputBytes.GetData()),
				static_cast<int32>(OutputFloatCount));
		}
		Call.IntArgs[0] = Count;
		if (!DispatchStaticCall(Import, HostDispatcher, Call, HostResult, OutFailureDetails))
		{
			return false;
		}
		SetIntegerResult(Signature, HostResult, OutResult);
		return true;
	}
	default:
		return FailStaticCall(OutFailureDetails, TEXT("Static host import has no adapter."));
	}

	if (!DispatchStaticCall(Import, HostDispatcher, Call, HostResult, OutFailureDetails))
	{
		return false;
	}

	switch (Import.BindingId)
	{
	case EAvidScriptHostBindingId::ActorGetLocation:
	case EAvidScriptHostBindingId::ActorGetRotation:
	case EAvidScriptHostBindingId::ActorGetScale:
	case EAvidScriptHostBindingId::SceneComponentGetWorldLocation:
		if (!WriteGuestBytes(
			GuestMemory,
			Arguments[2].I32,
			HostResult.FloatValues,
			sizeof(HostResult.FloatValues),
			OutFailureDetails))
		{
			return false;
		}
		break;
	case EAvidScriptHostBindingId::ActorGetRootComponent:
	{
		const uint32 HandleValues[] = { HostResult.IntValues[0], HostResult.IntValues[1] };
		if (!WriteGuestBytes(
			GuestMemory,
			Arguments[2].I32,
			HandleValues,
			sizeof(HandleValues),
			OutFailureDetails))
		{
			return false;
		}
		break;
	}
	default:
		break;
	}

	SetIntegerResult(Signature, HostResult, OutResult);
	return true;
}
