#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingDescriptorGenerator.h"

#include "AvidScriptBindingDescriptor.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
bool ParseDescriptor(const FString& Json, TSharedPtr<FJsonObject>& OutRoot)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, OutRoot) && OutRoot.IsValid();
}

bool SerializeDescriptor(const TSharedPtr<FJsonObject>& Root, FString& OutJson)
{
	OutJson.Empty();
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	return Root.IsValid() && FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
}

TSharedPtr<FJsonObject> FindBinding(
	const TArray<TSharedPtr<FJsonValue>>& Bindings,
	const FString& OwnerClass,
	const FString& FunctionName)
{
	for (const TSharedPtr<FJsonValue>& Value : Bindings)
	{
		const TSharedPtr<FJsonObject> Binding = Value.IsValid() ? Value->AsObject() : nullptr;
		if (Binding.IsValid()
			&& Binding->GetStringField(TEXT("owner_class")) == OwnerClass
			&& Binding->GetStringField(TEXT("ue_function")) == FunctionName)
		{
			return Binding;
		}
	}
	return nullptr;
}

bool IsLowerHexSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV3DeterminismTest,
	"AvidScript.Editor.BindingDescriptor.V3Determinism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV3DeterminismTest::RunTest(const FString& Parameters)
{
	FString FirstJson;
	FAvidScriptBindingDescriptorGenerateResult FirstResult;
	TestTrue(
		TEXT("Default binding descriptor v3 generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(FirstJson, FirstResult));
	TestTrue(TEXT("Default result succeeds"), FirstResult.bSucceeded);
	TestEqual(TEXT("Default v3 selection contains eight safe functions"), FirstResult.BindingCount, 8);
	TestTrue(TEXT("Default descriptor contains projected types"), FirstResult.TypeCount >= 5);
	TestTrue(TEXT("Package hash is a complete SHA-256"), IsLowerHexSha256(FirstResult.PackageHash));
	TestTrue(TEXT("Selection hash is a complete SHA-256"), IsLowerHexSha256(FirstResult.SelectionHash));

	FString SecondJson;
	FAvidScriptBindingDescriptorGenerateResult SecondResult;
	TestTrue(
		TEXT("Repeated binding descriptor v3 generation succeeds"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(SecondJson, SecondResult));
	TestEqual(TEXT("Descriptor bytes are deterministic"), SecondJson, FirstJson);
	TestEqual(TEXT("Repeated package hash is deterministic"), SecondResult.PackageHash, FirstResult.PackageHash);

	TSharedPtr<FJsonObject> Root;
	TestTrue(TEXT("Descriptor v3 is valid JSON"), ParseDescriptor(FirstJson, Root));
	if (!Root.IsValid())
	{
		return true;
	}

	TestEqual(TEXT("Descriptor schema is v3"), Root->GetIntegerField(TEXT("schema_version")), 3);
	TestEqual(TEXT("Descriptor source is UE reflection"), Root->GetStringField(TEXT("source")), FString(TEXT("ue_reflection")));
	TestEqual(
		TEXT("Default package name is stable"),
		Root->GetStringField(TEXT("package_name")),
		FString(TEXT("avidscript.engine.core")));
	TestEqual(TEXT("JSON package hash matches result"), Root->GetStringField(TEXT("package_hash")), FirstResult.PackageHash);
	TestEqual(TEXT("JSON selection hash matches result"), Root->GetStringField(TEXT("selection_hash")), FirstResult.SelectionHash);
	TestFalse(TEXT("Descriptor v3 does not expose handwritten projection fields"), FirstJson.Contains(TEXT("\"projection\"")));

	const TArray<TSharedPtr<FJsonValue>>& Bindings = Root->GetArrayField(TEXT("bindings"));
	TestEqual(TEXT("Descriptor serializes eight bindings"), Bindings.Num(), 8);
	FString PreviousIdentity;
	for (int32 Index = 0; Index < Bindings.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Binding = Bindings[Index]->AsObject();
		TestNotNull(TEXT("Binding entry is an object"), Binding.Get());
		if (!Binding.IsValid())
		{
			continue;
		}

		const FString Identity = Binding->GetStringField(TEXT("canonical_identity"));
		TestTrue(TEXT("Binding identities use stable sort order"), PreviousIdentity.IsEmpty() || PreviousIdentity < Identity);
		PreviousIdentity = Identity;
		TestTrue(TEXT("Binding stable id is SHA-256"), IsLowerHexSha256(Binding->GetStringField(TEXT("stable_id"))));
		TestEqual(TEXT("Binding ordinal matches stable array position"), Binding->GetIntegerField(TEXT("ordinal")), Index);
		TestEqual(
			TEXT("Generated host imports use the AvidScript binding module"),
			Binding->GetObjectField(TEXT("host_import"))->GetStringField(TEXT("module")),
			FString(TEXT("avidscript")));
		TestTrue(
			TEXT("Generated host import names are content addressed"),
			Binding->GetObjectField(TEXT("host_import"))->GetStringField(TEXT("name")).StartsWith(TEXT("avid_ue_")));
		TestTrue(TEXT("Every v3 binding declares reload effect policy"), Binding->HasTypedField<EJson::String>(TEXT("reload_effect")));
	}

	TSharedPtr<FJsonObject> LegacyRoot;
	TestTrue(TEXT("Generated descriptor can be cloned for v2 compatibility"), ParseDescriptor(FirstJson, LegacyRoot));
	if (LegacyRoot.IsValid())
	{
		LegacyRoot->SetNumberField(TEXT("schema_version"), 2);
		for (const TSharedPtr<FJsonValue>& Value : LegacyRoot->GetArrayField(TEXT("bindings")))
		{
			if (const TSharedPtr<FJsonObject> Binding = Value.IsValid() ? Value->AsObject() : nullptr)
			{
				Binding->RemoveField(TEXT("reload_effect"));
			}
		}
		FString LegacyJson;
		FAvidScriptBindingPackageModel LegacyPackage;
		FString ErrorCategory;
		FString ErrorSource;
		TestTrue(TEXT("Descriptor v2 remains parseable"),
			SerializeDescriptor(LegacyRoot, LegacyJson)
			&& FAvidScriptBindingDescriptorParser::Parse(LegacyJson, LegacyPackage, ErrorCategory, ErrorSource));
		const FAvidScriptBindingFunctionModel* LegacyGetScale = LegacyPackage.Bindings.FindByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding) { return Binding.UeFunction == TEXT("GetActorScale3D"); });
		const FAvidScriptBindingFunctionModel* LegacySetScale = LegacyPackage.Bindings.FindByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding) { return Binding.UeFunction == TEXT("SetActorScale3D"); });
		if (TestNotNull(TEXT("Legacy getter survives normalization"), LegacyGetScale))
		{
			TestEqual(TEXT("Legacy const binding normalizes to no effect"),
				LegacyGetScale->ReloadEffect, EAvidScriptBindingReloadEffect::None);
		}
		if (TestNotNull(TEXT("Legacy setter survives normalization"), LegacySetScale))
		{
			TestEqual(TEXT("Legacy mutating binding normalizes to unsupported"),
				LegacySetScale->ReloadEffect, EAvidScriptBindingReloadEffect::Unsupported);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV3ProjectionTest,
	"AvidScript.Editor.BindingDescriptor.V3Projection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV3ProjectionTest::RunTest(const FString& Parameters)
{
	FString Json;
	FAvidScriptBindingDescriptorGenerateResult Result;
	TestTrue(TEXT("Default descriptor generates for projection checks"), FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(Json, Result));

	TSharedPtr<FJsonObject> Root;
	if (!ParseDescriptor(Json, Root) || !Root.IsValid())
	{
		AddError(TEXT("Default descriptor could not be parsed for projection checks."));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>& Bindings = Root->GetArrayField(TEXT("bindings"));
	const TSharedPtr<FJsonObject> GetLocation = FindBinding(
		Bindings,
		TEXT("/Script/Engine.Actor"),
		TEXT("K2_GetActorLocation"));
	TestNotNull(TEXT("Actor location binding exists"), GetLocation.Get());
	if (GetLocation.IsValid())
	{
		TestEqual(TEXT("Actor location read has no reload effect"),
			GetLocation->GetStringField(TEXT("reload_effect")), FString(TEXT("none")));
		TestEqual(
			TEXT("Actor location returns canonical FVector"),
			GetLocation->GetObjectField(TEXT("return"))->GetStringField(TEXT("canonical_type")),
			FString(TEXT("struct:/Script/CoreUObject.Vector")));
		TestEqual(
			TEXT("Actor location return direction is explicit"),
			GetLocation->GetObjectField(TEXT("return"))->GetStringField(TEXT("direction")),
			FString(TEXT("return")));
		TestEqual(
			TEXT("Actor location ABI is generated from owner handle and struct return"),
			GetLocation->GetObjectField(TEXT("host_import"))->GetStringField(TEXT("signature")),
			FString(TEXT("(iii)i")));
	}

	const TSharedPtr<FJsonObject> SetScale = FindBinding(
		Bindings,
		TEXT("/Script/Engine.Actor"),
		TEXT("SetActorScale3D"));
	TestNotNull(TEXT("Actor scale write binding exists"), SetScale.Get());
	if (SetScale.IsValid())
	{
		TestEqual(TEXT("Actor scale write declares reversible transform effect"),
			SetScale->GetStringField(TEXT("reload_effect")), FString(TEXT("actor_transform")));
		const TArray<TSharedPtr<FJsonValue>>& ReflectedParameters = SetScale->GetArrayField(TEXT("parameters"));
		TestEqual(TEXT("Actor scale write has one reflected parameter"), ReflectedParameters.Num(), 1);
		if (ReflectedParameters.Num() == 1)
		{
			TestEqual(
				TEXT("Scale parameter is canonical FVector"),
				ReflectedParameters[0]->AsObject()->GetStringField(TEXT("canonical_type")),
				FString(TEXT("struct:/Script/CoreUObject.Vector")));
			TestEqual(
				TEXT("Scale parameter direction is value"),
				ReflectedParameters[0]->AsObject()->GetStringField(TEXT("direction")),
				FString(TEXT("value")));
		}
		TestEqual(
			TEXT("Scale write ABI flattens FVector without a manual signature"),
			SetScale->GetObjectField(TEXT("host_import"))->GetStringField(TEXT("signature")),
			FString(TEXT("(iifff)i")));
	}

	const TSharedPtr<FJsonObject> RootComponent = FindBinding(
		Bindings,
		TEXT("/Script/Engine.Actor"),
		TEXT("K2_GetRootComponent"));
	TestNotNull(TEXT("Root component binding exists"), RootComponent.Get());
	if (RootComponent.IsValid())
	{
		TestEqual(TEXT("Root component read has no reload effect"),
			RootComponent->GetStringField(TEXT("reload_effect")), FString(TEXT("none")));
		TestEqual(
			TEXT("Root component return is a UObject handle type"),
			RootComponent->GetObjectField(TEXT("return"))->GetStringField(TEXT("canonical_type")),
			FString(TEXT("object:/Script/Engine.SceneComponent")));
		TestEqual(
			TEXT("Object return ABI uses an out handle address"),
			RootComponent->GetObjectField(TEXT("host_import"))->GetStringField(TEXT("signature")),
			FString(TEXT("(iii)i")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV3DefaultsTest,
	"AvidScript.Editor.BindingDescriptor.V3Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV3DefaultsTest::RunTest(const FString& Parameters)
{
	const FAvidScriptReflectedFunctionSelection Selection{
		TEXT("/Script/Engine.SceneComponent"),
		TEXT("SetVisibility")
	};
	FString Json;
	FAvidScriptBindingDescriptorGenerateResult Result;
	TestTrue(
		TEXT("Supported function with a reflected default generates"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.defaults"),
			{ Selection },
			Json,
			Result));

	TSharedPtr<FJsonObject> Root;
	if (!ParseDescriptor(Json, Root) || !Root.IsValid())
	{
		AddError(TEXT("Default-value descriptor could not be parsed."));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>& Bindings = Root->GetArrayField(TEXT("bindings"));
	TestEqual(TEXT("Default-value package contains one binding"), Bindings.Num(), 1);
	if (Bindings.Num() != 1 || !Bindings[0]->AsObject().IsValid())
	{
		return true;
	}
	TestEqual(TEXT("Unclassified SetVisibility is unsafe during candidate reload"),
		Bindings[0]->AsObject()->GetStringField(TEXT("reload_effect")), FString(TEXT("unsupported")));

	const TArray<TSharedPtr<FJsonValue>>& ReflectedParameters =
		Bindings[0]->AsObject()->GetArrayField(TEXT("parameters"));
	TestEqual(TEXT("SetVisibility exposes two reflected parameters"), ReflectedParameters.Num(), 2);
	if (ReflectedParameters.Num() == 2)
	{
		const TSharedPtr<FJsonObject> RequiredParameter = ReflectedParameters[0]->AsObject();
		const TSharedPtr<FJsonObject> DefaultParameter = ReflectedParameters[1]->AsObject();
		TestFalse(TEXT("Required parameter is not marked defaulted"), RequiredParameter->GetBoolField(TEXT("has_default")));
		TestTrue(TEXT("Optional parameter is marked defaulted"), DefaultParameter->GetBoolField(TEXT("has_default")));
		TestEqual(
			TEXT("Optional parameter preserves reflected default text"),
			DefaultParameter->GetStringField(TEXT("default_value")),
			FString(TEXT("false")));
	}

	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV3FailureTest,
	"AvidScript.Editor.BindingDescriptor.V3Failure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV3FailureTest::RunTest(const FString& Parameters)
{
	TArray<FAvidScriptReflectedFunctionSelection> DuplicateSelections =
		FAvidScriptEditorBindingDescriptorGenerator::MakeDefaultSelections();
	const FAvidScriptReflectedFunctionSelection DuplicateSelection = DuplicateSelections[0];
	DuplicateSelections.Add(DuplicateSelection);
	FString Json;
	FAvidScriptBindingDescriptorGenerateResult Result;
	TestFalse(
		TEXT("Duplicate reflected selections fail closed"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.core"),
			DuplicateSelections,
			Json,
			Result));
	TestEqual(TEXT("Duplicate selection reports stable category"), Result.ErrorCategory, FString(TEXT("duplicate_selection")));
	TestTrue(TEXT("Duplicate selection produces no partial JSON"), Json.IsEmpty());

	const FAvidScriptReflectedFunctionSelection UnsupportedSelection{
		TEXT("/Script/Engine.Actor"),
		TEXT("K2_SetActorLocation")
	};
	TestFalse(
		TEXT("Unsupported FHitResult projection fails closed"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.core"),
			{ UnsupportedSelection },
			Json,
			Result));
	TestEqual(TEXT("Unsupported struct reports property category"), Result.ErrorCategory, FString(TEXT("unsupported_property")));
	TestTrue(TEXT("Unsupported property identifies FHitResult"), Result.ErrorSource.Contains(TEXT("FHitResult")));

	const FAvidScriptReflectedFunctionSelection MissingSelection{
		TEXT("/Script/Engine.Actor"),
		TEXT("AvidScriptMissingFunction")
	};
	TestFalse(
		TEXT("Missing reflected function fails closed"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.core"),
			{ MissingSelection },
			Json,
			Result));
	TestEqual(TEXT("Missing function reports stable category"), Result.ErrorCategory, FString(TEXT("function_missing")));

	return true;
}

#endif
