#pragma once

#include "AvidScriptBindingInvocation.h"

class FProperty;
class UClass;
class UFunction;

namespace UE::AvidScript::BindingPrivate
{
struct FFastPathValueSpec
{
	FProperty* Property = nullptr;
	int32 ArgumentOffset = INDEX_NONE;
	bool bIsValue = false;
	bool bIsInt32 = false;
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
	FString& OutErrorCategory,
	FString& OutErrorDetails);
} // namespace UE::AvidScript::BindingPrivate
