#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptVmTypedHostShape : uint8
{
	None,
	EmptyI32,
	I32PairToI32,
	SelfI32PairToI32,
	SelfPropertyI32GetSet,
	SelfPropertyI32Get,
	SelfPropertyI32Set,
	PackedSelfPropertyI32Get,
	PackedSelfPropertyI32Set,
	PackedSelfPropertyI64Get,
	PackedSelfPropertyI64Set,
	PackedSelfPropertyF32Get,
	PackedSelfPropertyF32Set,
	PackedSelfPropertyF64Get,
	PackedSelfPropertyF64Set,
	SelfVectorValue,
	StableObjectRoundtrip,
	CommandBufferSubmit,
	SelfI32PairToGuestI32,
	SelfF32TripleToGuestVector
};

enum class EAvidScriptVmTypedHostStatus : uint8
{
	Succeeded,
	Rejected,
	FallbackRequired
};

static_assert(sizeof(EAvidScriptVmTypedHostShape) == 1);
static_assert(sizeof(EAvidScriptVmTypedHostStatus) == 1);

using FAvidScriptVmPreparedSelfI32PairTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue);

using FAvidScriptVmPreparedSelfI32PairGuestResultTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32 GuestAddress,
		int32& OutStatus);

using FAvidScriptVmPreparedSelfF32TripleGuestVectorTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		float X,
		float Y,
		float Z,
		int32 GuestAddress,
		int32& OutStatus);

using FAvidScriptVmPreparedSelfGuestAddressTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue);

using FAvidScriptVmPreparedStableObjectRoundtripTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 ObjectSlot,
		int32 ObjectGeneration,
		int32 GuestAddress,
		int32& OutValue);

using FAvidScriptVmPreparedSelfPropertyI32GetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32& OutValue);

using FAvidScriptVmPreparedSelfPropertyI32SetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Value);

using FAvidScriptVmPreparedPackedSelfPropertyF32GetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int64 PackedSelf,
		float& OutValue);

using FAvidScriptVmPreparedPackedSelfPropertyF32SetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int64 PackedSelf,
		float Value);

using FAvidScriptVmPreparedPackedSelfPropertyI32GetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int64 PackedSelf,
		int32& OutValue);

using FAvidScriptVmPreparedPackedSelfPropertyI32SetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int64 PackedSelf,
		int32 Value);

using FAvidScriptVmPreparedPackedSelfPropertyI64GetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int64 PackedSelf,
		int64& OutValue);

using FAvidScriptVmPreparedPackedSelfPropertyI64SetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int64 PackedSelf,
		int64 Value);

using FAvidScriptVmPreparedPackedSelfPropertyF64GetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int64 PackedSelf,
		double& OutValue);

using FAvidScriptVmPreparedPackedSelfPropertyF64SetTarget =
	EAvidScriptVmTypedHostStatus (*)(
		void* Context,
		int64 PackedSelf,
		double Value);

struct AVIDSCRIPTVM_API FAvidScriptVmPreparedTypedHostTarget
{
	void* Context = nullptr;
	FAvidScriptVmPreparedSelfI32PairTarget SelfI32Pair = nullptr;
	FAvidScriptVmPreparedSelfI32PairGuestResultTarget
		SelfI32PairGuestResult = nullptr;
	FAvidScriptVmPreparedSelfF32TripleGuestVectorTarget
		SelfF32TripleGuestVector = nullptr;
	FAvidScriptVmPreparedSelfGuestAddressTarget SelfGuestAddress = nullptr;
	FAvidScriptVmPreparedStableObjectRoundtripTarget StableObjectRoundtrip =
		nullptr;
	FAvidScriptVmPreparedSelfPropertyI32GetTarget SelfPropertyI32Get = nullptr;
	FAvidScriptVmPreparedSelfPropertyI32SetTarget SelfPropertyI32Set = nullptr;
	FAvidScriptVmPreparedPackedSelfPropertyI32GetTarget PackedSelfPropertyI32Get = nullptr;
	FAvidScriptVmPreparedPackedSelfPropertyI32SetTarget PackedSelfPropertyI32Set = nullptr;
	FAvidScriptVmPreparedPackedSelfPropertyI64GetTarget PackedSelfPropertyI64Get = nullptr;
	FAvidScriptVmPreparedPackedSelfPropertyI64SetTarget PackedSelfPropertyI64Set = nullptr;
	FAvidScriptVmPreparedPackedSelfPropertyF32GetTarget PackedSelfPropertyF32Get = nullptr;
	FAvidScriptVmPreparedPackedSelfPropertyF32SetTarget PackedSelfPropertyF32Set = nullptr;
	FAvidScriptVmPreparedPackedSelfPropertyF64GetTarget PackedSelfPropertyF64Get = nullptr;
	FAvidScriptVmPreparedPackedSelfPropertyF64SetTarget PackedSelfPropertyF64Set = nullptr;

	bool IsBoundForShape(const EAvidScriptVmTypedHostShape Shape) const
	{
		if (Context == nullptr)
		{
			return false;
		}
		switch (Shape)
		{
		case EAvidScriptVmTypedHostShape::SelfI32PairToI32:
			return SelfI32Pair != nullptr;
		case EAvidScriptVmTypedHostShape::SelfI32PairToGuestI32:
			return SelfI32PairGuestResult != nullptr;
		case EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector:
			return SelfF32TripleGuestVector != nullptr;
		case EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet:
		case EAvidScriptVmTypedHostShape::SelfVectorValue:
			return SelfGuestAddress != nullptr;
		case EAvidScriptVmTypedHostShape::StableObjectRoundtrip:
			return StableObjectRoundtrip != nullptr;
		case EAvidScriptVmTypedHostShape::SelfPropertyI32Get:
			return SelfPropertyI32Get != nullptr;
		case EAvidScriptVmTypedHostShape::SelfPropertyI32Set:
			return SelfPropertyI32Set != nullptr;
		case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get:
			return PackedSelfPropertyI32Get != nullptr;
		case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Set:
			return PackedSelfPropertyI32Set != nullptr;
		case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Get:
			return PackedSelfPropertyI64Get != nullptr;
		case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Set:
			return PackedSelfPropertyI64Set != nullptr;
		case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Get:
			return PackedSelfPropertyF32Get != nullptr;
		case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Set:
			return PackedSelfPropertyF32Set != nullptr;
		case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Get:
			return PackedSelfPropertyF64Get != nullptr;
		case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Set:
			return PackedSelfPropertyF64Set != nullptr;
		default:
			return false;
		}
	}

	bool HasAnyTarget() const
	{
		return SelfI32Pair != nullptr
			|| SelfI32PairGuestResult != nullptr
			|| SelfF32TripleGuestVector != nullptr
			|| SelfGuestAddress != nullptr
			|| StableObjectRoundtrip != nullptr
			|| SelfPropertyI32Get != nullptr
			|| SelfPropertyI32Set != nullptr
			|| PackedSelfPropertyI32Get != nullptr
			|| PackedSelfPropertyI32Set != nullptr
			|| PackedSelfPropertyI64Get != nullptr
			|| PackedSelfPropertyI64Set != nullptr
			|| PackedSelfPropertyF32Get != nullptr
			|| PackedSelfPropertyF32Set != nullptr
			|| PackedSelfPropertyF64Get != nullptr
			|| PackedSelfPropertyF64Set != nullptr;
	}
};

class AVIDSCRIPTVM_API IAvidScriptVmTypedHostDispatcher
{
public:
	virtual ~IAvidScriptVmTypedHostDispatcher() = default;

	virtual EAvidScriptVmTypedHostStatus DispatchEmptyI32(
		uint32 BindingOrdinal,
		int32& OutValue) = 0;

	virtual EAvidScriptVmTypedHostStatus DispatchI32PairToI32(
		uint32 BindingOrdinal,
		int32 Left,
		int32 Right,
		int32& OutValue) = 0;

	virtual EAvidScriptVmTypedHostStatus DispatchSelfI32PairToI32(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue) = 0;

	virtual EAvidScriptVmTypedHostStatus DispatchSelfPropertyI32GetSet(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue) = 0;

	virtual EAvidScriptVmTypedHostStatus DispatchSelfVectorValue(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue) = 0;

	virtual EAvidScriptVmTypedHostStatus DispatchStableObjectRoundtrip(
		uint32 BindingOrdinal,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 ObjectSlot,
		int32 ObjectGeneration,
		int32 GuestAddress,
		int32& OutValue) = 0;

	virtual EAvidScriptVmTypedHostStatus DispatchCommandBufferSubmit(
		uint32 BindingOrdinal,
		int32 GuestAddress,
		int32 ByteCount,
		int32& OutValue) = 0;
};

struct AVIDSCRIPTVM_API FAvidScriptVmTypedHostImport
{
	FString StableId;
	uint32 BindingOrdinal = MAX_uint32;
	FString ModuleName;
	FString ImportName;
	FString Signature;
	EAvidScriptVmTypedHostShape Shape = EAvidScriptVmTypedHostShape::None;
	bool bSupplementalRuntimeAuthority = false;
	FAvidScriptVmPreparedTypedHostTarget PreparedTarget;
};
