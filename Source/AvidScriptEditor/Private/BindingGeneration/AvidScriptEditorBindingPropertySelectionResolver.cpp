#include "AvidScriptEditorBindingPropertySelectionResolver.h"

#include "BindingGeneration/AvidScriptEditorReflectedPropertyPolicy.h"
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
FString MakeResolvedPropertySelectionKey(const FString& OwnerClassPath, const FName PropertyName)
{
	return OwnerClassPath + TEXT(".") + PropertyName.ToString();
}

FString MakeResolvedPropertySelectionKey(const FAvidScriptReflectedPropertySelection& Selection)
{
	return MakeResolvedPropertySelectionKey(Selection.OwnerClassPath, Selection.PropertyName);
}

void SetPropertySelectionFailure(
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
		TEXT("AvidScript property selection error | category=%s | source=%s | next=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*NextAction);
}

void AddPropertySelectionIssue(
	FAvidScriptBindingSelectionResolveResult& OutResult,
	const bool bFatal,
	const FString& OwnerClassPath,
	const FName PropertyName,
	const FString& Category,
	const FString& Source)
{
	FAvidScriptBindingSelectionIssue Issue;
	Issue.bFatal = bFatal;
	Issue.OwnerClassPath = OwnerClassPath;
	Issue.PropertyName = PropertyName;
	Issue.MemberKind = TEXT("property");
	Issue.Category = Category;
	Issue.Source = Source;
	OutResult.Issues.Add(MoveTemp(Issue));
	++OutResult.RejectedPropertyCount;
}

void SortPropertySelectionIssues(TArray<FAvidScriptBindingSelectionIssue>& Issues)
{
	Issues.Sort([](const FAvidScriptBindingSelectionIssue& Left, const FAvidScriptBindingSelectionIssue& Right)
	{
		const FString LeftKey = Left.OwnerClassPath
			+ TEXT(".") + Left.PropertyName.ToString()
			+ TEXT(":") + Left.Category
			+ TEXT(":") + Left.Source;
		const FString RightKey = Right.OwnerClassPath
			+ TEXT(".") + Right.PropertyName.ToString()
			+ TEXT(":") + Right.Category
			+ TEXT(":") + Right.Source;
		return LeftKey.Compare(RightKey, ESearchCase::CaseSensitive) < 0;
	});
}

bool AddReadablePropertySelection(
	const FAvidScriptReflectedPropertySelection& Selection,
	TSet<FString>& SeenSelections,
	TArray<FAvidScriptReflectedPropertySelection>& OutSelections,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	const FString Key = MakeResolvedPropertySelectionKey(Selection);
	if (SeenSelections.Contains(Key))
	{
		AddPropertySelectionIssue(
			OutResult,
			true,
			Selection.OwnerClassPath,
			Selection.PropertyName,
			TEXT("duplicate_property_selection"),
			Key);
		SetPropertySelectionFailure(
			OutResult,
			TEXT("duplicate_property_selection"),
			Key,
			TEXT("Remove overlapping property discovery and explicit selections."));
		return false;
	}
	SeenSelections.Add(Key);
	OutSelections.Add(Selection);
	return true;
}

bool FailPropertySelection(
	TArray<FAvidScriptReflectedPropertySelection>& OutSelections,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	OutSelections.Empty();
	OutResult.AcceptedPropertyCount = 0;
	SortPropertySelectionIssues(OutResult.Issues);
	return false;
}
} // namespace

bool FAvidScriptEditorBindingPropertySelectionResolver::ResolveReadable(
	const FAvidScriptBindingSelectionProfile& Profile,
	TArray<FAvidScriptReflectedPropertySelection>& OutSelections,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	OutSelections.Empty();
	OutResult = FAvidScriptBindingSelectionResolveResult();
	if (Profile.PackageName.IsEmpty())
	{
		SetPropertySelectionFailure(
			OutResult,
			TEXT("package_name_missing"),
			Profile.PackageName,
			TEXT("Provide a stable non-empty binding package name."));
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
		const bool bSelectsProperties = Rule.bDiscoverReadableProperties || !Rule.IncludeProperties.IsEmpty();
		if (!bSelectsProperties)
		{
			continue;
		}
		if (Rule.OwnerClassPath.IsEmpty() || SeenClassRules.Contains(Rule.OwnerClassPath))
		{
			SetPropertySelectionFailure(
				OutResult,
				Rule.OwnerClassPath.IsEmpty() ? TEXT("class_missing") : TEXT("duplicate_class_rule"),
				Rule.OwnerClassPath,
				TEXT("Keep each non-empty reflected property class path exactly once."));
			return FailPropertySelection(OutSelections, OutResult);
		}
		SeenClassRules.Add(Rule.OwnerClassPath);

		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Rule.OwnerClassPath);
		if (OwnerClass == nullptr)
		{
			SetPropertySelectionFailure(
				OutResult,
				TEXT("class_missing"),
				Rule.OwnerClassPath,
				TEXT("Use a reflected UClass path available in the active UE5.8 build."));
			return FailPropertySelection(OutSelections, OutResult);
		}

		const TSet<FName> IncludeNames(Rule.IncludeProperties);
		const TSet<FName> ExcludeNames(Rule.ExcludeProperties);
		TSet<FName> FoundIncludeNames;
		TArray<FProperty*> Candidates;
		for (TFieldIterator<FProperty> It(OwnerClass, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (Property == nullptr
				|| (!Rule.bDiscoverReadableProperties && !IncludeNames.Contains(Property->GetFName()))
				|| (!IncludeNames.IsEmpty() && !IncludeNames.Contains(Property->GetFName()))
				|| ExcludeNames.Contains(Property->GetFName()))
			{
				continue;
			}
			FoundIncludeNames.Add(Property->GetFName());
			Candidates.Add(Property);
		}
		Candidates.Sort([](const FProperty& Left, const FProperty& Right)
		{
			return Left.GetName().Compare(Right.GetName(), ESearchCase::CaseSensitive) < 0;
		});

		for (const FName IncludeName : IncludeNames)
		{
			if (!FoundIncludeNames.Contains(IncludeName) && !ExcludeNames.Contains(IncludeName))
			{
				const FString Source = MakeResolvedPropertySelectionKey(Rule.OwnerClassPath, IncludeName);
				AddPropertySelectionIssue(OutResult, true, Rule.OwnerClassPath, IncludeName, TEXT("property_missing"), Source);
				SetPropertySelectionFailure(
					OutResult,
					TEXT("property_missing"),
					Source,
					TEXT("Update the property include filter to a member available on the selected class."));
				return FailPropertySelection(OutSelections, OutResult);
			}
		}

		for (FProperty* Property : Candidates)
		{
			++OutResult.CandidatePropertyCount;
			FString Category;
			FString Source;
			if (!FAvidScriptEditorReflectedPropertyPolicy::EvaluateReadable(Property, Category, Source))
			{
				AddPropertySelectionIssue(
					OutResult,
					false,
					Rule.OwnerClassPath,
					Property->GetFName(),
					Category,
					Source);
				continue;
			}
			if (!AddReadablePropertySelection(
				{ Rule.OwnerClassPath, Property->GetFName() },
				SeenSelections,
				OutSelections,
				OutResult))
			{
				return FailPropertySelection(OutSelections, OutResult);
			}
		}
	}

	TArray<FAvidScriptReflectedPropertySelection> ExplicitProperties = Profile.ExplicitProperties;
	ExplicitProperties.Sort([](const FAvidScriptReflectedPropertySelection& Left, const FAvidScriptReflectedPropertySelection& Right)
	{
		return MakeResolvedPropertySelectionKey(Left).Compare(MakeResolvedPropertySelectionKey(Right), ESearchCase::CaseSensitive) < 0;
	});
	for (const FAvidScriptReflectedPropertySelection& Selection : ExplicitProperties)
	{
		++OutResult.CandidatePropertyCount;
		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Selection.OwnerClassPath);
		FProperty* Property = OwnerClass == nullptr
			? nullptr
			: FindFProperty<FProperty>(OwnerClass, Selection.PropertyName);
		FString Category;
		FString Source;
		if (OwnerClass == nullptr)
		{
			Category = TEXT("class_missing");
			Source = Selection.OwnerClassPath;
		}
		else if (Property == nullptr)
		{
			Category = TEXT("property_missing");
			Source = MakeResolvedPropertySelectionKey(Selection);
		}
		else if (FAvidScriptEditorReflectedPropertyPolicy::EvaluateReadable(Property, Category, Source))
		{
			if (!AddReadablePropertySelection(Selection, SeenSelections, OutSelections, OutResult))
			{
				return FailPropertySelection(OutSelections, OutResult);
			}
			continue;
		}

		AddPropertySelectionIssue(
			OutResult,
			Profile.bStrictExplicitProperties,
			Selection.OwnerClassPath,
			Selection.PropertyName,
			Category,
			Source);
		if (Profile.bStrictExplicitProperties)
		{
			SetPropertySelectionFailure(
				OutResult,
				Category,
				Source,
				TEXT("Fix or remove the incompatible explicit property selection."));
			return FailPropertySelection(OutSelections, OutResult);
		}
	}

	OutSelections.Sort([](const FAvidScriptReflectedPropertySelection& Left, const FAvidScriptReflectedPropertySelection& Right)
	{
		return MakeResolvedPropertySelectionKey(Left).Compare(MakeResolvedPropertySelectionKey(Right), ESearchCase::CaseSensitive) < 0;
	});
	SortPropertySelectionIssues(OutResult.Issues);
	if (OutSelections.IsEmpty())
	{
		SetPropertySelectionFailure(
			OutResult,
			TEXT("property_selection_empty"),
			Profile.PackageName,
			TEXT("Enable readable property discovery, add compatible include names, or add explicit properties."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.AcceptedPropertyCount = OutSelections.Num();
	return true;
}
