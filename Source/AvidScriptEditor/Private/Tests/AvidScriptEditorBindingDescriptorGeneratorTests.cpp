#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingDescriptorGenerator.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "BindingGeneration/AvidScriptEditorObjectTypeGraph.h"
#include "Algo/Reverse.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

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
			&& (Binding->HasField(TEXT("ue_member"))
				? Binding->GetStringField(TEXT("ue_member"))
				: Binding->GetStringField(TEXT("ue_function"))) == FunctionName)
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

const FAvidScriptEditorObjectTypeNode* FindObjectTypeNode(
	const FAvidScriptEditorObjectTypeGraph& Graph,
	const UClass* Class)
{
	const FString CanonicalClassPath = Class->GetPathName();
	return Graph.Nodes.FindByPredicate([&CanonicalClassPath](const FAvidScriptEditorObjectTypeNode& Node)
	{
		return Node.CanonicalClassPath == CanonicalClassPath;
	});
}

UClass* MakeDuplicateShortNameObjectTypeClass(const TCHAR* ModulePath)
{
	UPackage* Package = CreatePackage(ModulePath);
	UClass* Class = NewObject<UClass>(Package, TEXT("AvidScriptObjectTypeGraphDuplicate"), RF_Transient);
	Class->SetSuperStruct(UObject::StaticClass());
	return Class;
}

bool AreObjectTypeGraphsEqual(
	const FAvidScriptEditorObjectTypeGraph& Left,
	const FAvidScriptEditorObjectTypeGraph& Right)
{
	if (Left.Nodes.Num() != Right.Nodes.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Nodes.Num(); ++Index)
	{
		const FAvidScriptEditorObjectTypeNode& LeftNode = Left.Nodes[Index];
		const FAvidScriptEditorObjectTypeNode& RightNode = Right.Nodes[Index];
		if (LeftNode.TypeId != RightNode.TypeId
			|| LeftNode.CanonicalClassPath != RightNode.CanonicalClassPath
			|| LeftNode.BaseTypeId != RightNode.BaseTypeId
			|| LeftNode.Ordinal != RightNode.Ordinal)
		{
			return false;
		}
	}
	return true;
}

FAvidScriptBindingPackageModel MakeV7CanonicalSerializerPackage()
{
	FAvidScriptBindingPackageModel Package;
	Package.SchemaVersion = 7;
	Package.GeneratorVersion = TEXT("51.1.serializer.test");
	Package.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = TEXT("avidscript.test.v7_canonical_serializer");

	FAvidScriptBindingTypeModel ObjectType;
	ObjectType.CanonicalType = TEXT("object:/Script/CoreUObject.Object");
	ObjectType.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
			ObjectType.CanonicalType,
			{});
	ObjectType.Kind = TEXT("object_handle");
	ObjectType.CppType = TEXT("UObject");
	ObjectType.Size = 8;
	ObjectType.Alignment = 4;
	ObjectType.AbiTypes = { TEXT("i"), TEXT("i") };
	ObjectType.ObjectTypeOrdinal = 0;
	ObjectType.ClassPath = TEXT("/Script/CoreUObject.Object");
	Package.Types.Add(ObjectType);

	FAvidScriptBindingTypeModel ActorType;
	ActorType.CanonicalType = TEXT("object:/Script/Engine.Actor");
	ActorType.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
			ActorType.CanonicalType,
			{});
	ActorType.Kind = TEXT("object_handle");
	ActorType.CppType = TEXT("AActor");
	ActorType.Size = 8;
	ActorType.Alignment = 4;
	ActorType.AbiTypes = { TEXT("i"), TEXT("i") };
	ActorType.ObjectTypeOrdinal = 1;
	ActorType.ClassPath = TEXT("/Script/Engine.Actor");
	ActorType.BaseTypeId = ObjectType.StableId;
	Package.Types.Add(ActorType);
	Package.SelfTypeId = ActorType.StableId;

	FAvidScriptBindingClassReferenceModel Reference;
	Reference.Ordinal = 0;
	Reference.ScriptName = TEXT("ConsoleClass");
	Reference.ClassPath = TEXT("/Script/Engine.Console");
	Reference.BaseClassPath = TEXT("/Script/CoreUObject.Object");
	Reference.LoadPolicy = TEXT("EditorLoad");
	Reference.ResultTypeId = ObjectType.StableId;
	Reference.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
			Reference.ClassPath,
			Reference.BaseClassPath,
			Reference.LoadPolicy);
	Package.ClassReferences.Add(Reference);

	FAvidScriptBindingObjectFactoryModel Factory;
	Factory.Ordinal = 0;
	Factory.ScriptName = TEXT("Console");
	Factory.ClassReferenceId = Reference.StableId;
	Factory.Kind = EAvidScriptObjectFactoryKind::NewObject;
	Factory.OuterTypeId = ObjectType.StableId;
	Factory.Ownership = EAvidScriptObjectOwnershipPolicy::Session;
	Factory.Registration = EAvidScriptComponentRegistrationPolicy::None;
	Factory.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
			Factory.ClassReferenceId,
			Factory.Kind,
			Factory.OuterTypeId,
			Factory.Ownership,
			Factory.Registration);
	Package.ObjectFactories.Add(Factory);

	Package.SelectionHash =
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
	Package.PackageHash =
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);
	return Package;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorObjectTypeGraphDeterminismTest,
	"AvidScript.Editor.BindingDescriptor.ObjectTypeGraphDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorObjectTypeGraphDeterminismTest::RunTest(const FString& Parameters)
{
	UClass* FirstDuplicate = MakeDuplicateShortNameObjectTypeClass(TEXT("/Script/AvidScriptGraphOne"));
	UClass* SecondDuplicate = MakeDuplicateShortNameObjectTypeClass(TEXT("/Script/AvidScriptGraphTwo"));
	const TArray<FAvidScriptProjectBindingClassSpec> ClassReferences = {
		{ TEXT("ActorClass"), TEXT("/Script/Engine.Actor"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") }
	};
	TArray<UClass*> HandleClasses = {
		USceneComponent::StaticClass(),
		FirstDuplicate,
		AActor::StaticClass(),
		SecondDuplicate,
		USceneComponent::StaticClass()
	};

	FAvidScriptEditorObjectTypeGraph FirstGraph;
	FString ErrorCategory;
	FString ErrorDetails;
	TestTrue(
		TEXT("Object type graph accepts unordered handle classes"),
		FAvidScriptEditorObjectTypeGraph::Build(
			HandleClasses,
			AActor::StaticClass(),
			ClassReferences,
			FirstGraph,
			ErrorCategory,
			ErrorDetails));
	TestTrue(TEXT("Object type graph does not report an error"), ErrorCategory.IsEmpty());

	Algo::Reverse(HandleClasses);
	FAvidScriptEditorObjectTypeGraph ReorderedGraph;
	TestTrue(
		TEXT("Object type graph accepts reordered handle classes"),
		FAvidScriptEditorObjectTypeGraph::Build(
			HandleClasses,
			AActor::StaticClass(),
			ClassReferences,
			ReorderedGraph,
			ErrorCategory,
			ErrorDetails));
	TestTrue(TEXT("Reflection input order does not change the graph"), AreObjectTypeGraphsEqual(ReorderedGraph, FirstGraph));

	FString PreviousPath;
	for (int32 Index = 0; Index < FirstGraph.Nodes.Num(); ++Index)
	{
		const FAvidScriptEditorObjectTypeNode& Node = FirstGraph.Nodes[Index];
		TestTrue(TEXT("Object type graph sorts by canonical class path"), PreviousPath.IsEmpty() || PreviousPath < Node.CanonicalClassPath);
		TestEqual(TEXT("Object type graph assigns contiguous ordinals"), Node.Ordinal, Index);
		PreviousPath = Node.CanonicalClassPath;
	}

	const FAvidScriptEditorObjectTypeNode* ObjectNode = FindObjectTypeNode(FirstGraph, UObject::StaticClass());
	const FAvidScriptEditorObjectTypeNode* ActorNode = FindObjectTypeNode(FirstGraph, AActor::StaticClass());
	const FAvidScriptEditorObjectTypeNode* ActorComponentNode = FindObjectTypeNode(FirstGraph, UActorComponent::StaticClass());
	const FAvidScriptEditorObjectTypeNode* SceneComponentNode = FindObjectTypeNode(FirstGraph, USceneComponent::StaticClass());
	const FAvidScriptEditorObjectTypeNode* FirstDuplicateNode = FindObjectTypeNode(FirstGraph, FirstDuplicate);
	const FAvidScriptEditorObjectTypeNode* SecondDuplicateNode = FindObjectTypeNode(FirstGraph, SecondDuplicate);
	TestNotNull(TEXT("Graph contains the UObject root"), ObjectNode);
	TestNotNull(TEXT("Graph contains the Actor self class"), ActorNode);
	TestNotNull(TEXT("Graph contains the ActorComponent super class"), ActorComponentNode);
	TestNotNull(TEXT("Graph contains the SceneComponent handle class"), SceneComponentNode);
	TestNotNull(TEXT("Graph contains the first duplicate short name"), FirstDuplicateNode);
	TestNotNull(TEXT("Graph contains the second duplicate short name"), SecondDuplicateNode);
	if (ObjectNode != nullptr && ActorNode != nullptr && ActorComponentNode != nullptr && SceneComponentNode != nullptr)
	{
		TestTrue(TEXT("UObject is the root edge"), ObjectNode->BaseTypeId.IsEmpty());
		TestEqual(TEXT("Actor directly derives from UObject"), ActorNode->BaseTypeId, ObjectNode->TypeId);
		TestEqual(TEXT("SceneComponent directly derives from ActorComponent"), SceneComponentNode->BaseTypeId, ActorComponentNode->TypeId);
	}
	if (FirstDuplicateNode != nullptr && SecondDuplicateNode != nullptr)
	{
		TestNotEqual(TEXT("Cross-module duplicate short names have distinct type ids"), FirstDuplicateNode->TypeId, SecondDuplicateNode->TypeId);
	}

	TArray<UClass*> NullHandleClasses = { nullptr };
	FAvidScriptEditorObjectTypeGraph InvalidGraph;
	TestFalse(
		TEXT("Null handle classes fail closed"),
		FAvidScriptEditorObjectTypeGraph::Build(
			NullHandleClasses,
			nullptr,
			{},
			InvalidGraph,
			ErrorCategory,
			ErrorDetails));
	TestEqual(TEXT("Null handle classes use a stable error category"), ErrorCategory, FString(TEXT("object_type_class_invalid")));

	const TArray<FAvidScriptProjectBindingClassSpec> MissingBaseReferences = {
		{ TEXT("MissingBase"), TEXT("/Script/Engine.Actor"), TEXT("/Script/AvidScriptMissing.NoBase"), TEXT("EditorLoad") }
	};
	TestFalse(
		TEXT("Missing class-reference bases fail closed"),
		FAvidScriptEditorObjectTypeGraph::Build(
			{},
			nullptr,
			MissingBaseReferences,
			InvalidGraph,
			ErrorCategory,
			ErrorDetails));
	TestEqual(
		TEXT("Missing class-reference bases use a stable error category"),
		ErrorCategory,
		FString(TEXT("object_type_class_reference_base_missing")));

	FAvidScriptEditorObjectTypeGraph StaticOnlyGraph;
	TestTrue(
		TEXT("Static-only packages accept an empty graph self class"),
		FAvidScriptEditorObjectTypeGraph::Build({}, nullptr, {}, StaticOnlyGraph, ErrorCategory, ErrorDetails));
	TestEqual(TEXT("Static-only packages have no handle-capable graph nodes"), StaticOnlyGraph.Nodes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV7CanonicalSerializerTest,
	"AvidScript.Editor.BindingDescriptor.V7CanonicalSerializer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV7CanonicalSerializerTest::RunTest(
	const FString& Parameters)
{
	const FAvidScriptBindingPackageModel Package =
		MakeV7CanonicalSerializerPackage();
	FString CanonicalJson;
	TestTrue(
		TEXT("Production canonical serializer emits descriptor v7"),
		FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
			Package,
			CanonicalJson));

	FAvidScriptBindingPackageModel ParsedPackage;
	FString ErrorCategory;
	FString ErrorSource;
	TestTrue(
		TEXT("Shared parser accepts production canonical descriptor v7"),
		FAvidScriptBindingDescriptorParser::Parse(
			CanonicalJson,
			ParsedPackage,
			ErrorCategory,
			ErrorSource));
	TestEqual(
		TEXT("Canonical descriptor retains one object factory"),
		ParsedPackage.ObjectFactories.Num(),
		1);

	for (int32 LegacySchemaVersion = 2; LegacySchemaVersion <= 6;
		++LegacySchemaVersion)
	{
		FAvidScriptBindingPackageModel LegacyBaseline = Package;
		LegacyBaseline.SchemaVersion = LegacySchemaVersion;
		LegacyBaseline.ObjectFactories.Empty();
		LegacyBaseline.SelectionHash =
			FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(
				LegacyBaseline);
		LegacyBaseline.PackageHash =
			FAvidScriptBindingDescriptorIdentity::MakePackageHash(
				LegacyBaseline);

		FAvidScriptBindingPackageModel LegacyWithIgnoredFactory =
			LegacyBaseline;
		LegacyWithIgnoredFactory.ObjectFactories = Package.ObjectFactories;
		FString BaselineJson;
		FString FactoryBearingJson;
		TestTrue(
			TEXT("Legacy baseline canonical serialization succeeds"),
			FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
				LegacyBaseline,
				BaselineJson));
		TestTrue(
			TEXT("Legacy factory-bearing model canonical serialization succeeds"),
			FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
				LegacyWithIgnoredFactory,
				FactoryBearingJson));
		TestEqual(
			*FString::Printf(
				TEXT("Schema v%d canonical bytes ignore v7-only factories"),
				LegacySchemaVersion),
			FactoryBearingJson,
			BaselineJson);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV7FactoryGenerationTest,
	"AvidScript.Editor.BindingDescriptor.V7FactoryGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV7FactoryGenerationTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.project.factory_generation");
	Profile.SelfClassPath = TEXT("/Script/Engine.Actor");

	const TArray<FAvidScriptProjectBindingClassSpec> ClassReferences = {
		{
			TEXT("InventoryStateClass"),
			TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"),
			TEXT("/Script/CoreUObject.Object"),
			TEXT("EditorLoad")
		},
		{
			TEXT("SceneComponentClass"),
			TEXT("/Script/Engine.SceneComponent"),
			TEXT("/Script/Engine.ActorComponent"),
			TEXT("EditorLoad")
		}
	};
	TArray<FAvidScriptProjectObjectFactorySpec> ObjectFactories = {
		{
			TEXT("InventoryState"),
			TEXT("InventoryStateClass"),
			TEXT("/Script/CoreUObject.Object"),
			EAvidScriptProjectObjectFactoryKind::NewObject,
			EAvidScriptProjectObjectOwnership::Session,
			EAvidScriptProjectComponentRegistration::None
		},
		{
			TEXT("SceneComponent"),
			TEXT("SceneComponentClass"),
			TEXT("/Script/Engine.Actor"),
			EAvidScriptProjectObjectFactoryKind::ActorComponent,
			EAvidScriptProjectObjectOwnership::Session,
			EAvidScriptProjectComponentRegistration::RegisterInstance
		}
	};

	FString FirstJson;
	FAvidScriptBindingSelectionResolveResult FirstSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult FirstResult;
	if (!TestTrue(
		TEXT("Factory-aware profile generates descriptor v7"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			ClassReferences,
			ObjectFactories,
			FirstJson,
			FirstSelectionResult,
			FirstResult)))
	{
		AddError(FirstResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("Generator reports two object factories"),
		FirstResult.ObjectFactoryCount,
		2);

	Algo::Reverse(ObjectFactories);
	FString ReorderedJson;
	FAvidScriptBindingSelectionResolveResult ReorderedSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult ReorderedResult;
	TestTrue(
		TEXT("Reordered factory input generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			ClassReferences,
			ObjectFactories,
			ReorderedJson,
			ReorderedSelectionResult,
			ReorderedResult));
	TestEqual(
		TEXT("Factory input order does not change descriptor bytes"),
		ReorderedJson,
		FirstJson);

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
		TEXT("Shared parser accepts generated descriptor v7"),
		FAvidScriptBindingDescriptorParser::Parse(
			FirstJson,
			Package,
			ErrorCategory,
			ErrorSource)))
	{
		AddError(FString::Printf(
			TEXT("%s:%s"),
			*ErrorCategory,
			*ErrorSource));
		return false;
	}
	TestEqual(TEXT("Factory descriptor schema is v7"), Package.SchemaVersion, 7);
	TestEqual(
		TEXT("Factory descriptor retains two factories"),
		Package.ObjectFactories.Num(),
		2);

	TSet<FString> ObjectClassPaths;
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		if (Type.ObjectTypeOrdinal != INDEX_NONE)
		{
			ObjectClassPaths.Add(Type.ClassPath);
		}
	}
	TestTrue(
		TEXT("Object type closure contains the concrete UObject factory class"),
		ObjectClassPaths.Contains(
			TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject")));
	TestTrue(
		TEXT("Object type closure contains the concrete component factory class"),
		ObjectClassPaths.Contains(TEXT("/Script/Engine.SceneComponent")));
	TestTrue(
		TEXT("Object type closure contains the Actor Outer constraint"),
		ObjectClassPaths.Contains(TEXT("/Script/Engine.Actor")));

	FString PreviousStableId;
	for (int32 Index = 0; Index < Package.ObjectFactories.Num(); ++Index)
	{
		const FAvidScriptBindingObjectFactoryModel& Factory =
			Package.ObjectFactories[Index];
		TestEqual(TEXT("Factory ordinal is contiguous"), Factory.Ordinal, Index);
		TestTrue(
			TEXT("Factories use stable-id order"),
			PreviousStableId.IsEmpty() || PreviousStableId < Factory.StableId);
		PreviousStableId = Factory.StableId;
	}

	FString LegacyJson;
	FAvidScriptBindingSelectionResolveResult LegacySelectionResult;
	FAvidScriptBindingDescriptorGenerateResult LegacyResult;
	TestTrue(
		TEXT("Legacy class-reference overload still generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			ClassReferences,
			LegacyJson,
			LegacySelectionResult,
			LegacyResult));
	FAvidScriptBindingPackageModel LegacyPackage;
	TestTrue(
		TEXT("Legacy descriptor remains parseable"),
		FAvidScriptBindingDescriptorParser::Parse(
			LegacyJson,
			LegacyPackage,
			ErrorCategory,
			ErrorSource));
	TestEqual(
		TEXT("Legacy descriptor remains schema v6"),
		LegacyPackage.SchemaVersion,
		6);
	TestTrue(
		TEXT("Legacy descriptor does not publish object factories"),
		LegacyPackage.ObjectFactories.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV5DeterminismTest,
	"AvidScript.Editor.BindingDescriptor.V5Determinism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV5DeterminismTest::RunTest(const FString& Parameters)
{
	FString FirstJson;
	FAvidScriptBindingDescriptorGenerateResult FirstResult;
	TestTrue(
		TEXT("Default binding descriptor v5 generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(FirstJson, FirstResult));
	TestTrue(TEXT("Default result succeeds"), FirstResult.bSucceeded);
	TestEqual(TEXT("Default v3 selection contains eight safe functions"), FirstResult.BindingCount, 8);
	TestTrue(TEXT("Default descriptor contains projected types"), FirstResult.TypeCount >= 5);
	TestTrue(TEXT("Package hash is a complete SHA-256"), IsLowerHexSha256(FirstResult.PackageHash));
	TestTrue(TEXT("Selection hash is a complete SHA-256"), IsLowerHexSha256(FirstResult.SelectionHash));

	FString SecondJson;
	FAvidScriptBindingDescriptorGenerateResult SecondResult;
	TestTrue(
		TEXT("Repeated binding descriptor v5 generation succeeds"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(SecondJson, SecondResult));
	TestEqual(TEXT("Descriptor bytes are deterministic"), SecondJson, FirstJson);
	TestEqual(TEXT("Repeated package hash is deterministic"), SecondResult.PackageHash, FirstResult.PackageHash);

	TSharedPtr<FJsonObject> Root;
	TestTrue(TEXT("Descriptor v3 is valid JSON"), ParseDescriptor(FirstJson, Root));
	if (!Root.IsValid())
	{
		return true;
	}

	TestEqual(TEXT("Descriptor schema is v6"), Root->GetIntegerField(TEXT("schema_version")), 6);
	TestEqual(TEXT("Descriptor source is UE reflection"), Root->GetStringField(TEXT("source")), FString(TEXT("ue_reflection")));
	TestEqual(
		TEXT("Default package name is stable"),
		Root->GetStringField(TEXT("package_name")),
		FString(TEXT("avidscript.engine.core")));
	TestEqual(TEXT("JSON package hash matches result"), Root->GetStringField(TEXT("package_hash")), FirstResult.PackageHash);
	TestEqual(TEXT("JSON selection hash matches result"), Root->GetStringField(TEXT("selection_hash")), FirstResult.SelectionHash);
	TestFalse(TEXT("Descriptor v5 does not expose handwritten projection fields"), FirstJson.Contains(TEXT("\"projection\"")));
	TestEqual(TEXT("Descriptor v5 publishes an empty class table by default"), Root->GetArrayField(TEXT("class_references")).Num(), 0);

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
		TestTrue(TEXT("Every v5 binding declares reload effect policy"), Binding->HasTypedField<EJson::String>(TEXT("reload_effect")));
	}

	TSharedPtr<FJsonObject> LegacyRoot;
	TestTrue(TEXT("Generated descriptor can be cloned for v2 compatibility"), ParseDescriptor(FirstJson, LegacyRoot));
	if (LegacyRoot.IsValid())
	{
		LegacyRoot->SetNumberField(TEXT("schema_version"), 2);
		LegacyRoot->RemoveField(TEXT("class_references"));
		for (const TSharedPtr<FJsonValue>& Value : LegacyRoot->GetArrayField(TEXT("bindings")))
		{
			if (const TSharedPtr<FJsonObject> Binding = Value.IsValid() ? Value->AsObject() : nullptr)
			{
				Binding->SetStringField(TEXT("ue_function"), Binding->GetStringField(TEXT("ue_member")));
				Binding->RemoveField(TEXT("binding_kind"));
				Binding->RemoveField(TEXT("ue_member"));
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
	FAvidScriptEditorBindingDescriptorV5PropertyGetTest,
	"AvidScript.Editor.BindingDescriptor.V5PropertyGet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV5PropertyGetTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedFunctionSelection> Functions = {
		{ TEXT("/Script/Engine.Actor"), TEXT("GetActorScale3D") }
	};
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation") },
		{ TEXT("/Script/Engine.Actor"), TEXT("InitialLifeSpan") },
		{ TEXT("/Script/Engine.Actor"), TEXT("RootComponent") }
	};
	FString FirstJson;
	FAvidScriptBindingDescriptorGenerateResult FirstResult;
	TestTrue(
		TEXT("Descriptor v4 combines functions and readable properties"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.engine.property_get"),
			Functions,
			Properties,
			FirstJson,
			FirstResult));
	TestEqual(TEXT("Combined descriptor has four bindings"), FirstResult.BindingCount, 4);

	FString SecondJson;
	FAvidScriptBindingDescriptorGenerateResult SecondResult;
	TestTrue(
		TEXT("Repeated descriptor v4 generation succeeds"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.engine.property_get"),
			Functions,
			Properties,
			SecondJson,
			SecondResult));
	TestEqual(TEXT("Descriptor v4 bytes are deterministic"), SecondJson, FirstJson);
	TestEqual(TEXT("Descriptor v4 package hash is deterministic"), SecondResult.PackageHash, FirstResult.PackageHash);

	TSharedPtr<FJsonObject> Root;
	TestTrue(TEXT("Descriptor v4 is valid JSON"), ParseDescriptor(FirstJson, Root));
	if (!Root.IsValid())
	{
		return true;
	}
	TestEqual(TEXT("Property descriptor uses schema v6"), Root->GetIntegerField(TEXT("schema_version")), 6);
	const TArray<TSharedPtr<FJsonValue>>& Bindings = Root->GetArrayField(TEXT("bindings"));
	int32 FunctionCount = 0;
	int32 PropertyCount = 0;
	bool bFoundObjectProperty = false;
	for (const TSharedPtr<FJsonValue>& Value : Bindings)
	{
		const TSharedPtr<FJsonObject> Binding = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Binding.IsValid())
		{
			continue;
		}
		TestTrue(TEXT("v4 binding has first-class member kind"), Binding->HasTypedField<EJson::String>(TEXT("binding_kind")));
		TestTrue(TEXT("v4 binding has a reflected UE member"), Binding->HasTypedField<EJson::String>(TEXT("ue_member")));
		TestFalse(TEXT("v4 binding does not publish legacy ue_function"), Binding->HasField(TEXT("ue_function")));
		if (Binding->GetStringField(TEXT("binding_kind")) == TEXT("function"))
		{
			++FunctionCount;
			TestEqual(TEXT("Function keeps cached ProcessEvent dispatch"), Binding->GetStringField(TEXT("dispatch_mode")), FString(TEXT("cached_process_event")));
		}
		else
		{
			++PropertyCount;
			TestEqual(TEXT("Property uses cached getter dispatch"), Binding->GetStringField(TEXT("dispatch_mode")), FString(TEXT("cached_property_get")));
			TestEqual(TEXT("Property getter has no parameters"), Binding->GetArrayField(TEXT("parameters")).Num(), 0);
			TestEqual(TEXT("Property getter ABI uses handle and return address"), Binding->GetObjectField(TEXT("host_import"))->GetStringField(TEXT("signature")), FString(TEXT("(iii)i")));
			if (Binding->GetStringField(TEXT("ue_member")) == TEXT("RootComponent"))
			{
				bFoundObjectProperty = true;
				const TSharedPtr<FJsonObject> ReturnValue = Binding->GetObjectField(TEXT("return"));
				TestEqual(TEXT("Object property projects to object_handle"), ReturnValue->GetStringField(TEXT("kind")), FString(TEXT("object_handle")));
				TestEqual(
					TEXT("Object property preserves the reflected component type"),
					ReturnValue->GetStringField(TEXT("canonical_type")),
					FString(TEXT("object:/Script/Engine.SceneComponent")));
				TestEqual(TEXT("Object property publishes slot and generation ABI"), ReturnValue->GetArrayField(TEXT("abi_types")).Num(), 2);
			}
		}
	}
	TestEqual(TEXT("v4 retains one function"), FunctionCount, 1);
	TestEqual(TEXT("v4 publishes three property getters"), PropertyCount, 3);
	TestTrue(TEXT("v4 publishes the object reference property"), bFoundObjectProperty);

	FAvidScriptBindingPackageModel ParsedPackage;
	FString ErrorCategory;
	FString ErrorSource;
	TestTrue(
		TEXT("Shared descriptor parser accepts v4 property bindings"),
		FAvidScriptBindingDescriptorParser::Parse(
			FirstJson,
			ParsedPackage,
			ErrorCategory,
			ErrorSource));
	TestEqual(TEXT("Parsed package retains schema v6"), ParsedPackage.SchemaVersion, 6);
	TestEqual(TEXT("Parsed package retains all bindings"), ParsedPackage.Bindings.Num(), 4);
	TestEqual(
		TEXT("Parsed property getter count is stable"),
		ParsedPackage.Bindings.FilterByPredicate([](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_get");
		}).Num(),
		3);

	TSharedPtr<FJsonObject> TamperedRoot;
	TestTrue(TEXT("v4 descriptor can be cloned for tamper checks"), ParseDescriptor(FirstJson, TamperedRoot));
	if (TamperedRoot.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& Value : TamperedRoot->GetArrayField(TEXT("bindings")))
		{
			const TSharedPtr<FJsonObject> Binding = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Binding.IsValid() && Binding->GetStringField(TEXT("binding_kind")) == TEXT("property_get"))
			{
				Binding->GetObjectField(TEXT("host_import"))->SetStringField(TEXT("signature"), TEXT("(ii)i"));
				break;
			}
		}
		FString TamperedJson;
		TestFalse(
			TEXT("Property getter ABI tampering fails closed"),
			SerializeDescriptor(TamperedRoot, TamperedJson)
			&& FAvidScriptBindingDescriptorParser::Parse(
				TamperedJson,
				ParsedPackage,
				ErrorCategory,
				ErrorSource));
		TestEqual(TEXT("Tampered property ABI uses stable category"), ErrorCategory, FString(TEXT("descriptor_contract_invalid")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV5ClassReferenceTest,
	"AvidScript.Editor.BindingDescriptor.V5ClassReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV5ClassReferenceTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedFunctionSelection> Functions = {
		{ TEXT("/Script/Engine.Actor"), TEXT("GetActorScale3D") }
	};
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences = {
		{ TEXT("CameraClass"), TEXT("/Script/Engine.CameraActor"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") },
		{ TEXT("StaticMeshClass"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") }
	};
	FString FirstJson;
	FAvidScriptBindingDescriptorGenerateResult FirstResult;
	TestTrue(
		TEXT("Schema v6 descriptor generates with class references"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
			TEXT("avidscript.project.class_refs"),
			Functions,
			{},
			ClassReferences,
			FirstJson,
			FirstResult));
	TestEqual(TEXT("Generator reports two class references"), FirstResult.ClassReferenceCount, 2);

	Algo::Reverse(ClassReferences);
	FString ReorderedJson;
	FAvidScriptBindingDescriptorGenerateResult ReorderedResult;
	TestTrue(
		TEXT("Reordered class reference input generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
			TEXT("avidscript.project.class_refs"),
			Functions,
			{},
			ClassReferences,
			ReorderedJson,
			ReorderedResult));
	TestEqual(TEXT("Class reference input order does not change descriptor bytes"), ReorderedJson, FirstJson);

	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("Class reference descriptor parses as JSON"), ParseDescriptor(FirstJson, Root))
		|| !Root.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Class reference descriptor uses schema v6"), Root->GetIntegerField(TEXT("schema_version")), 6);
	const TArray<TSharedPtr<FJsonValue>>& ReferenceValues = Root->GetArrayField(TEXT("class_references"));
	TestEqual(TEXT("Descriptor publishes two class references"), ReferenceValues.Num(), 2);
	FString PreviousStableId;
	for (int32 Index = 0; Index < ReferenceValues.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Reference = ReferenceValues[Index]->AsObject();
		TestEqual(TEXT("Class reference ordinal is contiguous"), Reference->GetIntegerField(TEXT("ordinal")), Index);
		const FString StableId = Reference->GetStringField(TEXT("stable_id"));
		TestTrue(TEXT("Class references use stable-id order"), PreviousStableId.IsEmpty() || PreviousStableId < StableId);
		PreviousStableId = StableId;
	}

	FAvidScriptBindingPackageModel ParsedPackage;
	FString ErrorCategory;
	FString ErrorSource;
	TestTrue(
		TEXT("Shared parser accepts the class reference table"),
		FAvidScriptBindingDescriptorParser::Parse(
			FirstJson,
			ParsedPackage,
			ErrorCategory,
			ErrorSource));
	TestEqual(TEXT("Shared model retains two class references"), ParsedPackage.ClassReferences.Num(), 2);

	FString ClassOnlyJson;
	FAvidScriptBindingDescriptorGenerateResult ClassOnlyResult;
	TestTrue(
		TEXT("Schema v6 class table can form an independent package"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
			TEXT("avidscript.project.class_refs_only"),
			{},
			{},
			ClassReferences,
			ClassOnlyJson,
			ClassOnlyResult));
	TestEqual(TEXT("Class-only descriptor has no binding imports"), ClassOnlyResult.BindingCount, 0);
	TestTrue(
		TEXT("Shared parser accepts a class-only package"),
		FAvidScriptBindingDescriptorParser::Parse(
			ClassOnlyJson,
			ParsedPackage,
			ErrorCategory,
			ErrorSource));
	TestTrue(TEXT("Class-only descriptor retains its class-result type graph"), !ParsedPackage.Types.IsEmpty());
	TestEqual(TEXT("Class-only descriptor retains its class table"), ParsedPackage.ClassReferences.Num(), 2);

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (TestTrue(
		TEXT("Runtime builds the immutable class plan"),
		FAvidScriptBindingPackage::LoadDescriptor(FirstJson, RuntimePackage, LoadResult))
		&& RuntimePackage.IsValid())
	{
		TestEqual(TEXT("Runtime reports two class plans"), LoadResult.ClassReferenceCount, 2);
		TestEqual(TEXT("Runtime exposes two class plans"), RuntimePackage->GetClassReferenceCount(), 2);
		for (uint32 Ordinal = 0; Ordinal < 2; ++Ordinal)
		{
			UClass* Class = nullptr;
			UClass* BaseClass = nullptr;
			TestTrue(TEXT("Class ordinal resolves without a name lookup"), RuntimePackage->TryResolveClassReference(Ordinal, Class, BaseClass));
			TestTrue(TEXT("Resolved class satisfies cached base"), Class != nullptr && BaseClass != nullptr && Class->IsChildOf(BaseClass));
			TestTrue(TEXT("Resolved class is Actor-derived"), Class != nullptr && Class->IsChildOf(AActor::StaticClass()));
		}
		UClass* OutOfRangeClass = nullptr;
		UClass* OutOfRangeBase = nullptr;
		TestFalse(TEXT("Out-of-range class ordinal fails closed"), RuntimePackage->TryResolveClassReference(2, OutOfRangeClass, OutOfRangeBase));
	}

	TArray<FAvidScriptProjectBindingClassSpec> AliasReferences = ClassReferences;
	AliasReferences[0].ScriptName = TEXT("RenamedCameraClass");
	FString AliasJson;
	FAvidScriptBindingDescriptorGenerateResult AliasResult;
	TestTrue(
		TEXT("Class reference alias change generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
			TEXT("avidscript.project.class_refs"),
			Functions,
			{},
			AliasReferences,
			AliasJson,
			AliasResult));
	TestNotEqual(TEXT("Class reference alias participates in package identity"), AliasResult.PackageHash, FirstResult.PackageHash);

	TArray<FAvidScriptProjectBindingClassSpec> ReservedNameReferences = ClassReferences;
	ReservedNameReferences[0].ScriptName = TEXT("ProjectClasses");
	FString ReservedNameJson;
	FAvidScriptBindingDescriptorGenerateResult ReservedNameResult;
	TestFalse(
		TEXT("Generated class container name is reserved"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
			TEXT("avidscript.project.reserved_class_ref"),
			Functions,
			{},
			ReservedNameReferences,
			ReservedNameJson,
			ReservedNameResult));
	TestEqual(
		TEXT("Reserved class container name uses stable diagnostic"),
		ReservedNameResult.ErrorCategory,
		FString(TEXT("class_reference_invalid")));

	const auto ParserRejectsMutation = [this, &Root](
		const TCHAR* Label,
		const TFunctionRef<void(const TArray<TSharedPtr<FJsonValue>>&)>& Mutate)
	{
		TSharedPtr<FJsonObject> MutatedRoot;
		FString SourceJson;
		SerializeDescriptor(Root, SourceJson);
		ParseDescriptor(SourceJson, MutatedRoot);
		const TArray<TSharedPtr<FJsonValue>>& MutableReferences = MutatedRoot->GetArrayField(TEXT("class_references"));
		Mutate(MutableReferences);
		FString MutatedJson;
		FAvidScriptBindingPackageModel MutatedPackage;
		FString Category;
		FString Source;
		TestFalse(Label,
			SerializeDescriptor(MutatedRoot, MutatedJson)
			&& FAvidScriptBindingDescriptorParser::Parse(MutatedJson, MutatedPackage, Category, Source));
		TestEqual(TEXT("Hostile class table reports descriptor contract failure"), Category, FString(TEXT("descriptor_contract_invalid")));
	};
	ParserRejectsMutation(TEXT("Class reference ordinal holes fail closed"), [](const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		Values[0]->AsObject()->SetNumberField(TEXT("ordinal"), 1);
	});
	ParserRejectsMutation(TEXT("Class reference stable-id drift fails closed"), [](const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		Values[0]->AsObject()->SetStringField(TEXT("stable_id"), FString::ChrN(64, TEXT('0')));
	});
	ParserRejectsMutation(TEXT("Empty class path fails closed"), [](const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		Values[0]->AsObject()->SetStringField(TEXT("class_path"), TEXT(""));
	});
	ParserRejectsMutation(TEXT("Unknown load policy fails closed"), [](const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		Values[0]->AsObject()->SetStringField(TEXT("load_policy"), TEXT("LazyMaybe"));
	});
	ParserRejectsMutation(TEXT("Duplicate class script names fail closed"), [](const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		Values[1]->AsObject()->SetStringField(
			TEXT("script_name"),
			Values[0]->AsObject()->GetStringField(TEXT("script_name")));
	});

	TSharedPtr<FJsonObject> HashTamperedRoot;
	TestTrue(TEXT("Hash tamper descriptor clone parses"), ParseDescriptor(FirstJson, HashTamperedRoot));
	HashTamperedRoot->GetArrayField(TEXT("class_references"))[0]->AsObject()->SetStringField(
		TEXT("script_name"),
		TEXT("HashTamperedClass"));
	FString HashTamperedJson;
	TestTrue(TEXT("Hash tamper descriptor serializes"), SerializeDescriptor(HashTamperedRoot, HashTamperedJson));
	TestTrue(TEXT("Hash tamper remains structurally valid"),
		FAvidScriptBindingDescriptorParser::Parse(HashTamperedJson, ParsedPackage, ErrorCategory, ErrorSource));
	TestFalse(TEXT("Runtime rejects a class table omitted from package hash"),
		FAvidScriptBindingPackage::LoadDescriptor(HashTamperedJson, RuntimePackage, LoadResult));
	TestEqual(TEXT("Class table hash mismatch has stable category"), LoadResult.ErrorCategory, FString(TEXT("binding_package_hash_mismatch")));

	const auto RuntimeRejectsClass = [this, &Functions](
		const TCHAR* Label,
		const FAvidScriptProjectBindingClassSpec& Reference,
		const TCHAR* ExpectedCategory)
	{
		FString Json;
		FAvidScriptBindingDescriptorGenerateResult GenerateResult;
		TestTrue(Label,
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
				TEXT("avidscript.project.invalid_class_ref"),
				Functions,
				{},
				{ Reference },
				Json,
				GenerateResult));
		TSharedPtr<const FAvidScriptBindingPackage> Package;
		FAvidScriptBindingPackageLoadResult Result;
		TestFalse(TEXT("Invalid class reference runtime load fails closed"),
			FAvidScriptBindingPackage::LoadDescriptor(Json, Package, Result));
		TestEqual(TEXT("Invalid class reference uses stable runtime category"), Result.ErrorCategory, FString(ExpectedCategory));
	};
	RuntimeRejectsClass(
		TEXT("Wrong inheritance descriptor generates for runtime validation"),
		{ TEXT("SceneComponentClass"), TEXT("/Script/Engine.SceneComponent"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") },
		TEXT("binding_class_inheritance_mismatch"));
	RuntimeRejectsClass(
		TEXT("Abstract Actor descriptor generates for runtime validation"),
		{ TEXT("ControllerClass"), TEXT("/Script/Engine.Controller"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") },
		TEXT("binding_class_not_spawnable"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV6ObjectTypePlanTest,
	"AvidScript.Editor.BindingDescriptor.V6ObjectTypePlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV6ObjectTypePlanTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.project.object_type_plan");
	Profile.SelfClassPath = TEXT("/Script/Engine.StaticMeshActor");
	Profile.ExplicitFunctions = {
		{ TEXT("/Script/Engine.Actor"), TEXT("GetActorScale3D") },
		{ TEXT("/Script/Engine.Actor"), TEXT("GetDistanceTo") },
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetRootComponent") }
	};
	const TArray<FAvidScriptProjectBindingClassSpec> ClassReferences = {
		{ TEXT("ProjectileClass"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") }
	};

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("Schema v6 descriptor generates from an explicit Actor self profile"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			ClassReferences,
			DescriptorJson,
			SelectionResult,
			GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("Schema v6 descriptor is valid JSON"), ParseDescriptor(DescriptorJson, Root))
		|| !Root.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Object type descriptor publishes schema v6"), Root->GetIntegerField(TEXT("schema_version")), 6);

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
		TEXT("Shared parser accepts the generated v6 object graph"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Package,
			ErrorCategory,
			ErrorSource)))
	{
		AddError(FString::Printf(TEXT("%s:%s"), *ErrorCategory, *ErrorSource));
		return false;
	}

	TMap<FString, const FAvidScriptBindingTypeModel*> GraphTypesById;
	TArray<const FAvidScriptBindingTypeModel*> GraphTypesByOrdinal;
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		if (Type.ObjectTypeOrdinal != INDEX_NONE)
		{
			GraphTypesById.Add(Type.StableId, &Type);
			if (GraphTypesByOrdinal.Num() <= Type.ObjectTypeOrdinal)
			{
				GraphTypesByOrdinal.SetNum(Type.ObjectTypeOrdinal + 1);
			}
			GraphTypesByOrdinal[Type.ObjectTypeOrdinal] = &Type;
		}
	}
	TestTrue(TEXT("V6 publishes a non-empty handle-capable graph"), !GraphTypesByOrdinal.IsEmpty());
	FString PreviousClassPath;
	for (int32 Ordinal = 0; Ordinal < GraphTypesByOrdinal.Num(); ++Ordinal)
	{
		const FAvidScriptBindingTypeModel* Type = GraphTypesByOrdinal[Ordinal];
		if (!TestNotNull(TEXT("Every object type ordinal is populated"), Type))
		{
			continue;
		}
		TestEqual(TEXT("Object type ordinal is dense"), Type->ObjectTypeOrdinal, Ordinal);
		TestTrue(
			TEXT("Object type ordinals follow canonical class path order"),
			PreviousClassPath.IsEmpty() || PreviousClassPath < Type->ClassPath);
		PreviousClassPath = Type->ClassPath;
		if (!Type->BaseTypeId.IsEmpty())
		{
			TestTrue(TEXT("Every base edge resolves inside the graph"), GraphTypesById.Contains(Type->BaseTypeId));
		}
	}

	const FAvidScriptBindingTypeModel* SelfType = GraphTypesById.FindRef(Package.SelfTypeId);
	if (TestNotNull(TEXT("Explicit self type resolves to a graph node"), SelfType))
	{
		TestEqual(
			TEXT("Explicit self keeps canonical class identity"),
			SelfType->ClassPath,
			FString(TEXT("/Script/Engine.StaticMeshActor")));
	}
	if (TestEqual(TEXT("V6 keeps one class reference"), Package.ClassReferences.Num(), 1))
	{
		const FAvidScriptBindingClassReferenceModel& Reference = Package.ClassReferences[0];
		const FAvidScriptBindingTypeModel* ResultType = GraphTypesById.FindRef(Reference.ResultTypeId);
		if (TestNotNull(TEXT("Class-reference result type resolves to the graph"), ResultType))
		{
			TestEqual(TEXT("Class-reference result matches its base class"), ResultType->ClassPath, Reference.BaseClassPath);
		}
	}

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (TestTrue(
		TEXT("Runtime loads immutable v6 object type plans"),
		FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, RuntimePackage, LoadResult))
		&& RuntimePackage.IsValid())
	{
		TestEqual(
			TEXT("Runtime object type count matches the descriptor graph"),
			RuntimePackage->GetObjectTypeCount(),
			GraphTypesByOrdinal.Num());
		for (int32 Ordinal = 0; Ordinal < GraphTypesByOrdinal.Num(); ++Ordinal)
		{
			UClass* ResolvedClass = nullptr;
			TestTrue(
				TEXT("Object type resolves by ordinal"),
				RuntimePackage->TryResolveObjectType(static_cast<uint32>(Ordinal), ResolvedClass));
			if (ResolvedClass != nullptr && GraphTypesByOrdinal[Ordinal] != nullptr)
			{
				TestEqual(
					TEXT("Resolved object type keeps canonical path"),
					ResolvedClass->GetPathName(),
					GraphTypesByOrdinal[Ordinal]->ClassPath);
			}
		}
		UClass* OutOfRangeClass = AActor::StaticClass();
		TestFalse(
			TEXT("Out-of-range object type ordinal fails closed"),
			RuntimePackage->TryResolveObjectType(static_cast<uint32>(GraphTypesByOrdinal.Num()), OutOfRangeClass));
		TestNull(TEXT("Failed object type resolution clears output"), OutOfRangeClass);
		TestEqual(
			TEXT("Expected self class is cached from the self type ordinal"),
			RuntimePackage->GetExpectedSelfClass(),
			AStaticMeshActor::StaticClass());
		TestEqual(
			TEXT("Graph and reflected owners reuse one class load per path"),
			RuntimePackage->GetInstrumentation().ClassLoadCount,
			static_cast<uint64>(GraphTypesByOrdinal.Num()));
	}

	const auto CloneRoot = [&Root]()
	{
		FString SourceJson;
		TSharedPtr<FJsonObject> Clone;
		SerializeDescriptor(Root, SourceJson);
		ParseDescriptor(SourceJson, Clone);
		return Clone;
	};
	const auto ParserRejectsRoot = [this](
		const TCHAR* Label,
		const TSharedPtr<FJsonObject>& MutatedRoot,
		const TCHAR* ExpectedSource)
	{
		FString MutatedJson;
		FAvidScriptBindingPackageModel MutatedPackage;
		FString Category;
		FString Source;
		TestFalse(
			Label,
			SerializeDescriptor(MutatedRoot, MutatedJson)
				&& FAvidScriptBindingDescriptorParser::Parse(
					MutatedJson,
					MutatedPackage,
					Category,
					Source));
		TestEqual(
			*FString::Printf(TEXT("%s uses the stable error category"), Label),
			Category,
			FString(TEXT("descriptor_contract_invalid")));
		if (ExpectedSource != nullptr)
		{
			TestEqual(
				*FString::Printf(TEXT("%s reaches the intended inner guard"), Label),
				Source,
				FString(ExpectedSource));
		}
	};
	const auto ParserRejectsMutation = [&CloneRoot, &ParserRejectsRoot](
		const TCHAR* Label,
		const TCHAR* ExpectedSource,
		const TFunctionRef<void(TSharedPtr<FJsonObject>&)>& Mutate)
	{
		TSharedPtr<FJsonObject> MutatedRoot = CloneRoot();
		Mutate(MutatedRoot);
		ParserRejectsRoot(Label, MutatedRoot, ExpectedSource);
	};

	ParserRejectsMutation(
		TEXT("Object type ordinal holes fail closed"),
		TEXT("types.object_type_ordinal"),
		[this](TSharedPtr<FJsonObject>& MutatedRoot)
		{
			int32 GraphTypeCount = 0;
			int32 MaximumOrdinal = INDEX_NONE;
			TSharedPtr<FJsonObject> MaximumOrdinalType;
			for (const TSharedPtr<FJsonValue>& Value : MutatedRoot->GetArrayField(TEXT("types")))
			{
				const TSharedPtr<FJsonObject> Type = Value->AsObject();
				const int32 Ordinal = Type->GetIntegerField(TEXT("object_type_ordinal"));
				if (Ordinal != INDEX_NONE)
				{
					++GraphTypeCount;
					if (Ordinal > MaximumOrdinal)
					{
						MaximumOrdinal = Ordinal;
						MaximumOrdinalType = Type;
					}
				}
			}
			TestEqual(
				TEXT("Generated graph starts with a dense maximum ordinal"),
				MaximumOrdinal,
				GraphTypeCount - 1);
			if (TestNotNull(TEXT("Ordinal-hole fixture finds the maximum node"), MaximumOrdinalType.Get()))
			{
				MaximumOrdinalType->SetNumberField(
					TEXT("object_type_ordinal"),
					MaximumOrdinal + 1);
			}
		});
	ParserRejectsMutation(
		TEXT("Missing object type IDs fail closed"),
		nullptr,
		[](TSharedPtr<FJsonObject>& MutatedRoot)
	{
		for (const TSharedPtr<FJsonValue>& Value : MutatedRoot->GetArrayField(TEXT("types")))
		{
			const TSharedPtr<FJsonObject> Type = Value->AsObject();
			if (Type->GetIntegerField(TEXT("object_type_ordinal")) != INDEX_NONE)
			{
				Type->RemoveField(TEXT("stable_id"));
				break;
			}
		}
	});
	ParserRejectsMutation(
		TEXT("Object type graph cycles fail closed"),
		TEXT("types.base_type_id"),
		[this](TSharedPtr<FJsonObject>& MutatedRoot)
		{
			TSharedPtr<FJsonObject> ActorType;
			TSharedPtr<FJsonObject> StaticMeshActorType;
			TSharedPtr<FJsonObject> ObjectType;
			int32 RootCount = 0;
			for (const TSharedPtr<FJsonValue>& Value : MutatedRoot->GetArrayField(TEXT("types")))
			{
				const TSharedPtr<FJsonObject> Type = Value->AsObject();
				if (Type->GetIntegerField(TEXT("object_type_ordinal")) == INDEX_NONE)
				{
					continue;
				}
				if (Type->GetStringField(TEXT("base_type_id")).IsEmpty())
				{
					++RootCount;
				}
				const FString ClassPath = Type->GetStringField(TEXT("class_path"));
				if (ClassPath == TEXT("/Script/Engine.Actor"))
				{
					ActorType = Type;
				}
				else if (ClassPath == TEXT("/Script/Engine.StaticMeshActor"))
				{
					StaticMeshActorType = Type;
				}
				else if (ClassPath == TEXT("/Script/CoreUObject.Object"))
				{
					ObjectType = Type;
				}
			}
			TestEqual(TEXT("Cycle fixture keeps exactly one graph root"), RootCount, 1);
			if (!TestNotNull(TEXT("Cycle fixture keeps the UObject root"), ObjectType.Get())
				|| !TestNotNull(TEXT("Cycle fixture finds Actor"), ActorType.Get())
				|| !TestNotNull(TEXT("Cycle fixture finds StaticMeshActor"), StaticMeshActorType.Get()))
			{
				return;
			}
			TestTrue(
				TEXT("Cycle fixture leaves UObject as the empty-base root"),
				ObjectType->GetStringField(TEXT("base_type_id")).IsEmpty());
			TestEqual(
				TEXT("Cycle fixture starts with StaticMeshActor pointing to Actor"),
				StaticMeshActorType->GetStringField(TEXT("base_type_id")),
				ActorType->GetStringField(TEXT("stable_id")));
			ActorType->SetStringField(
				TEXT("base_type_id"),
				StaticMeshActorType->GetStringField(TEXT("stable_id")));
		});
	ParserRejectsMutation(
		TEXT("Canonical object class path mismatches fail closed"),
		nullptr,
		[](TSharedPtr<FJsonObject>& MutatedRoot)
	{
		for (const TSharedPtr<FJsonValue>& Value : MutatedRoot->GetArrayField(TEXT("types")))
		{
			const TSharedPtr<FJsonObject> Type = Value->AsObject();
			if (Type->GetStringField(TEXT("class_path")) == TEXT("/Script/Engine.Actor"))
			{
				Type->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.Pawn"));
				break;
			}
		}
	});
	ParserRejectsMutation(
		TEXT("Class-reference result type mismatches fail closed"),
		TEXT("class_references.result_type_id"),
		[](TSharedPtr<FJsonObject>& MutatedRoot)
	{
		FString SceneComponentTypeId;
		for (const TSharedPtr<FJsonValue>& Value : MutatedRoot->GetArrayField(TEXT("types")))
		{
			const TSharedPtr<FJsonObject> Type = Value->AsObject();
			if (Type->GetStringField(TEXT("class_path")) == TEXT("/Script/Engine.SceneComponent"))
			{
				SceneComponentTypeId = Type->GetStringField(TEXT("stable_id"));
				break;
			}
		}
		MutatedRoot->GetArrayField(TEXT("class_references"))[0]->AsObject()->SetStringField(
			TEXT("result_type_id"),
			SceneComponentTypeId);
	});
	ParserRejectsMutation(
		TEXT("Mixed Actor packages require Self identity"),
		TEXT("self_type_id"),
		[](TSharedPtr<FJsonObject>& MutatedRoot)
	{
		MutatedRoot->SetStringField(TEXT("self_type_id"), TEXT(""));
	});

	FString InstanceOnlyJson;
	FAvidScriptBindingSelectionResolveResult InstanceOnlySelectionResult;
	FAvidScriptBindingDescriptorGenerateResult InstanceOnlyGenerateResult;
	const TArray<FAvidScriptProjectBindingClassSpec> NoClassReferences;
	if (TestTrue(
		TEXT("Instance-only descriptor generates without class references"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			NoClassReferences,
			InstanceOnlyJson,
			InstanceOnlySelectionResult,
			InstanceOnlyGenerateResult)))
	{
		TSharedPtr<FJsonObject> InstanceOnlyRoot;
		if (TestTrue(
				TEXT("Instance-only descriptor is valid JSON"),
				ParseDescriptor(InstanceOnlyJson, InstanceOnlyRoot)))
		{
			const TArray<TSharedPtr<FJsonValue>>& InstanceReferences =
				InstanceOnlyRoot->GetArrayField(TEXT("class_references"));
			const TArray<TSharedPtr<FJsonValue>>& InstanceBindings =
				InstanceOnlyRoot->GetArrayField(TEXT("bindings"));
			bool bAllBindingsUseInstanceReceivers = !InstanceBindings.IsEmpty();
			for (const TSharedPtr<FJsonValue>& Value : InstanceBindings)
			{
				bAllBindingsUseInstanceReceivers &=
					!Value->AsObject()->GetBoolField(TEXT("is_static"));
			}
			TestTrue(
				TEXT("Instance-only fixture contains no class-reference Self trigger"),
				InstanceReferences.IsEmpty());
			TestTrue(
				TEXT("Instance-only fixture requires only instance receivers"),
				bAllBindingsUseInstanceReceivers);
			InstanceOnlyRoot->SetStringField(TEXT("self_type_id"), TEXT(""));
			ParserRejectsRoot(
				TEXT("Instance receiver usage independently requires Self identity"),
				InstanceOnlyRoot,
				TEXT("self_type_id"));
		}
	}

	TSharedPtr<FJsonObject> WrongBaseRoot = CloneRoot();
	FString SceneComponentTypeId;
	TSharedPtr<FJsonObject> ActorTypeObject;
	for (const TSharedPtr<FJsonValue>& Value : WrongBaseRoot->GetArrayField(TEXT("types")))
	{
		const TSharedPtr<FJsonObject> Type = Value->AsObject();
		if (Type->GetStringField(TEXT("class_path")) == TEXT("/Script/Engine.SceneComponent"))
		{
			SceneComponentTypeId = Type->GetStringField(TEXT("stable_id"));
		}
		else if (Type->GetStringField(TEXT("class_path")) == TEXT("/Script/Engine.Actor"))
		{
			ActorTypeObject = Type;
		}
	}
	ActorTypeObject->SetStringField(TEXT("base_type_id"), SceneComponentTypeId);
	FString WrongBaseJson;
	SerializeDescriptor(WrongBaseRoot, WrongBaseJson);
	FAvidScriptBindingPackageModel WrongBaseModel;
	TestTrue(
		TEXT("Structurally complete wrong direct-super edge reaches package-load validation"),
		FAvidScriptBindingDescriptorParser::Parse(
			WrongBaseJson,
			WrongBaseModel,
			ErrorCategory,
			ErrorSource));
	WrongBaseRoot->SetStringField(
		TEXT("selection_hash"),
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(WrongBaseModel));
	WrongBaseModel.SelectionHash = WrongBaseRoot->GetStringField(TEXT("selection_hash"));
	WrongBaseRoot->SetStringField(
		TEXT("package_hash"),
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(WrongBaseModel));
	SerializeDescriptor(WrongBaseRoot, WrongBaseJson);
	TestFalse(
		TEXT("Package load rejects a reflected direct-super edge mismatch"),
		FAvidScriptBindingPackage::LoadDescriptor(WrongBaseJson, RuntimePackage, LoadResult));
	TestEqual(
		TEXT("Wrong direct-super edge has a stable load category"),
		LoadResult.ErrorCategory,
		FString(TEXT("binding_object_type_base_mismatch")));

	for (int32 LegacySchema = 2; LegacySchema <= 5; ++LegacySchema)
	{
		TSharedPtr<FJsonObject> LegacyRoot = CloneRoot();
		LegacyRoot->SetNumberField(TEXT("schema_version"), LegacySchema);
		LegacyRoot->RemoveField(TEXT("self_type_id"));
		for (const TSharedPtr<FJsonValue>& Value : LegacyRoot->GetArrayField(TEXT("types")))
		{
			const TSharedPtr<FJsonObject> Type = Value->AsObject();
			Type->RemoveField(TEXT("object_type_ordinal"));
			Type->RemoveField(TEXT("class_path"));
			Type->RemoveField(TEXT("base_type_id"));
		}
		for (const TSharedPtr<FJsonValue>& Value : LegacyRoot->GetArrayField(TEXT("class_references")))
		{
			Value->AsObject()->RemoveField(TEXT("result_type_id"));
		}
		if (LegacySchema < 5)
		{
			LegacyRoot->RemoveField(TEXT("class_references"));
		}
		if (LegacySchema <= 3)
		{
			for (const TSharedPtr<FJsonValue>& Value : LegacyRoot->GetArrayField(TEXT("bindings")))
			{
				const TSharedPtr<FJsonObject> Binding = Value->AsObject();
				Binding->SetStringField(TEXT("ue_function"), Binding->GetStringField(TEXT("ue_member")));
				Binding->RemoveField(TEXT("binding_kind"));
				Binding->RemoveField(TEXT("ue_member"));
			}
		}
		if (LegacySchema == 2)
		{
			for (const TSharedPtr<FJsonValue>& Value : LegacyRoot->GetArrayField(TEXT("bindings")))
			{
				Value->AsObject()->RemoveField(TEXT("reload_effect"));
			}
		}
		FString LegacyJson;
		FAvidScriptBindingPackageModel LegacyPackage;
		TestTrue(
			*FString::Printf(TEXT("Schema v%d parser remains compatible"), LegacySchema),
			SerializeDescriptor(LegacyRoot, LegacyJson)
				&& FAvidScriptBindingDescriptorParser::Parse(
					LegacyJson,
					LegacyPackage,
					ErrorCategory,
					ErrorSource));
		if (!LegacyPackage.Types.IsEmpty())
		{
			TestEqual(
				TEXT("Legacy type model keeps no object ordinal"),
				LegacyPackage.Types[0].ObjectTypeOrdinal,
				INDEX_NONE);
			TestTrue(TEXT("Legacy type model keeps no class path"), LegacyPackage.Types[0].ClassPath.IsEmpty());
			TestTrue(TEXT("Legacy type model keeps no base edge"), LegacyPackage.Types[0].BaseTypeId.IsEmpty());
		}
	}

	FString StaticOnlyJson;
	FAvidScriptBindingDescriptorGenerateResult StaticOnlyResult;
	TestTrue(
		TEXT("Pure static descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.static_only"),
			{ { TEXT("/Script/Engine.KismetMathLibrary"), TEXT("VLerp") } },
			StaticOnlyJson,
			StaticOnlyResult));
	FAvidScriptBindingPackageModel StaticOnlyPackage;
	if (TestTrue(
		TEXT("Pure static descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			StaticOnlyJson,
			StaticOnlyPackage,
			ErrorCategory,
			ErrorSource)))
	{
		TestTrue(TEXT("Pure static descriptor has no Self type"), StaticOnlyPackage.SelfTypeId.IsEmpty());
		const FAvidScriptBindingTypeModel* StaticOwnerType = StaticOnlyPackage.Types.FindByPredicate(
			[](const FAvidScriptBindingTypeModel& Type)
			{
				return Type.ClassPath.IsEmpty()
					&& Type.CanonicalType == TEXT("object:/Script/Engine.KismetMathLibrary");
			});
		if (TestNotNull(TEXT("Pure static owner type remains available to renderers"), StaticOwnerType))
		{
			TestEqual(TEXT("Pure static owner has no runtime object ordinal"), StaticOwnerType->ObjectTypeOrdinal, INDEX_NONE);
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
	FAvidScriptEditorBindingDescriptorFNameProjectionTest,
	"AvidScript.Editor.BindingDescriptor.FNameProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorFNameProjectionTest::RunTest(const FString& Parameters)
{
	const FAvidScriptReflectedFunctionSelection ActorHasTag{
		TEXT("/Script/Engine.Actor"),
		TEXT("ActorHasTag")
	};
	FString Json;
	FAvidScriptBindingDescriptorGenerateResult Result;
	TestTrue(
		TEXT("ActorHasTag descriptor generates with an FName input"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(TEXT("avidscript.engine.fname"), { ActorHasTag }, Json, Result));

	TSharedPtr<FJsonObject> Root;
	if (TestTrue(TEXT("ActorHasTag descriptor parses"), ParseDescriptor(Json, Root)) && Root.IsValid())
	{
		const TSharedPtr<FJsonObject> Binding = FindBinding(
			Root->GetArrayField(TEXT("bindings")),
			TEXT("/Script/Engine.Actor"),
			TEXT("ActorHasTag"));
		TestNotNull(TEXT("ActorHasTag binding is present"), Binding.Get());
		if (Binding.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>& BindingParameters = Binding->GetArrayField(TEXT("parameters"));
			TestEqual(TEXT("ActorHasTag has one FName parameter"), BindingParameters.Num(), 1);
			if (BindingParameters.Num() == 1)
			{
				const TSharedPtr<FJsonObject> Parameter = BindingParameters[0]->AsObject();
				TestEqual(TEXT("FName parameter direction is value"), Parameter->GetStringField(TEXT("direction")), FString(TEXT("value")));
				TestEqual(TEXT("FName canonical type is exact"), Parameter->GetStringField(TEXT("canonical_type")), FString(TEXT("name:fname")));
				TestEqual(TEXT("FName kind is exact"), Parameter->GetStringField(TEXT("kind")), FString(TEXT("name_utf8")));
				TestEqual(TEXT("FName C++ type is exact"), Parameter->GetStringField(TEXT("cpp_type")), FString(TEXT("FName")));
				const TArray<TSharedPtr<FJsonValue>>& ParameterAbiTypes = Parameter->GetArrayField(TEXT("abi_types"));
				TestEqual(TEXT("FName parameter ABI has one cell"), ParameterAbiTypes.Num(), 1);
				if (ParameterAbiTypes.Num() == 1)
				{
					TestEqual(TEXT("FName ABI is one data address"), ParameterAbiTypes[0]->AsString(), FString(TEXT("i")));
				}
			}
			TestEqual(TEXT("ActorHasTag ABI carries self, name address, and bool return address"),
				Binding->GetObjectField(TEXT("host_import"))->GetStringField(TEXT("signature")), FString(TEXT("(iiii)i")));
		}

		const TSharedPtr<FJsonValue>* FNameTypeValue = Root->GetArrayField(TEXT("types")).FindByPredicate(
			[](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid() && Value->AsObject()->GetStringField(TEXT("canonical_type")) == TEXT("name:fname");
			});
		TestNotNull(TEXT("FName type descriptor is present"), FNameTypeValue);
		if (FNameTypeValue != nullptr)
		{
			const TSharedPtr<FJsonObject> Type = (*FNameTypeValue)->AsObject();
			TestEqual(TEXT("FName type kind is exact"), Type->GetStringField(TEXT("kind")), FString(TEXT("name_utf8")));
			TestEqual(TEXT("FName type C++ name is exact"), Type->GetStringField(TEXT("cpp_type")), FString(TEXT("FName")));
			TestEqual(TEXT("FName type size is exact"), Type->GetIntegerField(TEXT("size")), 4);
			TestEqual(TEXT("FName type alignment is exact"), Type->GetIntegerField(TEXT("alignment")), 4);
			const TArray<TSharedPtr<FJsonValue>>& TypeAbiTypes = Type->GetArrayField(TEXT("abi_types"));
			TestEqual(TEXT("FName type ABI has one cell"), TypeAbiTypes.Num(), 1);
			if (TypeAbiTypes.Num() == 1)
			{
				TestEqual(TEXT("FName type ABI is exact"), TypeAbiTypes[0]->AsString(), FString(TEXT("i")));
			}
		}
	}

	const FString FixtureOwner = TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject");
	FString PropertyJson;
	FAvidScriptBindingDescriptorGenerateResult PropertyResult;
	TestFalse(
		TEXT("Readable FName property return fails closed"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.engine.fname.property"),
			{},
			{ { FixtureOwner, TEXT("ReadableFName") } },
			PropertyJson,
			PropertyResult));
	TestEqual(TEXT("Readable FName property has a type category"),
		PropertyResult.ErrorCategory, FString(TEXT("unsupported_property_type")));
	TestTrue(TEXT("Readable FName property identifies the return direction"),
		PropertyResult.ErrorSource.Contains(TEXT("FName:return")));

	for (const FString& FunctionName : { TEXT("ReturnFName"), TEXT("OutFName"), TEXT("RefFName") })
	{
		FString RejectedJson;
		FAvidScriptBindingDescriptorGenerateResult RejectedResult;
		TestFalse(
			TEXT("Unsupported FName direction fails closed: ") + FunctionName,
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.engine.fname.rejected"),
				{ { FixtureOwner, FName(*FunctionName) } },
				RejectedJson,
				RejectedResult));
		TestEqual(TEXT("Unsupported FName direction has a type category: ") + FunctionName,
			RejectedResult.ErrorCategory, FString(TEXT("unsupported_property_type")));
		const FString ExpectedDirection = FunctionName == TEXT("ReturnFName")
			? TEXT("return")
			: (FunctionName == TEXT("OutFName") ? TEXT("out") : TEXT("ref"));
		TestTrue(TEXT("Unsupported FName direction identifies the exact direction: ") + FunctionName,
			RejectedResult.ErrorSource.EndsWith(TEXT("FName:") + ExpectedDirection));
	}

	FString ConstRefJson;
	FAvidScriptBindingDescriptorGenerateResult ConstRefResult;
	TestTrue(
		TEXT("Const FName reference input remains supported"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.fname.constref"),
			{ { FixtureOwner, TEXT("ConstRefFName") } },
			ConstRefJson,
			ConstRefResult));
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
	FAvidScriptEditorBindingDescriptorGameplayProfileOwnerTest,
	"AvidScript.Editor.BindingDescriptor.GameplayProfileOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorGameplayProfileOwnerTest::RunTest(const FString& Parameters)
{
	FString DefaultJson;
	FAvidScriptBindingDescriptorGenerateResult DefaultResult;
	TestTrue(
		TEXT("Default descriptor generates for Actor stable-id comparison"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(DefaultJson, DefaultResult));

	const FAvidScriptBindingSelectionProfile Profile =
		FAvidScriptEditorBindingDescriptorGenerator::MakeEngineGameplayProfile();
	FString GameplayJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GameplayResult;
	TestTrue(
		TEXT("Gameplay profile descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			GameplayJson,
			SelectionResult,
			GameplayResult));

	TSharedPtr<FJsonObject> DefaultRoot;
	TSharedPtr<FJsonObject> GameplayRoot;
	if (TestTrue(TEXT("Default descriptor parses"), ParseDescriptor(DefaultJson, DefaultRoot))
		&& TestTrue(TEXT("Gameplay descriptor parses"), ParseDescriptor(GameplayJson, GameplayRoot))
		&& DefaultRoot.IsValid()
		&& GameplayRoot.IsValid())
	{
		const TSharedPtr<FJsonObject> DefaultActorLocation = FindBinding(
			DefaultRoot->GetArrayField(TEXT("bindings")),
			TEXT("/Script/Engine.Actor"),
			TEXT("K2_GetActorLocation"));
		const TSharedPtr<FJsonObject> GameplayActorLocation = FindBinding(
			GameplayRoot->GetArrayField(TEXT("bindings")),
			TEXT("/Script/Engine.Actor"),
			TEXT("K2_GetActorLocation"));
		TestNotNull(TEXT("Gameplay descriptor retains Actor location binding"), GameplayActorLocation.Get());
		if (DefaultActorLocation.IsValid() && GameplayActorLocation.IsValid())
		{
			TestEqual(
				TEXT("Actor location stable id is unchanged by gameplay profile expansion"),
				GameplayActorLocation->GetStringField(TEXT("stable_id")),
				DefaultActorLocation->GetStringField(TEXT("stable_id")));
		}

		const TSharedPtr<FJsonObject> PawnMovementInput = FindBinding(
			GameplayRoot->GetArrayField(TEXT("bindings")),
			TEXT("/Script/Engine.Pawn"),
			TEXT("AddMovementInput"));
		TestNotNull(TEXT("Gameplay descriptor publishes Pawn movement input"), PawnMovementInput.Get());
		TestTrue(
			TEXT("Pawn movement input receiver type is emitted"),
			GameplayRoot->GetArrayField(TEXT("types")).ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid()
					&& Value->AsObject()->GetStringField(TEXT("canonical_type")) == TEXT("object:/Script/Engine.Pawn");
			}));
	}

	FString Json;
	FAvidScriptBindingDescriptorGenerateResult Result;
	TestFalse(
		TEXT("Inherited function cannot be published under a Pawn facade"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.owner_mismatch"),
			{ { TEXT("/Script/Engine.Pawn"), TEXT("K2_GetActorLocation") } },
			Json,
			Result));
	TestEqual(TEXT("Inherited function reports owner mismatch"), Result.ErrorCategory, FString(TEXT("function_owner_mismatch")));
	TestTrue(TEXT("Inherited function produces no partial descriptor"), Json.IsEmpty());

	TestFalse(
		TEXT("Inherited property cannot be published under a Pawn facade"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.engine.property_owner_mismatch"),
			{},
			{ { TEXT("/Script/Engine.Pawn"), TEXT("CustomTimeDilation") } },
			Json,
			Result));
	TestEqual(TEXT("Inherited property reports owner mismatch"), Result.ErrorCategory, FString(TEXT("property_owner_mismatch")));
	TestTrue(TEXT("Inherited property produces no partial descriptor"), Json.IsEmpty());

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
