#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorMenuRegistrar.h"

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "ToolMenus.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorMenuRegistrarRegistersEntryTest,
	"AvidScript.Editor.MenuRegistrar.RegistersMenuEntrySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorMenuRegistrarRegistersEntryTest::RunTest(const FString& Parameters)
{

	bool bExecuted = false;

	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = TEXT("AvidScriptEditorMenuRegistrarTestOwner");
	Config.MenuName = FName(*FString::Printf(
		TEXT("AvidScriptEditor.MenuRegistrar.TestMenu.%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Config.SectionName = TEXT("AvidScriptSection");
	Config.EntryName = TEXT("RunSample");
	Config.SectionLabel = FText::FromString(TEXT("AvidScript"));
	Config.Label = FText::FromString(TEXT("Run Sample"));
	Config.ToolTip = FText::FromString(TEXT("Run an AvidScript sample command."));
	Config.ExecuteAction = FSimpleDelegate::CreateLambda([&bExecuted]() {
		bExecuted = true;
	});

	FAvidScriptEditorMenuRegistrationResult Result;
	TestTrue(TEXT("Registrar accepts complete menu entry config"), FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(Config, Result));
	TestTrue(TEXT("Registration result succeeds"), Result.bSucceeded);
	TestTrue(TEXT("No registration error category"), Result.ErrorCategory.IsEmpty());
	TestTrue(TEXT("No registration error message"), Result.ErrorMessage.IsEmpty());

	UToolMenu* Menu = UToolMenus::Get()->FindMenu(Config.MenuName);
	TestNotNull(TEXT("Test menu is registered"), Menu);
	if (Menu == nullptr)
	{
		return true;
	}

	TestTrue(TEXT("Section is registered"), Menu->ContainsSection(Config.SectionName));
	TestTrue(TEXT("Entry is registered"), Menu->ContainsEntry(Config.EntryName));

	Config.ExecuteAction.ExecuteIfBound();
	TestTrue(TEXT("Registered delegate is executable"), bExecuted);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
