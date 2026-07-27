#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptVmTypedHostShape : uint8
{
	None,
	EmptyI32,
	I32PairToI32,
	SelfI32PairToI32,
	SelfPropertyI32GetSet,
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
};
