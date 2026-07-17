#pragma once

#include "CoreMinimal.h"

struct FAvidScriptReflectedFunctionSelection
{
	FString OwnerClassPath;
	FName FunctionName;
};

struct FAvidScriptReflectedClassSelection
{
	FString OwnerClassPath;
	TArray<FName> IncludeFunctions;
	TArray<FName> ExcludeFunctions;
};

struct FAvidScriptBindingSelectionProfile
{
	FString PackageName;
	TArray<FAvidScriptReflectedClassSelection> Classes;
	TArray<FAvidScriptReflectedFunctionSelection> ExplicitFunctions;
	bool bStrictExplicitFunctions = true;
};

struct FAvidScriptBindingSelectionIssue
{
	bool bFatal = false;
	FString OwnerClassPath;
	FName FunctionName;
	FString Category;
	FString Source;
};

struct FAvidScriptBindingSelectionResolveResult
{
	bool bSucceeded = false;
	int32 CandidateFunctionCount = 0;
	int32 AcceptedFunctionCount = 0;
	int32 RejectedFunctionCount = 0;
	TArray<FAvidScriptBindingSelectionIssue> Issues;
	FString ErrorCategory;
	FString ErrorSource;
	FString NextAction;
	FString ErrorMessage;
};
