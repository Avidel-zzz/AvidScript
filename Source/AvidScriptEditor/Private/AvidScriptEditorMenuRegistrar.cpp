#include "AvidScriptEditorMenuRegistrar.h"

#include "ToolMenu.h"
#include "ToolMenuEntry.h"
#include "ToolMenuOwner.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"

namespace
{
void SetAvidScriptMenuRegistrationFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	FAvidScriptEditorMenuRegistrationResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
}

bool ValidateAvidScriptMenuEntryConfig(
	const FAvidScriptEditorMenuEntryConfig& Config,
	FAvidScriptEditorMenuRegistrationResult& OutResult)
{
	if (Config.MenuName.IsNone())
	{
		SetAvidScriptMenuRegistrationFailure(
			TEXT("menu_name_missing"),
			TEXT("AvidScript menu registration requires a menu name."),
			OutResult);
		return false;
	}

	if (Config.SectionName.IsNone())
	{
		SetAvidScriptMenuRegistrationFailure(
			TEXT("section_name_missing"),
			TEXT("AvidScript menu registration requires a section name."),
			OutResult);
		return false;
	}

	if (Config.EntryName.IsNone())
	{
		SetAvidScriptMenuRegistrationFailure(
			TEXT("entry_name_missing"),
			TEXT("AvidScript menu registration requires an entry name."),
			OutResult);
		return false;
	}

	if (!Config.ExecuteAction.IsBound())
	{
		SetAvidScriptMenuRegistrationFailure(
			TEXT("execute_action_missing"),
			TEXT("AvidScript menu registration requires an execute action."),
			OutResult);
		return false;
	}

	return true;
}
} // namespace

bool FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(
	const FAvidScriptEditorMenuEntryConfig& Config,
	FAvidScriptEditorMenuRegistrationResult& OutResult)
{
	OutResult = FAvidScriptEditorMenuRegistrationResult();
	OutResult.MenuName = Config.MenuName;
	OutResult.SectionName = Config.SectionName;
	OutResult.EntryName = Config.EntryName;

	if (!ValidateAvidScriptMenuEntryConfig(Config, OutResult))
	{
		return false;
	}

	UToolMenus* ToolMenus = UToolMenus::Get();
	if (ToolMenus == nullptr)
	{
		SetAvidScriptMenuRegistrationFailure(
			TEXT("toolmenus_unavailable"),
			TEXT("UToolMenus is not available."),
			OutResult);
		return false;
	}

	TUniquePtr<FToolMenuOwnerScoped> OwnerScope;
	if (!Config.OwnerName.IsNone())
	{
		OwnerScope = MakeUnique<FToolMenuOwnerScoped>(FToolMenuOwner(Config.OwnerName));
	}

	UToolMenu* Menu = ToolMenus->FindMenu(Config.MenuName);
	if (Menu == nullptr)
	{
		Menu = ToolMenus->RegisterMenu(Config.MenuName, NAME_None, EMultiBoxType::Menu, false);
	}

	if (Menu == nullptr)
	{
		SetAvidScriptMenuRegistrationFailure(
			TEXT("menu_registration_failed"),
			FString::Printf(TEXT("AvidScript menu could not be registered: %s"), *Config.MenuName.ToString()),
			OutResult);
		return false;
	}

	FToolMenuSection& Section = Config.SectionLabel.IsEmpty()
		? Menu->FindOrAddSection(Config.SectionName)
		: Menu->FindOrAddSection(Config.SectionName, Config.SectionLabel);
	const FSimpleDelegate ExecuteAction = Config.ExecuteAction;
	const FToolUIActionChoice Action(FExecuteAction::CreateLambda([ExecuteAction]() {
		ExecuteAction.ExecuteIfBound();
	}));
	FToolMenuEntry Entry = FToolMenuEntry::InitMenuEntry(
		Config.EntryName,
		Config.Label,
		Config.ToolTip,
		FSlateIcon(),
		Action);

	Section.AddEntry(Entry);

	OutResult.bSucceeded = true;
	return true;
}
