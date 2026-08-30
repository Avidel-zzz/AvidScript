#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorNetworkTopologyBuildProfileTest,
	"AvidScript.Editor.NetworkTopology.BuildProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorNetworkTopologyBuildProfileTest::RunTest(
	const FString& Parameters)
{
	const FString ProfilePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("AvidScript/Samples/CSharp/NetworkTopology/NetworkTopology.csharp-profile.json")));
	FAvidScriptEditorCSharpProfileLoadResult Profile;
	if (!TestTrue(
			TEXT("Network topology profile loads through schema 10"),
			FAvidScriptEditorCSharpProfileService::LoadProfile(
				ProfilePath,
				Profile)))
	{
		AddError(Profile.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Network topology profile schema"), Profile.SchemaVersion, 10);

	FAvidScriptEditorCSharpBuildResult Build;
	if (!TestTrue(
			TEXT("Network topology profile builds through the production pipeline"),
			FAvidScriptEditorCSharpBuildService::BuildProfile(
				FAvidScriptEditorCSharpProfileService::MakeBuildRequest(Profile),
				Build)))
	{
		AddError(Build.ErrorMessage + TEXT("\n") + Build.Stderr);
		return false;
	}
	TestTrue(TEXT("Network topology WASM artifact is published"), Build.bVmArtifactPublished);
	TestTrue(TEXT("Network topology WASM artifact exists"), FPaths::FileExists(Build.VmArtifactPath));
	TestTrue(TEXT("Network topology manifest exists"), FPaths::FileExists(Build.ManifestPath));
	TestTrue(TEXT("Network topology binding package exists"), FPaths::FileExists(Build.BindingPackagePath));
	return true;
}

#endif
