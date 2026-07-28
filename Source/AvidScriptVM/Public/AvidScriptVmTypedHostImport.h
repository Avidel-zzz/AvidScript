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
	SelfVectorValue,
	StableObjectRoundtrip,
	CommandBufferSubmit
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

struct AVIDSCRIPTVM_API FAvidScriptVmPreparedTypedHostTarget
{
	void* Context = nullptr;
	FAvidScriptVmPreparedSelfI32PairTarget SelfI32Pair = nullptr;
	FAvidScriptVmPreparedSelfPropertyI32GetTarget SelfPropertyI32Get = nullptr;
	FAvidScriptVmPreparedSelfPropertyI32SetTarget SelfPropertyI32Set = nullptr;

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
		case EAvidScriptVmTypedHostShape::SelfPropertyI32Get:
			return SelfPropertyI32Get != nullptr;
		case EAvidScriptVmTypedHostShape::SelfPropertyI32Set:
			return SelfPropertyI32Set != nullptr;
		default:
			return false;
		}
	}

	bool HasAnyTarget() const
	{
		return SelfI32Pair != nullptr
			|| SelfPropertyI32Get != nullptr
			|| SelfPropertyI32Set != nullptr;
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
	FAvidScriptVmPreparedTypedHostTarget PreparedTarget;
};
