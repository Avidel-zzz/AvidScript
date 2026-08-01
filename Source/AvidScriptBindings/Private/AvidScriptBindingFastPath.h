#pragma once

#include "AvidScriptBindingInvocation.h"

class FProperty;
class UClass;
class UFunction;

namespace UE::AvidScript::BindingPrivate
{
enum class EFastPathValueKind : uint8
{
	Unsupported,
	Int32,
	Vector,
	Object
};

struct FFastPathValueSpec
{
	FProperty* Property = nullptr;
	int32 ArgumentOffset = INDEX_NONE;
	EFastPathValueKind Kind = EFastPathValueKind::Unsupported;
	bool bIsInput = false;
};

struct FFastPathBuildSpec
{
	UFunction* Function = nullptr;
	int32 FrameSize = 0;
	int32 FrameAlignment = 1;
	int32 ExpectedArgumentCount = 0;
	bool bStatic = false;
	bool bRequiresWriteAccess = false;
	bool bHasReloadEffect = false;
	bool bQualifiedNativeDirectAuthorized = false;
	TConstArrayView<FFastPathValueSpec> Parameters;
	FFastPathValueSpec ReturnValue;
};

struct FFastPathPlan;

using FFastPathThunk = bool(*)(
	const FFastPathPlan& Plan,
	UObject& Target,
	const FAvidScriptDynamicHostCall& Call,
	TArray<uint8>& InvocationScratch,
	FString& OutErrorCategory,
	FString& OutErrorDetails);

struct FFastPathPlan
{
	EAvidScriptBindingFastPathKind Kind = EAvidScriptBindingFastPathKind::None;
	EAvidScriptBindingInvocationMode HighestInvocationMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	UFunction* Function = nullptr;
	UClass* NativeDirectOwnerClass = nullptr;
	FNativeFuncPtr NativeFunction = nullptr;
	bool bAdaptiveNativeEligible = false;
	FFastPathThunk SemanticThunk = nullptr;
	FFastPathThunk NativeDirectThunk = nullptr;
	int32 FrameSize = 0;
	int32 FrameAlignment = 1;
	int32 ParameterFrameOffsets[2] = { INDEX_NONE, INDEX_NONE };
	int32 ReturnFrameOffset = INDEX_NONE;
	int32 ReturnGuestArgumentOffset = INDEX_NONE;

	bool IsBound() const
	{
		return Kind != EAvidScriptBindingFastPathKind::None
			&& SemanticThunk != nullptr;
	}
};

bool IsQualifiedNativeDirectFunction(const UFunction& Function);

bool TryBuildFastPath(
	const FFastPathBuildSpec& Spec,
	FFastPathPlan& OutPlan);

bool DispatchFastPath(
	const FFastPathPlan& Plan,
	UObject& Target,
	const FAvidScriptDynamicHostCall& Call,
	TArray<uint8>& InvocationScratch,
	EAvidScriptBindingInvocationPolicy InvocationPolicy,
	EAvidScriptBindingInvocationMode& OutInvocationMode,
	bool& bOutAdaptiveGuardRejected,
	FString& OutErrorCategory,
	FString& OutErrorDetails);

bool InvokePreparedScalarI32PairToI32(
	const FFastPathPlan& Plan,
	UObject& Target,
	int32 Left,
	int32 Right,
	EAvidScriptBindingInvocationPolicy InvocationPolicy,
	int32& OutValue,
	EAvidScriptBindingInvocationMode& OutInvocationMode,
	FString& OutErrorCategory,
	FString& OutErrorDetails);

bool ValidatePreparedNativeCallCell(
	const FFastPathPlan& Plan,
	UObject& Target);

bool ValidatePreparedNativeTarget(
	const UClass* ExpectedClass,
	UObject& Target,
	FString* OutErrorCategory = nullptr,
	FString* OutErrorDetails = nullptr);

bool InvokePreparedScalarI32PairCallCell(
	const FFastPathPlan& Plan,
	UObject& Target,
	int32 Left,
	int32 Right,
	bool bUseNative,
	int32& OutValue,
	FString& OutErrorCategory,
	FString& OutErrorDetails);

bool InvokePreparedVectorCallCell(
	const FFastPathPlan& Plan,
	UObject& Target,
	const FVector& Input,
	bool bUseNative,
	FVector& OutValue,
	FString& OutErrorCategory,
	FString& OutErrorDetails);

bool InvokePreparedObjectCallCell(
	const FFastPathPlan& Plan,
	UObject& Target,
	UObject* Input,
	bool bUseNative,
	UObject*& OutValue,
	FString& OutErrorCategory,
	FString& OutErrorDetails);
} // namespace UE::AvidScript::BindingPrivate
