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
	TestEqual(TEXT("Default schema contains thirteen host intrinsics"), FirstResult.IntrinsicCount, 13);

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
	const TArray<TSharedPtr<FJsonValue>>& Intrinsics = Root->GetArrayField(TEXT("intrinsics"));
	TestEqual(TEXT("Schema serializes thirteen intrinsic objects"), Intrinsics.Num(), 13);
	const auto CountIntrinsic = [&Intrinsics](
		const FString& Module,
		const FString& Name,
		const FString& Signature) -> int32
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& Value : Intrinsics)
		{
			const TSharedPtr<FJsonObject> Intrinsic = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Intrinsic.IsValid()
				&& Intrinsic->GetStringField(TEXT("import_module")) == Module
				&& Intrinsic->GetStringField(TEXT("import_name")) == Name
				&& Intrinsic->GetStringField(TEXT("abi_signature")) == Signature)
			{
				++Count;
			}
		}
		return Count;
	};
	const auto CountText = [&FirstJson](const FString& Token)
	{
		int32 Count = 0;
		int32 SearchFrom = 0;
		while ((SearchFrom = FirstJson.Find(
			Token,
			ESearchCase::CaseSensitive,
			ESearchDir::FromStart,
			SearchFrom)) != INDEX_NONE)
		{
			++Count;
			SearchFrom += Token.Len();
		}
		return Count;
	};
	TestEqual(
		TEXT("Schema contains exactly one packed owner intrinsic"),
		CountIntrinsic(TEXT("avidscript"), TEXT("avid_owner_get_handle"), TEXT("()I")),
		1);
	TestEqual(
		TEXT("Schema retains legacy owner generation intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("owner_get_generation"), TEXT("()i")),
		1);
	TestEqual(
		TEXT("Schema retains legacy owner slot intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("owner_get_slot"), TEXT("()i")),
		1);
	TestEqual(
		TEXT("Schema contains continuation cancellation intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("continuation_cancel"), TEXT("(I)i")),
		1);
	TestEqual(
		TEXT("Schema contains continuation delay intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("continuation_delay"), TEXT("(fi)I")),
		1);
	TestEqual(
		TEXT("Schema contains async object-load intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("continuation_load_object"), TEXT("(ii)I")),
		1);
	TestEqual(
		TEXT("Schema contains bulk continuation result intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("continuation_result_read"), TEXT("(iiiii)i")),
		1);
	TestEqual(
		TEXT("Schema contains cancellation source creation intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("continuation_cancel_source_create"), TEXT("()I")),
		1);
	TestEqual(
		TEXT("Schema contains cancellation source cancel intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("continuation_cancel_source_cancel"), TEXT("(I)i")),
		1);
	TestEqual(
		TEXT("Schema contains cancellation source release intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("continuation_cancel_source_release"), TEXT("(I)i")),
		1);
	TestEqual(
		TEXT("Schema contains cancellation binding intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("continuation_bind_cancel"), TEXT("(II)i")),
		1);
	TestEqual(
		TEXT("Schema retains legacy Timer cancel intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("timer_cancel"), TEXT("(i)i")),
		1);
	TestEqual(
		TEXT("Schema retains legacy set-once Timer intrinsic"),
		CountIntrinsic(TEXT("env"), TEXT("timer_set_once"), TEXT("(fi)i")),
		1);
	TestTrue(
		TEXT("Schema text spells the packed owner import exactly"),
		FirstJson.Contains(TEXT("\"import_module\": \"avidscript\""))
		&& FirstJson.Contains(TEXT("\"import_name\": \"avid_owner_get_handle\""))
		&& FirstJson.Contains(TEXT("\"abi_signature\": \"()I\"")));
	TestEqual(
		TEXT("Schema text contains one packed owner import name"),
		CountText(TEXT("\"avid_owner_get_handle\"")),
		1);
	TestEqual(
		TEXT("Schema text contains packed owner and cancellation source i64 signatures"),
		CountText(TEXT("\"()I\"")),
		2);
	TestTrue(TEXT("Schema includes actor location import"), FirstJson.Contains(TEXT("actor_get_location")));
	TestTrue(TEXT("Schema includes actor root component import"), FirstJson.Contains(TEXT("actor_get_root_component")));
	TestTrue(TEXT("Schema includes SceneComponent world location import"), FirstJson.Contains(TEXT("scene_component_get_world_location")));
	TestTrue(TEXT("Schema includes FVector reflected type"), FirstJson.Contains(TEXT("FVector")));
	TestTrue(TEXT("Schema includes FHitResult reflected wrapper context"), FirstJson.Contains(TEXT("FHitResult")));
	TestTrue(TEXT("Schema includes USceneComponent return type"), FirstJson.Contains(TEXT("USceneComponent")));
	TestTrue(TEXT("Schema includes owner generation intrinsic"), FirstJson.Contains(TEXT("owner_get_generation")) && FirstJson.Contains(TEXT("()i")));
	TestTrue(TEXT("Schema includes owner slot intrinsic"), FirstJson.Contains(TEXT("owner_get_slot")));
	TestTrue(TEXT("Schema includes continuation delay intrinsic"), FirstJson.Contains(TEXT("continuation_delay")) && FirstJson.Contains(TEXT("(fi)I")));
	TestTrue(TEXT("Schema includes continuation cancel intrinsic"), FirstJson.Contains(TEXT("continuation_cancel")) && FirstJson.Contains(TEXT("(I)i")));
	TestTrue(TEXT("Schema includes async object-load intrinsic"), FirstJson.Contains(TEXT("continuation_load_object")) && FirstJson.Contains(TEXT("(ii)I")));
	TestTrue(TEXT("Schema includes bulk continuation result intrinsic"), FirstJson.Contains(TEXT("continuation_result_read")) && FirstJson.Contains(TEXT("(iiiii)i")));
	TestTrue(TEXT("Schema includes cancellation source creation intrinsic"), FirstJson.Contains(TEXT("continuation_cancel_source_create")));
	TestTrue(TEXT("Schema includes cancellation source cancel intrinsic"), FirstJson.Contains(TEXT("continuation_cancel_source_cancel")));
	TestTrue(TEXT("Schema includes cancellation source release intrinsic"), FirstJson.Contains(TEXT("continuation_cancel_source_release")));
	TestTrue(TEXT("Schema includes cancellation binding intrinsic"), FirstJson.Contains(TEXT("continuation_bind_cancel")) && FirstJson.Contains(TEXT("(II)i")));
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
