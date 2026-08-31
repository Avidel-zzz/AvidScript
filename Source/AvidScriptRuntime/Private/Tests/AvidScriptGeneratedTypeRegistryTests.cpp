#if WITH_DEV_AUTOMATION_TESTS

#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"

#include "AvidScriptComponent.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

namespace
{
FString BuildValidGeneratedTypeManifest()
{
	return FString::Printf(
		TEXT(R"JSON({
  "schema_version": 5,
  "generator_version": "1.6",
  "module_name": "AvidScriptRuntime",
  "types": [
    {
      "type_ordinal": 0,
      "stable_type_id": "type:registry-fixture",
      "engine_name": "AvidScriptComponent",
      "class_path": "%s",
      "properties": [
        {
          "member_ordinal": 0,
          "stable_member_id": "property:script-manifest-file",
          "name": "ScriptManifestFile",
          "getter_import_name": "avid_ue_property_0_0_get",
          "setter_import_name": "avid_ue_property_0_0_set"
        }
      ],
      "functions": [
        {
          "member_ordinal": 1,
          "stable_member_id": "function:reload-script",
          "native_name": "ReloadScript",
          "export_name": "avid_ue_0123456789abcdef0123456789abcdef",
          "flags": []
        }
      ]
    }
  ]
})JSON"),
		*UAvidScriptComponent::StaticClass()->GetPathName());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedTypeRegistryTest,
	"AvidScript.Runtime.GeneratedTypes.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedTypeRegistryTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FString Error;
	const FString ValidManifest = BuildValidGeneratedTypeManifest();
	if (!TestTrue(
		TEXT("Schema 5 manifest builds an immutable generated type registry"),
		FAvidScriptGeneratedTypeRegistry::BuildFromJson(ValidManifest, Registry, Error)))
	{
		AddError(Error);
		return true;
	}

	TestTrue(TEXT("Registry snapshot is published"), Registry.IsValid());
	TestEqual(TEXT("Registry retains one generated class"), Registry->Num(), 1);
	const FAvidScriptGeneratedTypePlan* const Type = Registry->FindTypeByOrdinal(0);
	TestNotNull(TEXT("Dense ordinal resolves the type plan"), Type);
	if (Type != nullptr)
	{
		TestEqual(TEXT("Type plan caches the reflected UClass"), Type->Class, UAvidScriptComponent::StaticClass());
		TestTrue(
			TEXT("Stable type id resolves the same immutable plan"),
			Registry->FindTypeByStableId(Type->StableTypeId) == Type);
		TestTrue(
			TEXT("UClass resolves the same immutable plan"),
			Registry->FindTypeByClass(Type->Class) == Type);
		const FAvidScriptGeneratedMemberPlan* const Property = Type->FindMember(0);
		const FAvidScriptGeneratedMemberPlan* const Function = Type->FindMember(1);
		TestNotNull(TEXT("Property ordinal is prepared"), Property);
		TestNotNull(TEXT("Function ordinal is prepared"), Function);
		if (Property != nullptr)
		{
			TestEqual(TEXT("Property plan caches FProperty"), Property->Property,
				FindFProperty<FProperty>(UAvidScriptComponent::StaticClass(), TEXT("ScriptManifestFile")));
		}
		if (Function != nullptr)
		{
			TestEqual(TEXT("Function plan caches UFunction"), Function->Function,
				UAvidScriptComponent::StaticClass()->FindFunctionByName(TEXT("ReloadScript")));
		}
	}

	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> RejectedRegistry;
	FString RejectedError;
	TestFalse(
		TEXT("Duplicate member ordinals fail closed"),
		FAvidScriptGeneratedTypeRegistry::BuildFromJson(
			ValidManifest.Replace(TEXT("\"member_ordinal\": 1"), TEXT("\"member_ordinal\": 0")),
			RejectedRegistry,
			RejectedError));
	TestFalse(TEXT("Rejected manifest publishes no partial registry"), RejectedRegistry.IsValid());
	TestFalse(
		TEXT("Class paths are explicit and cannot be guessed"),
		FAvidScriptGeneratedTypeRegistry::BuildFromJson(
			ValidManifest.Replace(
				TEXT("/Script/AvidScriptRuntime.AvidScriptComponent"),
				TEXT("/Script/AvidScriptRuntime.MissingComponent")),
			RejectedRegistry,
			RejectedError));
	TestFalse(
		TEXT("Old manifest schemas fail closed"),
		FAvidScriptGeneratedTypeRegistry::BuildFromJson(
			ValidManifest.Replace(TEXT("\"schema_version\": 5"), TEXT("\"schema_version\": 4")),
			RejectedRegistry,
			RejectedError));
	return true;
}

#endif
