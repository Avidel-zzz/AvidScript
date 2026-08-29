#pragma once

#include "AvidScriptBindingLatent.h"
#include "AvidScriptBindingFastPath.h"
#include "AvidScriptArrayValueHeap.h"
#include "AvidScriptBindingInvocationKind.h"
#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptGeneratedBindingRegistry.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptUtf8ValueHeap.h"
#include "AvidScriptVmBackend.h"
#include "CoreMinimal.h"

class FProperty;
class FObjectPropertyBase;
class FStructProperty;
class UClass;
class UFunction;
class UObject;
class UScriptStruct;
class IAvidScriptVmGuestMemory;
struct FAvidScriptBindingInvocationContext;

namespace UE::AvidScript::BindingPrivate
{
enum class EValueCodecDirection : uint8
{
	Value,
	ConstRef,
	Ref,
	Out,
	Return
};

enum class EValueCodecKind : uint8
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
	String,
	Object,
	Vector,
	Rotator,
	Transform,
	StructWire,
	Array
};

struct FValueCodecProgram
{
	FProperty* Property = nullptr;
	UClass* ObjectClass = nullptr;
	UScriptStruct* StructType = nullptr;
	EValueCodecDirection Direction = EValueCodecDirection::Value;
	EValueCodecKind Kind = EValueCodecKind::Void;
	int32 ArgumentOffset = INDEX_NONE;
	int32 ArgumentWidth = 0;
	int32 GuestStorageSize = 0;
	int32 WireOffset = 0;
	int32 WireSize = 0;
	int32 WireAlignment = 1;
	FString Name;
	FString TypeId;
	TArray<FValueCodecProgram> Children;
};

struct FCodecOutputTransaction
{
	TArray<FAvidScriptObjectHandle, TInlineAllocator<128>> BorrowedHandles;
	TArray<FAvidScriptUtf8ValueReservation, TInlineAllocator<16>>
		Utf8Reservations;
	TArray<uint32, TInlineAllocator<16>> CreatedUtf8Tokens;
	TArray<FAvidScriptArrayValueReservation, TInlineAllocator<16>>
		ArrayReservations;
	TArray<uint32, TInlineAllocator<16>> CreatedArrayTokens;
	FAvidScriptUtf8ValueHeap* Utf8ValueHeap = nullptr;
	FAvidScriptArrayValueHeap* ArrayValueHeap = nullptr;
	int32 NextUtf8Reservation = 0;
	int32 NextArrayReservation = 0;

	bool ReserveUtf8Value(
		const FAvidScriptBindingInvocationContext& Context,
		FString& OutDetails);
	bool InternNextUtf8Value(
		TConstArrayView<uint8> Bytes,
		const FAvidScriptBindingInvocationContext& Context,
		uint32& OutToken,
		FString& OutDetails);
	bool ReserveArrayValue(
		const FAvidScriptBindingInvocationContext& Context,
		FString& OutDetails);
	bool PublishNextArrayValue(
		const FString& TypeId,
		int32 ElementCount,
		int32 ElementStride,
		int32 ElementAlignment,
		TConstArrayView<uint8> Bytes,
		const FAvidScriptBindingInvocationContext& Context,
		uint32& OutToken,
		FString& OutDetails);
	void Commit();
	void Rollback(const FAvidScriptBindingInvocationContext& Context);
};

struct FPreparedValueOutput
{
	TArrayView<uint8> GuestBytes;
	TArray<uint8> EncodedBytes;
};

struct FInvocationCodecProgram
{
	EAvidScriptBindingInvocationKind Kind =
		EAvidScriptBindingInvocationKind::ReflectedFunction;
	UClass* OwnerClass = nullptr;
	UFunction* Function = nullptr;
	FProperty* ReflectedProperty = nullptr;
	FStructProperty* LatentInfoProperty = nullptr;
	FObjectPropertyBase* WorldContextProperty = nullptr;
	FAvidScriptBindingLatentCompletionContract LatentCompletion;
	FString DebugPath;
	bool bStatic = false;
	bool bRequiresWriteAccess = false;
	bool bLatent = false;
	EAvidScriptBindingReloadEffect ReloadEffect =
		EAvidScriptBindingReloadEffect::Unsupported;
	bool bRequiresGuestMemory = false;
	int32 FrameSize = 0;
	int32 FrameAlignment = 1;
	int32 RequiredScratchSize = 0;
	int32 ExpectedArgumentCount = 0;
	int32 CallbackIdArgumentOffset = INDEX_NONE;
	TArray<FValueCodecProgram> Parameters;
	FValueCodecProgram ReturnValue;
	FFastPathPlan FastPath;
	FAvidScriptGeneratedBindingLease GeneratedLease;
	const FAvidScriptGeneratedBindingEntry* GeneratedEntry = nullptr;
	FAvidScriptVmTypedHostImport TypedHostImport;
};

bool ResolveObjectHandle(
	uint32 Slot,
	uint32 Generation,
	UClass* ExpectedClass,
	const FAvidScriptBindingInvocationContext& Context,
	bool bAllowNull,
	UObject*& OutObject,
	FString& OutDetails);

bool SetValueFromCells(
	const FValueCodecProgram& Program,
	TConstArrayView<uint64> Cells,
	IAvidScriptVmGuestMemory* GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails);

bool SetValueFromGuest(
	const FValueCodecProgram& Program,
	uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails);

bool SetStructValueFromGuest(
	const FValueCodecProgram& Program,
	uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* StructValue,
	FString& OutDetails);

bool ResolveGuestAddress(
	uint64 Cell,
	uint32 ByteCount,
	uint32& OutGuestAddress,
	FString& OutDetails);

bool PreflightValueOutput(
	const FValueCodecProgram& Program,
	uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	FCodecOutputTransaction& Transaction,
	FPreparedValueOutput& OutPreparedOutput,
	FString& OutDetails);

bool WriteValueToGuest(
	const FValueCodecProgram& Program,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FCodecOutputTransaction& Transaction,
	FPreparedValueOutput& PreparedOutput,
	FString& OutDetails);

void PublishValueOutput(FPreparedValueOutput& PreparedOutput);
} // namespace UE::AvidScript::BindingPrivate
