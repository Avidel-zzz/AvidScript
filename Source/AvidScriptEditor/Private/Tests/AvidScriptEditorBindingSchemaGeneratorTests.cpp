#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingSchemaGenerator.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingSchemaDefaultReflectionSmokeTest,
	"AvidScript.Editor.BindingSchema.DefaultReflectionSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingSchemaDefaultReflectionSmokeTest::RunTest(const FString& Parameters)
{
	FString FirstJson;
	FAvidScriptBindingSchemaGenerateResult FirstResult;
	TestTrue(TEXT("Default reflection schema generates"), FAvidScriptEditorBindingSchemaGenerator::GenerateDefault(FirstJson, FirstResult));
	TestTrue(TEXT("Generation result succeeds"), FirstResult.bSucceeded);
	TestEqual(TEXT("Default schema contains ten reflected bindings"), FirstResult.BindingCount, 10);
	TestEqual(TEXT("Default schema contains four host intrinsics"), FirstResult.IntrinsicCount, 4);

	FString SecondJson;
	FAvidScriptBindingSchemaGenerateResult SecondResult;
	TestTrue(TEXT("Repeated schema generation succeeds"), FAvidScriptEditorBindingSchemaGenerator::GenerateDefault(SecondJson, SecondResult));
	TestEqual(TEXT("Reflection schema output is deterministic"), SecondJson, FirstJson);

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FirstJson);
	TestTrue(TEXT("Generated schema is valid JSON"), FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid());
	if (!Root.IsValid())
	{
		return true;
	}

	TestEqual(TEXT("Schema version is one"), Root->GetIntegerField(TEXT("schema_version")), 1);
	TestEqual(TEXT("Schema source is UE reflection"), Root->GetStringField(TEXT("source")), FString(TEXT("ue_reflection")));
	TestTrue(TEXT("Schema includes actor location import"), FirstJson.Contains(TEXT("actor_get_location")));
	TestTrue(TEXT("Schema includes actor root component import"), FirstJson.Contains(TEXT("actor_get_root_component")));
	TestTrue(TEXT("Schema includes SceneComponent world location import"), FirstJson.Contains(TEXT("scene_component_get_world_location")));
	TestTrue(TEXT("Schema includes FVector reflected type"), FirstJson.Contains(TEXT("FVector")));
	TestTrue(TEXT("Schema includes FHitResult reflected wrapper context"), FirstJson.Contains(TEXT("FHitResult")));
	TestTrue(TEXT("Schema includes USceneComponent return type"), FirstJson.Contains(TEXT("USceneComponent")));
	TestTrue(TEXT("Schema includes owner slot intrinsic"), FirstJson.Contains(TEXT("owner_get_slot")));
	TestTrue(TEXT("Schema includes set-once Timer intrinsic"), FirstJson.Contains(TEXT("timer_set_once")) && FirstJson.Contains(TEXT("(fi)i")));
	TestTrue(TEXT("Schema includes Timer cancel intrinsic"), FirstJson.Contains(TEXT("timer_cancel")) && FirstJson.Contains(TEXT("(i)i")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingSchemaFailureAndWriteSmokeTest,
	"AvidScript.Editor.BindingSchema.FailureAndWriteSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingSchemaFailureAndWriteSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptReflectedBindingSpec InvalidSpec;
	InvalidSpec.OwnerClassPath = TEXT("/Script/Engine.Actor");
	InvalidSpec.FunctionName = TEXT("AvidScriptMissingFunction");
	InvalidSpec.ImportModule = TEXT("env");
	InvalidSpec.ImportName = TEXT("invalid_test_import");
	InvalidSpec.AbiSignature = TEXT("()i");
	InvalidSpec.Projection = TEXT("test");

	FString InvalidJson;
	FAvidScriptBindingSchemaGenerateResult InvalidResult;
	TestFalse(TEXT("Missing reflected function fails closed"), FAvidScriptEditorBindingSchemaGenerator::Generate({ InvalidSpec }, InvalidJson, InvalidResult));
	TestEqual(TEXT("Missing function reports category"), InvalidResult.ErrorCategory, FString(TEXT("function_missing")));
	TestTrue(TEXT("Failed generation does not return partial JSON"), InvalidJson.IsEmpty());

	TArray<FAvidScriptReflectedBindingSpec> DuplicateSpecs = FAvidScriptEditorBindingSchemaGenerator::MakeDefaultSpecs();
	const FAvidScriptReflectedBindingSpec DuplicateSpec = DuplicateSpecs[0];
	DuplicateSpecs.Add(DuplicateSpec);
	FAvidScriptBindingSchemaGenerateResult DuplicateResult;
	TestFalse(TEXT("Duplicate imports fail closed"), FAvidScriptEditorBindingSchemaGenerator::Generate(DuplicateSpecs, InvalidJson, DuplicateResult));
	TestEqual(TEXT("Duplicate import reports category"), DuplicateResult.ErrorCategory, FString(TEXT("duplicate_import")));

	const FString OutputPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("AvidScriptTests"), TEXT("Phase31"), TEXT("bindings.generated.json")));
	FAvidScriptBindingSchemaGenerateResult WriteResult;
	TestTrue(TEXT("Default schema writes to Saved"), FAvidScriptEditorBindingSchemaGenerator::WriteDefault(OutputPath, WriteResult));
	FString WrittenJson;
	TestTrue(TEXT("Written schema can be read"), FFileHelper::LoadFileToString(WrittenJson, *OutputPath));
	TestTrue(TEXT("Written schema contains reflected marker"), WrittenJson.Contains(TEXT("ue_reflection")));
	IFileManager::Get().Delete(*OutputPath);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingSchemaManifestContractSmokeTest,
	"AvidScript.Editor.BindingSchema.ManifestContractSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingSchemaManifestContractSmokeTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ActorLifecycle"),
		TEXT("actor_lifecycle.avidscript.json")));
	FAvidScriptBindingSchemaGenerateResult ValidResult;
	TestTrue(
		TEXT("Current C# guest imports are covered by reflected schema"),
		FAvidScriptEditorBindingSchemaGenerator::ValidateManifestImports(ManifestPath, ValidResult));
	TestTrue(TEXT("Manifest contract result succeeds"), ValidResult.bSucceeded);

	const FString InvalidManifestPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("AvidScriptTests"), TEXT("Phase31"), TEXT("invalid_imports.avidscript.json")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(InvalidManifestPath), true);
	const FString InvalidManifest = TEXT("{\"required_imports\":[{\"module\":\"env\",\"name\":\"missing_binding\"}]}");
	TestTrue(TEXT("Invalid manifest fixture writes"), FFileHelper::SaveStringToFile(InvalidManifest, *InvalidManifestPath));

	FAvidScriptBindingSchemaGenerateResult InvalidResult;
	TestFalse(
		TEXT("Unknown guest import fails closed"),
		FAvidScriptEditorBindingSchemaGenerator::ValidateManifestImports(InvalidManifestPath, InvalidResult));
	TestEqual(TEXT("Unknown import reports contract mismatch"), InvalidResult.ErrorCategory, FString(TEXT("binding_contract_mismatch")));
	TestTrue(TEXT("Unknown import identifies its source"), InvalidResult.ErrorSource.Contains(TEXT("env.missing_binding")));

	const FString MalformedManifest = TEXT(R"json({"required_imports":[7]})json");
	TestTrue(TEXT("Malformed manifest fixture writes"), FFileHelper::SaveStringToFile(MalformedManifest, *InvalidManifestPath));
	FAvidScriptBindingSchemaGenerateResult MalformedResult;
	TestFalse(
		TEXT("Non-object guest import fails closed"),
		FAvidScriptEditorBindingSchemaGenerator::ValidateManifestImports(InvalidManifestPath, MalformedResult));
	TestEqual(TEXT("Non-object import reports invalid manifest"), MalformedResult.ErrorCategory, FString(TEXT("manifest_invalid")));
	IFileManager::Get().Delete(*InvalidManifestPath);
	return true;
}

#endif
