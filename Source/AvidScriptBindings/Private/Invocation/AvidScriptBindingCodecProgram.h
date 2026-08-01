#pragma once

#include "AvidScriptBindingFastPath.h"
#include "AvidScriptBindingInvocationKind.h"
#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptGeneratedBindingRegistry.h"
#include "AvidScriptVmBackend.h"
#include "CoreMinimal.h"

class FProperty;
class UClass;
class UFunction;
class UObject;
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
	Object,
	Vector,
	Rotator,
	Transform
};

struct FValueCodecProgram
{
	FProperty* Property = nullptr;
	UClass* ObjectClass = nullptr;
	EValueCodecDirection Direction = EValueCodecDirection::Value;
	EValueCodecKind Kind = EValueCodecKind::Void;
	int32 ArgumentOffset = INDEX_NONE;
	int32 ArgumentWidth = 0;
	int32 GuestStorageSize = 0;
	FString Name;
};

struct FInvocationCodecProgram
{
	EAvidScriptBindingInvocationKind Kind =
		EAvidScriptBindingInvocationKind::ReflectedFunction;
	UClass* OwnerClass = nullptr;
	UFunction* Function = nullptr;
	FProperty* ReflectedProperty = nullptr;
	FString DebugPath;
	bool bStatic = false;
	bool bRequiresWriteAccess = false;
	EAvidScriptBindingReloadEffect ReloadEffect =
		EAvidScriptBindingReloadEffect::Unsupported;
	bool bRequiresGuestMemory = false;
	int32 FrameSize = 0;
	int32 FrameAlignment = 1;
	int32 RequiredScratchSize = 0;
	int32 ExpectedArgumentCount = 0;
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

bool WriteValueToGuest(
	const FValueCodecProgram& Program,
	uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	void* Frame,
	FString& OutDetails);
} // namespace UE::AvidScript::BindingPrivate
