#include "AvidScriptEditorBindingSelectionResolver.h"

#include "BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/UObjectGlobals.h"

namespace
{
constexpr const TCHAR* NativeDirectAuthorizationNextAction =
	TEXT("Authorize only an accepted include_functions member after asserting standard UHT exec-thunk provenance, accepting bypass of inherited/custom ProcessEvent, and never auto-authorizing Engine or third-party functions.");

FString MakeResolvedSelectionKey(const FString& OwnerClassPath, const FName FunctionName)
{
	return OwnerClassPath + TEXT(".") + FunctionName.ToString();
}

FString MakeResolvedSelectionKey(const FAvidScriptReflectedFunctionSelection& Selection)
{
	return MakeResolvedSelectionKey(Selection.OwnerClassPath, Selection.FunctionName);
}

void SetFailure(
	FAvidScriptBindingSelectionResolveResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript binding selection error | category=%s | source=%s | next=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*NextAction);
}

void AddIssue(
	FAvidScriptBindingSelectionResolveResult& OutResult,
	const bool bFatal,
	const FString& OwnerClassPath,
	const FName FunctionName,
	const FString& Category,
	const FString& Source)
{
	FAvidScriptBindingSelectionIssue Issue;
	Issue.bFatal = bFatal;
	Issue.OwnerClassPath = OwnerClassPath;
	Issue.FunctionName = FunctionName;
	Issue.Category = Category;
	Issue.Source = Source;
	OutResult.Issues.Add(MoveTemp(Issue));
	++OutResult.RejectedFunctionCount;
}

void SortIssues(TArray<FAvidScriptBindingSelectionIssue>& Issues)
{
	Issues.Sort([](const FAvidScriptBindingSelectionIssue& Left, const FAvidScriptBindingSelectionIssue& Right)
	{
		const FString LeftKey = Left.OwnerClassPath
			+ TEXT(".") + Left.FunctionName.ToString()
			+ TEXT(":") + Left.Category
			+ TEXT(":") + Left.Source;
		const FString RightKey = Right.OwnerClassPath
			+ TEXT(".") + Right.FunctionName.ToString()
			+ TEXT(":") + Right.Category
			+ TEXT(":") + Right.Source;
		return LeftKey.Compare(RightKey, ESearchCase::CaseSensitive) < 0;
	});
}

bool EvaluateFunction(
	const UFunction* Function,
	FString& OutCategory,
	FString& OutSource)
{
	if (!FAvidScriptEditorReflectedFunctionPolicy::Evaluate(Function, OutCategory, OutSource))
	{
		return false;
	}

	FAvidScriptProjectedFunction Projection;
	FString ProjectionErrorSource;
	if (!FAvidScriptEditorReflectedTypePolicy::ProjectFunction(
		Function,
		Function->HasAnyFunctionFlags(FUNC_Static),
		Projection,
		ProjectionErrorSource))
	{
		OutCategory = TEXT("unsupported_property");
		OutSource = ProjectionErrorSource;
		return false;
	}
	return true;
}

bool AddAcceptedSelection(
	const FAvidScriptReflectedFunctionSelection& Selection,
	TSet<FString>& SeenSelections,
	TArray<FAvidScriptReflectedFunctionSelection>& OutSelections,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	const FString Key = MakeResolvedSelectionKey(Selection);
	if (SeenSelections.Contains(Key))
	{
		AddIssue(
			OutResult,
			true,
			Selection.OwnerClassPath,
			Selection.FunctionName,
			TEXT("duplicate_selection"),
			Key);
		SetFailure(
			OutResult,
			TEXT("duplicate_selection"),
			Key,
			TEXT("Remove overlapping class discovery and explicit function selections."));
		return false;
	}
	SeenSelections.Add(Key);
	OutSelections.Add(Selection);
	return true;
}

bool FailAndClear(
	TArray<FAvidScriptReflectedFunctionSelection>& OutSelections,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	OutSelections.Empty();
	OutResult.AcceptedFunctionCount = 0;
	SortIssues(OutResult.Issues);
	return false;
}
} // namespace

bool FAvidScriptEditorBindingSelectionResolver::Resolve(
	const FAvidScriptBindingSelectionProfile& Profile,
	TArray<FAvidScriptReflectedFunctionSelection>& OutSelections,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	OutSelections.Empty();
	OutResult = FAvidScriptBindingSelectionResolveResult();
	if (Profile.PackageName.IsEmpty())
	{
		SetFailure(
			OutResult,
			TEXT("package_name_missing"),
			Profile.PackageName,
			TEXT("Provide a stable non-empty binding package name."));
		return false;
	}
	if (Profile.Classes.IsEmpty() && Profile.ExplicitFunctions.IsEmpty())
	{
		SetFailure(
			OutResult,
			TEXT("profile_empty"),
			Profile.PackageName,
			TEXT("Add at least one reflected class rule or explicit function."));
		return false;
	}

	TArray<FAvidScriptReflectedClassSelection> ClassRules = Profile.Classes;
	ClassRules.Sort([](const FAvidScriptReflectedClassSelection& Left, const FAvidScriptReflectedClassSelection& Right)
	{
		return Left.OwnerClassPath.Compare(Right.OwnerClassPath, ESearchCase::CaseSensitive) < 0;
	});
	TSet<FString> SeenClassRules;
	TSet<FString> SeenSelections;
	for (const FAvidScriptReflectedClassSelection& Rule : ClassRules)
	{
		if (Rule.OwnerClassPath.IsEmpty() || SeenClassRules.Contains(Rule.OwnerClassPath))
		{
			const FString Category = Rule.OwnerClassPath.IsEmpty()
				? FString(TEXT("class_missing"))
				: FString(TEXT("duplicate_class_rule"));
			SetFailure(
				OutResult,
				Category,
				Rule.OwnerClassPath,
				TEXT("Keep each non-empty reflected class path exactly once in the profile."));
			return FailAndClear(OutSelections, OutResult);
		}
		SeenClassRules.Add(Rule.OwnerClassPath);

		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Rule.OwnerClassPath);
		if (OwnerClass == nullptr)
		{
			SetFailure(
				OutResult,
				TEXT("class_missing"),
				Rule.OwnerClassPath,
				TEXT("Use a reflected UClass path available in the active UE5.8 build."));
			return FailAndClear(OutSelections, OutResult);
		}

		TSet<FName> IncludeNames(Rule.IncludeFunctions);
		TSet<FName> ExcludeNames(Rule.ExcludeFunctions);
		TSet<FName> SeenNativeDirectNames;
		for (const FName NativeDirectName : Rule.NativeDirectFunctions)
		{
			const FString Source =
				MakeResolvedSelectionKey(Rule.OwnerClassPath, NativeDirectName);
			if (SeenNativeDirectNames.Contains(NativeDirectName))
			{
				AddIssue(
					OutResult,
					true,
					Rule.OwnerClassPath,
					NativeDirectName,
					TEXT("native_direct_function_duplicate"),
					Source);
				SetFailure(
					OutResult,
					TEXT("native_direct_function_duplicate"),
					Source,
					NativeDirectAuthorizationNextAction);
				return FailAndClear(OutSelections, OutResult);
			}
			SeenNativeDirectNames.Add(NativeDirectName);
			if (!IncludeNames.Contains(NativeDirectName))
			{
				AddIssue(
					OutResult,
					true,
					Rule.OwnerClassPath,
					NativeDirectName,
					TEXT("native_direct_function_not_included"),
					Source);
				SetFailure(
					OutResult,
					TEXT("native_direct_function_not_included"),
					Source,
					NativeDirectAuthorizationNextAction);
				return FailAndClear(OutSelections, OutResult);
			}
		}

		TSet<FName> FoundIncludeNames;
		TSet<FName> DeclaredFunctionNames;
		TArray<UFunction*> Candidates;
		for (TFieldIterator<UFunction> It(OwnerClass, EFieldIterationFlags::None); It; ++It)
		{
			UFunction* Function = *It;
			if (Function == nullptr)
			{
				continue;
			}
			DeclaredFunctionNames.Add(Function->GetFName());
			if ((!IncludeNames.IsEmpty() && !IncludeNames.Contains(Function->GetFName()))
				|| ExcludeNames.Contains(Function->GetFName()))
			{
				continue;
			}
			FoundIncludeNames.Add(Function->GetFName());
			Candidates.Add(Function);
		}
		Candidates.Sort([](const UFunction& Left, const UFunction& Right)
		{
			return Left.GetName().Compare(Right.GetName(), ESearchCase::CaseSensitive) < 0;
		});

		for (const FName NativeDirectName : Rule.NativeDirectFunctions)
		{
			if (!DeclaredFunctionNames.Contains(NativeDirectName))
			{
				const FString Source =
					MakeResolvedSelectionKey(Rule.OwnerClassPath, NativeDirectName);
				AddIssue(
					OutResult,
					true,
					Rule.OwnerClassPath,
					NativeDirectName,
					TEXT("native_direct_function_unknown"),
					Source);
				SetFailure(
					OutResult,
					TEXT("native_direct_function_unknown"),
					Source,
					NativeDirectAuthorizationNextAction);
				return FailAndClear(OutSelections, OutResult);
			}
		}

		for (const FName IncludeName : IncludeNames)
		{
			if (!FoundIncludeNames.Contains(IncludeName) && !ExcludeNames.Contains(IncludeName))
			{
				const FString Source = MakeResolvedSelectionKey(Rule.OwnerClassPath, IncludeName);
				AddIssue(
					OutResult,
					true,
					Rule.OwnerClassPath,
					IncludeName,
					TEXT("function_missing"),
					Source);
				SetFailure(
					OutResult,
					TEXT("function_missing"),
					Source,
					TEXT("Update the class include filter to functions declared by the selected class."));
				return FailAndClear(OutSelections, OutResult);
			}
		}

		TSet<FName> AcceptedNames;
		for (UFunction* Function : Candidates)
		{
			++OutResult.CandidateFunctionCount;
			FString Category;
			FString Source;
			if (!EvaluateFunction(Function, Category, Source))
			{
				AddIssue(
					OutResult,
					false,
					Rule.OwnerClassPath,
					Function->GetFName(),
					Category,
					Source);
				continue;
			}
			if (!AddAcceptedSelection(
				{ Rule.OwnerClassPath, Function->GetFName() },
				SeenSelections,
				OutSelections,
				OutResult))
			{
				return FailAndClear(OutSelections, OutResult);
			}
			AcceptedNames.Add(Function->GetFName());
		}

		for (const FName NativeDirectName : Rule.NativeDirectFunctions)
		{
			if (!AcceptedNames.Contains(NativeDirectName))
			{
				const FString Source =
					MakeResolvedSelectionKey(Rule.OwnerClassPath, NativeDirectName);
				AddIssue(
					OutResult,
					true,
					Rule.OwnerClassPath,
					NativeDirectName,
					TEXT("native_direct_function_rejected"),
					Source);
				SetFailure(
					OutResult,
					TEXT("native_direct_function_rejected"),
					Source,
					NativeDirectAuthorizationNextAction);
				return FailAndClear(OutSelections, OutResult);
			}
		}
	}

	TArray<FAvidScriptReflectedFunctionSelection> ExplicitFunctions = Profile.ExplicitFunctions;
	ExplicitFunctions.Sort([](const FAvidScriptReflectedFunctionSelection& Left, const FAvidScriptReflectedFunctionSelection& Right)
	{
		return MakeResolvedSelectionKey(Left).Compare(MakeResolvedSelectionKey(Right), ESearchCase::CaseSensitive) < 0;
	});
	for (const FAvidScriptReflectedFunctionSelection& Selection : ExplicitFunctions)
	{
		++OutResult.CandidateFunctionCount;
		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Selection.OwnerClassPath);
		UFunction* Function = OwnerClass == nullptr
			? nullptr
			: OwnerClass->FindFunctionByName(Selection.FunctionName);
		FString Category;
		FString Source;
		if (OwnerClass == nullptr)
		{
			Category = TEXT("class_missing");
			Source = Selection.OwnerClassPath;
		}
		else if (Function == nullptr)
		{
			Category = TEXT("function_missing");
			Source = MakeResolvedSelectionKey(Selection);
		}
		else if (EvaluateFunction(Function, Category, Source))
		{
			if (!AddAcceptedSelection(Selection, SeenSelections, OutSelections, OutResult))
			{
				return FailAndClear(OutSelections, OutResult);
			}
			continue;
		}

		AddIssue(
			OutResult,
			Profile.bStrictExplicitFunctions,
			Selection.OwnerClassPath,
			Selection.FunctionName,
			Category,
			Source);
		if (Profile.bStrictExplicitFunctions)
		{
			SetFailure(
				OutResult,
				Category,
				Source,
				TEXT("Fix or remove the incompatible explicit function selection."));
			return FailAndClear(OutSelections, OutResult);
		}
	}

	OutSelections.Sort([](const FAvidScriptReflectedFunctionSelection& Left, const FAvidScriptReflectedFunctionSelection& Right)
	{
		return MakeResolvedSelectionKey(Left).Compare(MakeResolvedSelectionKey(Right), ESearchCase::CaseSensitive) < 0;
	});
	SortIssues(OutResult.Issues);
	if (OutSelections.IsEmpty())
	{
		SetFailure(
			OutResult,
			TEXT("selection_empty"),
			Profile.PackageName,
			TEXT("Broaden the profile or add support for the rejected reflected types."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.AcceptedFunctionCount = OutSelections.Num();
	return true;
}
