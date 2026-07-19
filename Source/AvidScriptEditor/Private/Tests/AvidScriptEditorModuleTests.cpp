#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorModule.h"

#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorModuleLoadsSmokeTest,
	"AvidScript.Editor.ModuleLoadsSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleLoadsSmokeTest::RunTest(const FString& Parameters)
{
	const FName ModuleName(TEXT("AvidScriptEditor"));
	IModuleInterface& Module = FModuleManager::LoadModuleChecked<IModuleInterface>(ModuleName);
	FAvidScriptEditorModule& AvidScriptEditorModule =
		FModuleManager::GetModuleChecked<FAvidScriptEditorModule>(ModuleName);

	TestTrue(TEXT("AvidScriptEditor module is loaded"), FModuleManager::Get().IsModuleLoaded(ModuleName));
	TestTrue(TEXT("AvidScriptEditor module interface is valid"), &Module != nullptr);
	TestFalse(
		TEXT("Project C# Auto Live Reload is opt-in at module startup"),
		AvidScriptEditorModule.IsCSharpWorkspaceLiveReloadRunning());

	return true;
}

#endif
