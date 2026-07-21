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
};

struct FAvidScriptReflectedClassSelection
{
	FString OwnerClassPath;
	TArray<FName> IncludeFunctions;
	TArray<FName> ExcludeFunctions;
	TArray<FName> IncludeProperties;
	TArray<FName> ExcludeProperties;
	bool bDiscoverReadableProperties = false;
};

struct FAvidScriptBindingSelectionProfile
{
	FString PackageName;
	TArray<FAvidScriptReflectedClassSelection> Classes;
	TArray<FAvidScriptReflectedFunctionSelection> ExplicitFunctions;
	TArray<FAvidScriptReflectedPropertySelection> ExplicitProperties;
	bool bStrictExplicitFunctions = true;
	bool bStrictExplicitProperties = true;
};

struct FAvidScriptBindingSelectionIssue
{
	bool bFatal = false;
	FString OwnerClassPath;
	FName FunctionName;
	FName PropertyName;
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
	TArray<FAvidScriptBindingSelectionIssue> Issues;
	FString ErrorCategory;
	FString ErrorSource;
	FString NextAction;
	FString ErrorMessage;
};
