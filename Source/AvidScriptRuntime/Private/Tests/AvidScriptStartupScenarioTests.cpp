#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Startup/AvidScriptStartupScenario.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptStartupScenarioSchemaTest,
	"AvidScript.Runtime.StartupScenario.Schema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptStartupScenarioSchemaTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString ValidJson = TEXT(R"JSON({
  "schema_version": 1,
  "scenarios": [
    {
      "scenario_id": "pickup_rush",
      "activation": "explicit",
      "worlds": ["/Game/Variant_TwinStick/LVL_TwinStick"],
      "bindings": [
        {
          "module_id": "pickup_rush",
          "target": { "mode": "world_host" }
        },
        {
          "module_id": "pickup_actor",
          "target": {
            "mode": "existing_actor",
            "class_path": "/Script/Engine.StaticMeshActor",
            "required_tag": "pickup",
            "max_instances": 8
          }
        },
        {
          "module_id": "spawned_pickup",
          "target": {
            "mode": "spawn_actor",
            "class_path": "/Script/Engine.StaticMeshActor",
            "transforms": [
              {
                "location": [100.0, 200.0, 300.0],
                "rotation": [0.0, 90.0, 0.0],
                "scale": [1.0, 1.0, 1.0]
              }
            ]
          }
        }
      ]
    }
  ]
})JSON");

	FAvidScriptStartupDocument Document;
	FAvidScriptStartupLoadResult Result;
	if (!TestTrue(
		TEXT("Valid startup scenario parses"),
		AvidScript::Startup::ParseDocument(ValidJson, Document, Result)))
	{
		AddError(Result.ErrorMessage);
		return true;
	}
	TestEqual(TEXT("Schema version is retained"), Document.SchemaVersion, 1);
	TestEqual(TEXT("One scenario is retained"), Document.Scenarios.Num(), 1);
	const FAvidScriptStartupScenario* Scenario =
		AvidScript::Startup::FindScenario(Document, TEXT("pickup_rush"));
	if (!TestNotNull(TEXT("Scenario lookup succeeds"), Scenario))
	{
		return true;
	}
	TestEqual(TEXT("All target modes are retained"), Scenario->Bindings.Num(), 3);
	TestTrue(
		TEXT("World whitelist matches exact package"),
		AvidScript::Startup::IsWorldAllowed(
			*Scenario,
			TEXT("/Game/Variant_TwinStick/LVL_TwinStick")));
	TestFalse(
		TEXT("World whitelist rejects a different package"),
		AvidScript::Startup::IsWorldAllowed(*Scenario, TEXT("/Game/TopDown/Lvl_TopDown")));
	TestEqual(
		TEXT("Existing target limit is retained"),
		Scenario->Bindings[1].Target.MaxInstances,
		8);
	TestEqual(
		TEXT("Spawn transform is retained"),
		Scenario->Bindings[2].Target.SpawnTransforms[0].GetLocation(),
		FVector(100.0, 200.0, 300.0));

	auto ExpectRejected = [this](
		const TCHAR* Label,
		const FString& Json,
		const TCHAR* Category)
	{
		FAvidScriptStartupDocument RejectedDocument;
		FAvidScriptStartupLoadResult RejectedResult;
		TestFalse(
			Label,
			AvidScript::Startup::ParseDocument(
				Json,
				RejectedDocument,
				RejectedResult));
		TestEqual(
			FString::Printf(TEXT("%s category"), Label),
			RejectedResult.ErrorCategory,
			FString(Category));
	};

	ExpectRejected(
		TEXT("Duplicate JSON key fails closed"),
		TEXT(R"JSON({"schema_version":1,"schema_version":1,"scenarios":[]})JSON"),
		TEXT("json_duplicate_key"));
	ExpectRejected(
		TEXT("Unknown root field fails closed"),
		TEXT(R"JSON({"schema_version":1,"scenarios":[],"command":"run"})JSON"),
		TEXT("document_invalid"));
	ExpectRejected(
		TEXT("Empty scenario array fails closed"),
		TEXT(R"JSON({"schema_version":1,"scenarios":[]})JSON"),
		TEXT("document_invalid"));
	ExpectRejected(
		TEXT("Implicit activation fails closed"),
		ValidJson.Replace(TEXT("\"explicit\""), TEXT("\"automatic\"")),
		TEXT("scenario_invalid"));
	ExpectRejected(
		TEXT("Invalid world package fails closed"),
		ValidJson.Replace(
			TEXT("/Game/Variant_TwinStick/LVL_TwinStick"),
			TEXT("C:/Private/Level")),
		TEXT("world_filter_invalid"));
	ExpectRejected(
		TEXT("Unknown target mode fails closed"),
		ValidJson.Replace(TEXT("\"world_host\""), TEXT("\"shell\"")),
		TEXT("target_mode_unsupported"));
	ExpectRejected(
		TEXT("Zero existing target limit fails closed"),
		ValidJson.Replace(TEXT("\"max_instances\": 8"), TEXT("\"max_instances\": 0")),
		TEXT("target_invalid"));
	ExpectRejected(
		TEXT("Zero spawn scale fails closed"),
		ValidJson.Replace(TEXT("\"scale\": [1.0, 1.0, 1.0]"), TEXT("\"scale\": [0.0, 1.0, 1.0]")),
		TEXT("target_invalid"));
	return true;
}

#endif

