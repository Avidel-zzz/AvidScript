#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorModule.h"

#include "AvidScriptEditorCommandLauncher.h"
#include "AvidScriptEditorMenuRegistrar.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorModuleSampleCommandConfigTest,
	"AvidScript.Editor.Module.SampleCommandConfigSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleSampleCommandConfigTest::RunTest(const FString& Parameters)
{
	const FString SourcePath = FAvidScriptEditorModule::GetSampleCommandSourcePath();
	TestTrue(TEXT("Sample command points to actor_set_location.avid"), SourcePath.EndsWith(TEXT("Plugins/AvidScript/Samples/AvidScript/ActorSetLocation/actor_set_location.avid")));
	TestTrue(TEXT("Sample command source exists"), FPaths::FileExists(SourcePath));

	FAvidScriptEditorCommandLaunchConfig CommandConfig;
	FString ErrorMessage;
	TestTrue(TEXT("Sample command default config can be built"), FAvidScriptEditorModule::MakeSampleCommandConfig(CommandConfig, ErrorMessage));
	TestEqual(TEXT("Sample command uses sample source"), CommandConfig.SourcePath, SourcePath);
	TestTrue(TEXT("Sample command uses default ActorHost bindings"), CommandConfig.BindingsPath.EndsWith(TEXT("Bindings/ActorHostBindings.avidscript.json")));
	TestTrue(TEXT("Sample command output root uses source name"), CommandConfig.OutputRoot.EndsWith(TEXT("Saved/AvidScriptGenerated/actor_set_location")));
	TestTrue(TEXT("Sample command report path uses source name"), CommandConfig.ReportPath.EndsWith(TEXT("Saved/AvidScriptReports/actor_set_location.frontend.report.json")));
	TestFalse(TEXT("Sample command compiles by default"), CommandConfig.bSkipCompile);
	TestTrue(TEXT("Sample command config has no error on success"), ErrorMessage.IsEmpty());

	FAvidScriptEditorMenuEntryConfig MenuConfig = FAvidScriptEditorModule::MakeSampleMenuEntryConfig(FSimpleDelegate::CreateLambda([]() {
	}));
	TestEqual(TEXT("Sample command owner"), MenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("Sample command menu"), MenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("Sample command section"), MenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("Sample command entry"), MenuConfig.EntryName, FName(TEXT("AvidScript.RunSampleCommand")));
	TestFalse(TEXT("Sample command label is set"), MenuConfig.Label.IsEmpty());
	TestFalse(TEXT("Sample command tooltip is set"), MenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("Sample command execute action is bound"), MenuConfig.ExecuteAction.IsBound());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
