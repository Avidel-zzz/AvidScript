#include "AvidScriptEditorBindingDelegateEventSelectionResolver.h"

#include "AvidScriptBindingNetworkPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedDelegateEventPolicy.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
FString MakeEventSelectionKey(
	const FAvidScriptReflectedDelegateEventSelection& Selection)
{
	return Selection.CallbackKind + TEXT(":")
		+ Selection.OwnerClassPath + TEXT(".")
		+ Selection.EventName.ToString();
}

bool TryResolveFunctionHandler(
	const FAvidScriptReflectedDelegateEventSelection& Selection,
	UClass& OwnerClass,
	FAvidScriptReflectedDelegateEventSelection& OutSelection,
	FString& OutCategory,
	FString& OutSource)
{
	UFunction* const Function = OwnerClass.FindFunctionByName(
		Selection.EventName,
		EIncludeSuperFlag::ExcludeSuper);
	if (Function == nullptr)
	{
		OutCategory = TEXT("function_handler_missing");
		OutSource = MakeEventSelectionKey(Selection);
		return false;
	}
	if ((!Function->HasAnyFunctionFlags(FUNC_Native)
			&& Function->Script.IsEmpty())
		|| Function->HasAnyFunctionFlags(FUNC_Static | FUNC_Delegate
			| FUNC_MulticastDelegate)
		|| Function->HasMetaData(TEXT("Latent"))
		|| !IsAvidScriptBindingNetworkOwnerClass(&OwnerClass)
		|| (Selection.HandlerMode != TEXT("replace")
			&& Selection.HandlerMode != TEXT("before")
			&& Selection.HandlerMode != TEXT("after")))
	{
		OutCategory = TEXT("function_handler_owner_or_flags_unsupported");
		OutSource = Function->GetPathName();
		return false;
	}

	FAvidScriptBindingNetworkContract Network;
	if (!TryResolveAvidScriptBindingNetworkContract(*Function, Network))
	{
		OutCategory = TEXT("function_handler_network_contract_invalid");
		OutSource = Function->GetPathName();
		return false;
	}
	FProperty* RepNotifyProperty = nullptr;
	for (TFieldIterator<FProperty> It(
		&OwnerClass,
		EFieldIterationFlags::IncludeSuper); It; ++It)
	{
		FProperty* const Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_RepNotify)
			&& Property->RepNotifyFunc == Function->GetFName())
		{
			if (RepNotifyProperty != nullptr)
			{
				OutCategory = TEXT("function_handler_rep_notify_ambiguous");
				OutSource = Function->GetPathName();
				return false;
			}
			RepNotifyProperty = Property;
		}
	}
	if (Network.IsNetworked() == (RepNotifyProperty != nullptr))
	{
		const bool bBlueprintEvent = !Network.IsNetworked()
			&& RepNotifyProperty == nullptr
			&& OwnerClass.ClassGeneratedBy != nullptr
			&& Function->GetOwnerClass() == &OwnerClass
			&& !Function->HasAnyFunctionFlags(FUNC_Native)
			&& !Function->Script.IsEmpty();
		if (!bBlueprintEvent)
		{
			OutCategory = Network.IsNetworked()
				? FString(TEXT("function_handler_kind_ambiguous"))
				: FString(TEXT("function_handler_kind_unsupported"));
			OutSource = Function->GetPathName();
			return false;
		}
	}

	FAvidScriptProjectedDelegateEvent Projection;
	if (!FAvidScriptEditorReflectedDelegateEventPolicy::
		EvaluateSignatureAndProject(
			Function,
			Projection,
			OutCategory,
			OutSource))
	{
		return false;
	}
	const bool bBlueprintEvent = !Network.IsNetworked()
		&& RepNotifyProperty == nullptr;
	if (bBlueprintEvent
		&& (Projection.ReturnValue.Type.Kind != TEXT("void")
			|| Projection.Parameters.ContainsByPredicate(
				[](const FAvidScriptProjectedBindingValue& Parameter)
				{
					return Parameter.Direction == TEXT("ref")
						|| Parameter.Direction == TEXT("out");
				})))
	{
		OutCategory = TEXT("blueprint_event_signature_unsupported");
		OutSource = Function->GetPathName();
		return false;
	}
	OutSelection = Selection;
	FString ResolvedKind(TEXT("blueprint_event"));
	if (Network.IsNetworked())
	{
		ResolvedKind = TEXT("network_rpc");
	}
	else if (RepNotifyProperty != nullptr)
	{
		ResolvedKind = TEXT("rep_notify");
	}
	if (Selection.CallbackKind != TEXT("function_handler")
		&& Selection.CallbackKind != ResolvedKind)
	{
		OutCategory = TEXT("function_handler_kind_mismatch");
		OutSource = Function->GetPathName();
		return false;
	}
	OutSelection.CallbackKind = ResolvedKind;
	return true;
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
	Issue.MemberKind = Selection.CallbackKind == TEXT("multicast")
		|| Selection.CallbackKind == TEXT("singlecast")
		? FString(TEXT("delegate_event"))
		: FString(TEXT("function_handler"));
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
	FString Category;
	FString Source;
	if (OwnerClass == nullptr)
	{
		Category = TEXT("class_missing");
		Source = Selection.OwnerClassPath;
	}
	else if (Selection.CallbackKind == TEXT("function_handler")
		|| Selection.CallbackKind == TEXT("network_rpc")
		|| Selection.CallbackKind == TEXT("rep_notify")
		|| Selection.CallbackKind == TEXT("blueprint_event"))
	{
		FAvidScriptReflectedDelegateEventSelection ResolvedSelection;
		if (TryResolveFunctionHandler(
				Selection,
				*OwnerClass,
				ResolvedSelection,
				Category,
				Source))
		{
			const FString ResolvedKey = MakeEventSelectionKey(ResolvedSelection);
			if (SeenSelections.Contains(ResolvedKey))
			{
				Category = TEXT("duplicate_function_handler_selection");
				Source = ResolvedKey;
			}
			else
			{
				SeenSelections.Add(ResolvedKey);
				OutSelections.Add(MoveTemp(ResolvedSelection));
				return true;
			}
		}
	}
	else if (Selection.CallbackKind == TEXT("multicast")
		|| Selection.CallbackKind == TEXT("singlecast"))
	{
		FProperty* const Property = FindFProperty<FProperty>(
			OwnerClass,
			Selection.EventName);
		if (Property == nullptr)
		{
			Category = TEXT("delegate_event_missing");
			Source = Key;
		}
		else if (Property->GetOwnerStruct() != OwnerClass)
		{
			Category = TEXT("delegate_event_owner_mismatch");
			Source = Key;
		}
		else
		{
			const bool bPropertyKindMatches =
				(Selection.CallbackKind == TEXT("multicast")
					&& Property->IsA<FMulticastDelegateProperty>())
				|| (Selection.CallbackKind == TEXT("singlecast")
					&& Property->IsA<FDelegateProperty>());
			FAvidScriptProjectedDelegateEvent Projection;
			if (!bPropertyKindMatches)
			{
				Category = TEXT("delegate_event_kind_mismatch");
				Source = Key;
			}
			else if (FAvidScriptEditorReflectedDelegateEventPolicy::EvaluateAndProject(
					Property,
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
	}
	else
	{
		Category = TEXT("delegate_event_kind_unsupported");
		Source = Key;
	}

	AddIssue(OutResult, bFatal, Selection, Category, Source);
	if (bFatal)
	{
		SetDelegateEventSelectionFailure(
			OutResult,
			Category,
			Source,
			Selection.CallbackKind == TEXT("function_handler")
				|| Selection.CallbackKind == TEXT("network_rpc")
				|| Selection.CallbackKind == TEXT("rep_notify")
				|| Selection.CallbackKind == TEXT("blueprint_event")
				? FString(TEXT("Select a declared native or Blueprint-bytecode Actor/ActorComponent RPC, RepNotify, or void Blueprint event UFunction with a supported value-only signature and explicit chain mode."))
				: FString(TEXT("Select a declared dynamic singlecast or multicast delegate with a supported return/value/const-ref/ref/out signature within the eight-cell ABI limit.")));
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
		for (const FName HandlerName : Rule.IncludeHandlers)
		{
			if (Rule.BeforeHandlers.Contains(HandlerName)
				|| Rule.AfterHandlers.Contains(HandlerName))
			{
				SetDelegateEventSelectionFailure(
					OutResult,
					TEXT("handler_mode_conflict"),
					Rule.OwnerClassPath + TEXT(".")
						+ HandlerName.ToString(),
					TEXT("Keep each handler in exactly one chain-mode group."));
				return false;
			}
		}
		for (const FName HandlerName : Rule.BeforeHandlers)
		{
			if (Rule.AfterHandlers.Contains(HandlerName))
			{
				SetDelegateEventSelectionFailure(
					OutResult,
					TEXT("handler_mode_conflict"),
					Rule.OwnerClassPath + TEXT(".")
						+ HandlerName.ToString(),
					TEXT("Keep each handler in exactly one chain-mode group."));
				return false;
			}
		}
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
		const auto ResolveHandlerGroup = [&Rule, &SeenSelections,
			&OutSelections, &OutResult](
			TArray<FName> HandlerNames,
			const FString& HandlerMode)
		{
			HandlerNames.Sort([](const FName Left, const FName Right)
			{
				return Left.ToString().Compare(
					Right.ToString(),
					ESearchCase::CaseSensitive) < 0;
			});
			for (const FName HandlerName : HandlerNames)
			{
				if (!ResolveOne(
						{
							Rule.OwnerClassPath,
							HandlerName,
							TEXT("function_handler"),
							HandlerMode
						},
						true,
						SeenSelections,
						OutSelections,
						OutResult))
				{
					return false;
				}
			}
			return true;
		};
		if (!ResolveHandlerGroup(Rule.IncludeHandlers, TEXT("replace"))
			|| !ResolveHandlerGroup(Rule.BeforeHandlers, TEXT("before"))
			|| !ResolveHandlerGroup(Rule.AfterHandlers, TEXT("after")))
		{
			OutSelections.Reset();
			OutResult.AcceptedDelegateEventCount = 0;
			return false;
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
