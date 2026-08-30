#pragma once

#include "CoreMinimal.h"

struct FAvidScriptReflectedFunctionSelection
{
	FString OwnerClassPath;
	FName FunctionName;
};

struct FAvidScriptReflectedPropertySelection
{
	FString OwnerClassPath;
	FName PropertyName;
	bool bWritable = false;
};

struct FAvidScriptReflectedDelegateEventSelection
{
	FString OwnerClassPath;
	FName EventName;
	FString CallbackKind = TEXT("multicast");
	FString HandlerMode = TEXT("replace");
};

struct FAvidScriptReflectedClassSelection
{
	FString OwnerClassPath;
	TArray<FName> IncludeFunctions;
	TArray<FName> ExcludeFunctions;
	TArray<FName> NativeDirectFunctions;
	TArray<FName> GeneratedNativeFunctions;
	TArray<FName> IncludeProperties;
	TArray<FName> ExcludeProperties;
	TArray<FName> WritableProperties;
	TArray<FName> GeneratedNativeProperties;
	TArray<FName> IncludeEvents;
	TArray<FName> ExcludeEvents;
	TArray<FName> IncludeHandlers;
	TArray<FName> BeforeHandlers;
	TArray<FName> AfterHandlers;
	bool bDiscoverReadableProperties = false;
};

struct FAvidScriptBindingSelectionProfile
{
	FString PackageName;
	TArray<FAvidScriptReflectedClassSelection> Classes;
	TArray<FAvidScriptReflectedFunctionSelection> ExplicitFunctions;
	TArray<FAvidScriptReflectedPropertySelection> ExplicitProperties;
	TArray<FAvidScriptReflectedDelegateEventSelection> ExplicitDelegateEvents;
	bool bStrictExplicitFunctions = true;
	bool bStrictExplicitProperties = true;
	bool bStrictExplicitDelegateEvents = true;
	FString SelfClassPath;
};

struct FAvidScriptBindingSelectionIssue
{
	bool bFatal = false;
	FString OwnerClassPath;
	FName FunctionName;
	FName PropertyName;
	FName DelegateEventName;
	FString MemberKind = TEXT("function");
	FString Category;
	FString Source;
};

struct FAvidScriptBindingSelectionResolveResult
{
	bool bSucceeded = false;
	int32 CandidateFunctionCount = 0;
	int32 AcceptedFunctionCount = 0;
	int32 RejectedFunctionCount = 0;
	int32 CandidatePropertyCount = 0;
	int32 AcceptedPropertyCount = 0;
	int32 RejectedPropertyCount = 0;
	int32 CandidateWritablePropertyCount = 0;
	int32 AcceptedWritablePropertyCount = 0;
	int32 RejectedWritablePropertyCount = 0;
	int32 CandidateDelegateEventCount = 0;
	int32 AcceptedDelegateEventCount = 0;
	int32 RejectedDelegateEventCount = 0;
	TArray<FAvidScriptBindingSelectionIssue> Issues;
	FString ErrorCategory;
	FString ErrorSource;
	FString NextAction;
	FString ErrorMessage;
};
