#include "AvidScriptEditorBindingDelegateEventSelectionResolver.h"

#include "BindingGeneration/AvidScriptEditorReflectedDelegateEventPolicy.h"
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
FString MakeEventSelectionKey(
	const FAvidScriptReflectedDelegateEventSelection& Selection)
{
	return Selection.OwnerClassPath + TEXT(".")
		+ Selection.EventName.ToString();
}

void SetDelegateEventSelectionFailure(
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
		TEXT("AvidScript delegate event selection error | category=%s | source=%s | next=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*NextAction);
}

void AddIssue(
	FAvidScriptBindingSelectionResolveResult& OutResult,
	const bool bFatal,
	const FAvidScriptReflectedDelegateEventSelection& Selection,
	const FString& Category,
	const FString& Source)
{
	FAvidScriptBindingSelectionIssue Issue;
	Issue.bFatal = bFatal;
	Issue.OwnerClassPath = Selection.OwnerClassPath;
	Issue.DelegateEventName = Selection.EventName;
	Issue.MemberKind = TEXT("delegate_event");
	Issue.Category = Category;
	Issue.Source = Source;
	OutResult.Issues.Add(MoveTemp(Issue));
	++OutResult.RejectedDelegateEventCount;
}

bool ResolveOne(
	const FAvidScriptReflectedDelegateEventSelection& Selection,
	const bool bFatal,
	TSet<FString>& SeenSelections,
	TArray<FAvidScriptReflectedDelegateEventSelection>& OutSelections,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	++OutResult.CandidateDelegateEventCount;
	const FString Key = MakeEventSelectionKey(Selection);
	UClass* OwnerClass = Selection.OwnerClassPath.IsEmpty()
		? nullptr
		: LoadObject<UClass>(nullptr, *Selection.OwnerClassPath);
	FProperty* Property = OwnerClass == nullptr
		? nullptr
		: FindFProperty<FProperty>(OwnerClass, Selection.EventName);
	FString Category;
	FString Source;
	if (OwnerClass == nullptr)
	{
		Category = TEXT("class_missing");
		Source = Selection.OwnerClassPath;
	}
	else if (Property == nullptr)
	{
		Category = TEXT("delegate_event_missing");
		Source = Key;
	}
	else if (Property->GetOwnerStruct() != OwnerClass)
	{
		Category = TEXT("delegate_event_owner_mismatch");
		Source = Key;
	}
	else if (const FMulticastDelegateProperty* DelegateProperty =
		CastField<FMulticastDelegateProperty>(Property))
	{
		FAvidScriptProjectedDelegateEvent Projection;
		if (FAvidScriptEditorReflectedDelegateEventPolicy::EvaluateAndProject(
				DelegateProperty,
				Projection,
				Category,
				Source))
		{
			if (SeenSelections.Contains(Key))
			{
				Category = TEXT("duplicate_delegate_event_selection");
				Source = Key;
			}
			else
			{
				SeenSelections.Add(Key);
				OutSelections.Add(Selection);
				return true;
			}
		}
	}
	else
	{
		Category = Property->IsA<FDelegateProperty>()
			? FString(TEXT("delegate_event_singlecast_unsupported"))
			: FString(TEXT("delegate_event_property_required"));
		Source = Key;
	}

	AddIssue(OutResult, bFatal, Selection, Category, Source);
	if (bFatal)
	{
		SetDelegateEventSelectionFailure(
			OutResult,
			Category,
			Source,
			TEXT("Select a declared dynamic multicast delegate with a supported value-only signature of at most eight ABI cells."));
		return false;
	}
	return true;
}
} // namespace

bool FAvidScriptEditorBindingDelegateEventSelectionResolver::Resolve(
	const FAvidScriptBindingSelectionProfile& Profile,
	TArray<FAvidScriptReflectedDelegateEventSelection>& OutSelections,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	OutSelections.Reset();
	OutResult = FAvidScriptBindingSelectionResolveResult();
	if (Profile.PackageName.IsEmpty())
	{
		SetDelegateEventSelectionFailure(
			OutResult,
			TEXT("package_name_missing"),
			Profile.PackageName,
			TEXT("Provide a stable non-empty binding package name."));
		return false;
	}

	TArray<FAvidScriptReflectedClassSelection> ClassRules = Profile.Classes;
	ClassRules.Sort([](
		const FAvidScriptReflectedClassSelection& Left,
		const FAvidScriptReflectedClassSelection& Right)
	{
		return Left.OwnerClassPath.Compare(
			Right.OwnerClassPath,
			ESearchCase::CaseSensitive) < 0;
	});
	TSet<FString> SeenSelections;
	for (const FAvidScriptReflectedClassSelection& Rule : ClassRules)
	{
		TArray<FName> EventNames = Rule.IncludeEvents;
		EventNames.Sort([](const FName Left, const FName Right)
		{
			return Left.ToString().Compare(
				Right.ToString(),
				ESearchCase::CaseSensitive) < 0;
		});
		for (const FName EventName : EventNames)
		{
			if (Rule.ExcludeEvents.Contains(EventName))
			{
				const FAvidScriptReflectedDelegateEventSelection Selection = {
					Rule.OwnerClassPath,
					EventName
				};
				const FString Source = MakeEventSelectionKey(Selection);
				++OutResult.CandidateDelegateEventCount;
				AddIssue(
					OutResult,
					true,
					Selection,
					TEXT("member_filter_conflict"),
					Source);
				SetDelegateEventSelectionFailure(
					OutResult,
					TEXT("member_filter_conflict"),
					Source,
					TEXT("Remove delegate events that appear in both include and exclude filters."));
				OutSelections.Reset();
				return false;
			}
			if (!ResolveOne(
					{ Rule.OwnerClassPath, EventName },
					true,
					SeenSelections,
					OutSelections,
					OutResult))
			{
				OutSelections.Reset();
				OutResult.AcceptedDelegateEventCount = 0;
				return false;
			}
		}
	}

	TArray<FAvidScriptReflectedDelegateEventSelection> ExplicitEvents =
		Profile.ExplicitDelegateEvents;
	ExplicitEvents.Sort([](
		const FAvidScriptReflectedDelegateEventSelection& Left,
		const FAvidScriptReflectedDelegateEventSelection& Right)
	{
		return MakeEventSelectionKey(Left).Compare(
			MakeEventSelectionKey(Right),
			ESearchCase::CaseSensitive) < 0;
	});
	for (const FAvidScriptReflectedDelegateEventSelection& Selection :
		ExplicitEvents)
	{
		if (!ResolveOne(
				Selection,
				Profile.bStrictExplicitDelegateEvents,
				SeenSelections,
				OutSelections,
				OutResult))
		{
			OutSelections.Reset();
			OutResult.AcceptedDelegateEventCount = 0;
			return false;
		}
	}

	OutSelections.Sort([](
		const FAvidScriptReflectedDelegateEventSelection& Left,
		const FAvidScriptReflectedDelegateEventSelection& Right)
	{
		return MakeEventSelectionKey(Left).Compare(
			MakeEventSelectionKey(Right),
			ESearchCase::CaseSensitive) < 0;
	});
	OutResult.bSucceeded = true;
	OutResult.AcceptedDelegateEventCount = OutSelections.Num();
	return true;
}
