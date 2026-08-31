#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingDescriptorGenerator.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptBindingLatent.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorCSharpBindingEmitterTestTypes.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.h"
#include "BindingGeneration/AvidScriptEditorObjectTypeGraph.h"
#include "Algo/Reverse.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
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

TSharedPtr<FJsonObject> FindType(
	const TArray<TSharedPtr<FJsonValue>>& Types,
	const FString& CanonicalType)
{
	for (const TSharedPtr<FJsonValue>& Value : Types)
	{
		const TSharedPtr<FJsonObject> Type = Value.IsValid() ? Value->AsObject() : nullptr;
		if (Type.IsValid() && Type->GetStringField(TEXT("canonical_type")) == CanonicalType)
		{
			return Type;
		}
	}
	return nullptr;
}

bool IsDescriptorLowerHexSha256(const FString& Value)
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

class FAvidScriptDescriptorLatentProvider final
	: public IAvidScriptLatentCompletionProvider
{
public:
	FAvidScriptDescriptorLatentProvider(
		FString InFunctionPath,
		FString InPayloadTypeId)
		: FunctionPath(MoveTemp(InFunctionPath))
		, PayloadTypeId(MoveTemp(InPayloadTypeId))
	{
	}

	FString GetProviderId() const override
	{
		return TEXT("avidscript.test.score.v1");
	}

	FString GetFunctionPath() const override
	{
		return FunctionPath;
	}

	FString GetPayloadTypeId() const override
	{
		return PayloadTypeId;
	}

	bool ConsumePayload(
		UObject* CallbackTarget,
		int32 UUID,
		FAvidScriptBindingLatentCompletionPayload& OutPayload) override
	{
		OutPayload = {};
		return false;
	}

	void AbandonPayload(UObject* CallbackTarget, int32 UUID) override
	{
	}

private:
	FString FunctionPath;
	FString PayloadTypeId;
};
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
	FAvidScriptEditorBindingDescriptorGeneratedNativeTest,
	"AvidScript.Editor.BindingDescriptor.GeneratedNativeS1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorGeneratedNativeTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject");
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.generated.descriptor");
	FAvidScriptReflectedClassSelection Rule;
	Rule.OwnerClassPath = OwnerPath;
	Rule.IncludeFunctions.Add(TEXT("ReservedHandleNames"));
	Rule.GeneratedNativeFunctions.Add(TEXT("ReservedHandleNames"));
	Profile.Classes.Add(Rule);

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	TestTrue(
		TEXT("Eligible int32 pair generates an S1 descriptor"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			DescriptorJson,
			SelectionResult,
			GenerateResult));

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	TestTrue(
		TEXT("Generated descriptor satisfies parser contract"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Package,
			ErrorCategory,
			ErrorSource));
	if (Package.Bindings.Num() == 1)
	{
		const FAvidScriptBindingFunctionModel& Binding = Package.Bindings[0];
		TestEqual(
			TEXT("Generated dispatch mode is explicit"),
			Binding.DispatchMode,
			FString(TEXT("generated_native_s1")));
		TestEqual(
			TEXT("Generated shape is explicit"),
			Binding.GeneratedShape,
			FString(TEXT("i32_pair_to_i32")));
		TestEqual(
			TEXT("Generated pair uses the typed direct-return ABI"),
			Binding.HostImport.Signature,
			FString(TEXT("(iiii)i")));
		TestTrue(
			TEXT("Generated import is deterministic and dedicated"),
			Binding.HostImport.Name.StartsWith(TEXT("avid_s1_"))
				&& Binding.HostImport.Name
					== Binding.GeneratedImportName);
		TestEqual(
			TEXT("Semantic fallback ordinal is retained"),
			Binding.SemanticFallbackOrdinal,
			Binding.Ordinal);
	}
	else
	{
		AddError(TEXT("Expected exactly one generated S1 binding."));
	}
	FString PairReferenceSource;
	FString PairManifestJson;
	FAvidScriptCSharpBindingEmitResult PairEmitResult;
	TestTrue(
		TEXT("Generated pair survives C# facade rendering"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			PairReferenceSource,
			PairManifestJson,
			PairEmitResult));
	TestTrue(
		TEXT("Generated pair facade returns the typed host result directly"),
		PairReferenceSource.Contains(
			TEXT("return AvidScriptNative.Invoke0000(this.Slot, this.Generation"))
			&& !PairReferenceSource.Contains(TEXT("out __returnValue")));

	FAvidScriptBindingSelectionProfile VectorProfile;
	VectorProfile.PackageName = TEXT("avidscript.generated.vector.descriptor");
	FAvidScriptReflectedClassSelection VectorRule;
	VectorRule.OwnerClassPath = OwnerPath;
	VectorRule.IncludeFunctions.Add(TEXT("GeneratedVectorValue"));
	VectorRule.GeneratedNativeFunctions.Add(TEXT("GeneratedVectorValue"));
	VectorProfile.Classes.Add(MoveTemp(VectorRule));

	FString VectorDescriptorJson;
	FAvidScriptBindingSelectionResolveResult VectorSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult VectorGenerateResult;
	TestTrue(
		TEXT("Eligible const-ref FVector generates an S1 descriptor"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			VectorProfile,
			VectorDescriptorJson,
			VectorSelectionResult,
			VectorGenerateResult));
	FAvidScriptBindingPackageModel VectorPackage;
	TestTrue(
		TEXT("Generated const-ref vector descriptor satisfies parser contract"),
		FAvidScriptBindingDescriptorParser::Parse(
			VectorDescriptorJson,
			VectorPackage,
			ErrorCategory,
			ErrorSource));
	if (VectorPackage.Bindings.Num() == 1)
	{
		const FAvidScriptBindingFunctionModel& VectorBinding =
			VectorPackage.Bindings[0];
		TestEqual(
			TEXT("Const-ref vector uses generated S1"),
			VectorBinding.DispatchMode,
			FString(TEXT("generated_native_s1")));
		TestEqual(
			TEXT("Const-ref vector shape is explicit"),
			VectorBinding.GeneratedShape,
			FString(TEXT("vector_value")));
		TestEqual(
			TEXT("Const-ref input direction remains explicit"),
			VectorBinding.Parameters[0].Direction,
			FString(TEXT("const_ref")));
		TestEqual(
			TEXT("Generated vector uses one in-place guest buffer"),
			VectorBinding.HostImport.Signature,
			FString(TEXT("(iii)i")));
	}
	else
	{
		AddError(TEXT("Expected exactly one generated const-ref vector binding."));
	}
	FString VectorReferenceSource;
	FString VectorManifestJson;
	FAvidScriptCSharpBindingEmitResult VectorEmitResult;
	TestTrue(
		TEXT("Generated const-ref vector survives canonical reflection regeneration"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			VectorDescriptorJson,
			VectorReferenceSource,
			VectorManifestJson,
			VectorEmitResult));
	TestTrue(
		TEXT("Generated vector facade uses a single in-place buffer"),
		VectorReferenceSource.Contains(
			TEXT("ref FAvidScriptVectorValueBuffer value"))
			&& VectorReferenceSource.Contains(
				TEXT("return __vectorValue.Result;")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorGeneratedNativePropertyTest,
	"AvidScript.Editor.BindingDescriptor.GeneratedNativePropertyS1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorGeneratedNativePropertyTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		TEXT("/Script/AvidScriptEditor.AvidScriptBindingRuntimeProcessEventTestActor");
	const auto MakeProfile = [&OwnerPath](
		const FName PropertyName,
		const bool bWritable)
	{
		FAvidScriptBindingSelectionProfile Profile;
		Profile.PackageName = TEXT("avidscript.generated.property.descriptor");
		FAvidScriptReflectedClassSelection Rule;
		Rule.OwnerClassPath = OwnerPath;
		Rule.ExcludeFunctions.Add(TEXT("SetAlternateRoutedValue"));
		Rule.ExcludeFunctions.Add(TEXT("SetGeneratedSetterInt"));
		Rule.ExcludeFunctions.Add(TEXT("SetRoutedValue"));
		Rule.IncludeProperties.Add(PropertyName);
		Rule.GeneratedNativeProperties.Add(PropertyName);
		if (bWritable)
		{
			Rule.WritableProperties.Add(PropertyName);
		}
		Profile.Classes.Add(MoveTemp(Rule));
		return Profile;
	};
	const auto Generate = [](
		const FAvidScriptBindingSelectionProfile& Profile,
		FString& OutJson,
		FAvidScriptBindingDescriptorGenerateResult& OutResult)
	{
		FAvidScriptBindingSelectionResolveResult SelectionResult;
		return FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			OutJson,
			SelectionResult,
			OutResult);
	};

	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	TestTrue(
		TEXT("Public writable int32 property generates S1 getter and setter"),
		Generate(
			MakeProfile(TEXT("GeneratedPublicInt"), true),
			DescriptorJson,
			GenerateResult));

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	TestTrue(
		TEXT("Generated property descriptor satisfies parser contract"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Package,
			ErrorCategory,
			ErrorSource));
	TestEqual(
		TEXT("Generated writable property retains separate ordinals"),
		Package.Bindings.Num(),
		2);
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		TestEqual(
			TEXT("Property dispatch uses generated S1"),
			Binding.DispatchMode,
			FString(TEXT("generated_native_s1")));
		TestEqual(
			TEXT("Property shape is explicit"),
			Binding.GeneratedShape,
			Binding.BindingKind == TEXT("property_get")
				? FString(TEXT("property_i32_get"))
				: FString(TEXT("property_i32_set")));
		TestEqual(
			TEXT("Property receiver is self-bound"),
			Binding.GeneratedReceiverMode,
			FString(TEXT("self_bound")));
		TestEqual(
			TEXT("Property ABI is direction-specific"),
			Binding.HostImport.Signature,
			Binding.BindingKind == TEXT("property_get")
				? FString(TEXT("(ii)i"))
				: FString(TEXT("(iii)i")));
		TestEqual(
			TEXT("Semantic fallback retains the binding ordinal"),
			Binding.SemanticFallbackOrdinal,
			Binding.Ordinal);
	}
	if (Package.Bindings.Num() == 2)
	{
		TestNotEqual(
			TEXT("Getter and setter retain separate stable ids"),
			Package.Bindings[0].StableId,
			Package.Bindings[1].StableId);
		TestNotEqual(
			TEXT("Getter and setter retain separate imports"),
			Package.Bindings[0].GeneratedImportName,
			Package.Bindings[1].GeneratedImportName);
	}

	FString GetterOnlyJson;
	FAvidScriptBindingDescriptorGenerateResult GetterOnlyResult;
	TestTrue(
		TEXT("Generated authorization without writable authorization emits only a getter"),
		Generate(
			MakeProfile(TEXT("GeneratedPublicInt"), false),
			GetterOnlyJson,
			GetterOnlyResult));
	TestTrue(
		TEXT("Generated getter-only descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			GetterOnlyJson,
			Package,
			ErrorCategory,
			ErrorSource));
	if (TestEqual(
		TEXT("Generated authorization does not expand write capability"),
		Package.Bindings.Num(),
		1))
	{
		TestEqual(
			TEXT("The sole generated property binding is a getter"),
			Package.Bindings[0].BindingKind,
			FString(TEXT("property_get")));
		TestEqual(
			TEXT("Getter-only authorization uses the split getter shape"),
			Package.Bindings[0].GeneratedShape,
			FString(TEXT("property_i32_get")));
		TestEqual(
			TEXT("Getter-only authorization returns the native int directly"),
			Package.Bindings[0].HostImport.Signature,
			FString(TEXT("(ii)i")));
	}

	FAvidScriptBindingSelectionProfile SemanticProfile =
		MakeProfile(TEXT("GeneratedPublicInt"), true);
	SemanticProfile.Classes[0].GeneratedNativeProperties.Empty();
	FString SemanticJson;
	FAvidScriptBindingDescriptorGenerateResult SemanticResult;
	TestTrue(
		TEXT("Semantic property baseline still generates"),
		Generate(SemanticProfile, SemanticJson, SemanticResult));
	TestNotEqual(
		TEXT("Generated property authorization changes package hash"),
		GenerateResult.PackageHash,
		SemanticResult.PackageHash);

	TSharedPtr<FJsonObject> TamperedRoot;
	if (!TestTrue(
		TEXT("Generated property descriptor is readable for tamper test"),
		ParseDescriptor(DescriptorJson, TamperedRoot)))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* TamperedBindings = nullptr;
	if (!TestTrue(
		TEXT("Generated property tamper target exists"),
		TamperedRoot->TryGetArrayField(TEXT("bindings"), TamperedBindings)
			&& TamperedBindings != nullptr))
	{
		return false;
	}
	bool bTamperedGeneratedShape = false;
	for (const TSharedPtr<FJsonValue>& Value : *TamperedBindings)
	{
		const TSharedPtr<FJsonObject> Binding =
			Value.IsValid() ? Value->AsObject() : nullptr;
		if (Binding.IsValid()
			&& Binding->GetStringField(TEXT("dispatch_mode"))
				== TEXT("generated_native_s1"))
		{
			Binding->SetStringField(TEXT("generated_shape"), TEXT("vector_value"));
			bTamperedGeneratedShape = true;
			break;
		}
	}
	if (!TestTrue(
		TEXT("Generated property shape is structurally tampered"),
		bTamperedGeneratedShape))
	{
		return false;
	}
	FString TamperedJson;
	if (!TestTrue(
		TEXT("Tampered generated property descriptor serializes"),
		SerializeDescriptor(TamperedRoot, TamperedJson)))
	{
		return false;
	}
	TestFalse(
		TEXT("Generated property shape tamper fails closed"),
		FAvidScriptBindingDescriptorParser::Parse(
			TamperedJson,
			Package,
			ErrorCategory,
			ErrorSource));

	const struct
	{
		FName PropertyName;
		bool bWritable;
		const TCHAR* ExpectedCategory;
	} Rejections[] = {
		{ TEXT("GeneratedPrivateInt"), false, TEXT("generated_native_property_not_public") },
		{ TEXT("GeneratedPublicFloat"), false, TEXT("generated_native_property_type_unsupported") },
		{ TEXT("GeneratedSetterInt"), true, TEXT("generated_native_property_accessor_unsupported") }
	};
	for (const auto& Rejection : Rejections)
	{
		FAvidScriptBindingDescriptorGenerateResult RejectedResult;
		TestFalse(
			*FString::Printf(
				TEXT("%s generated property is rejected"),
				*Rejection.PropertyName.ToString()),
			Generate(
				MakeProfile(Rejection.PropertyName, Rejection.bWritable),
				DescriptorJson,
				RejectedResult));
		TestEqual(
			TEXT("Generated property rejection category is stable"),
			RejectedResult.ErrorCategory,
			FString(Rejection.ExpectedCategory));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorLatentV12Test,
	"AvidScript.Editor.BindingDescriptor.LatentV12",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorLatentV12Test::RunTest(
	const FString& Parameters)
{
	const TArray<FAvidScriptReflectedFunctionSelection> Selections = {
		{
			TEXT("/Script/AvidScriptEditor.AvidScriptEditorLatentFunctionLibraryTestObject"),
			TEXT("WaitForFlag")
		},
		{
			TEXT("/Script/AvidScriptEditor.AvidScriptEditorLatentFunctionLibraryTestObject"),
			TEXT("WaitForMode")
		},
		{
			TEXT("/Script/AvidScriptEditor.AvidScriptEditorLatentFunctionLibraryTestObject"),
			TEXT("WaitForTarget")
		},
		{
			TEXT("/Script/AvidScriptEditor.AvidScriptEditorLatentFunctionLibraryTestObject"),
			TEXT("WaitForLocation")
		},
		{
			TEXT("/Script/AvidScriptEditor.AvidScriptEditorLatentFunctionLibraryTestObject"),
			TEXT("WaitForSettings")
		},
		{ TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("Delay") },
		{ TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("DelayUntilNextFrame") }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Generic reflected latent descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.test.latent_v12"),
				Selections,
				DescriptorJson,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ") + GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("Latent descriptor parses through the shared v12 contract"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Package,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorSource);
		return false;
	}
	TSharedPtr<FJsonObject> TamperedRoot;
	TestTrue(
		TEXT("Latent descriptor JSON can be cloned for tamper validation"),
		ParseDescriptor(DescriptorJson, TamperedRoot));
	if (TamperedRoot.IsValid())
	{
		TamperedRoot->GetArrayField(TEXT("bindings"))[0]
			->AsObject()
			->SetStringField(
				TEXT("latent_info_parameter"),
				TEXT("WrongLatentInfo"));
	}
	FString TamperedDescriptor;
	TestTrue(
		TEXT("Tampered latent descriptor JSON serializes"),
		SerializeDescriptor(TamperedRoot, TamperedDescriptor));
	FAvidScriptBindingPackageModel TamperedPackage;
	FString TamperedCategory;
	FString TamperedSource;
	TestTrue(
		TEXT("Structural parser accepts the well-formed tampered descriptor"),
		FAvidScriptBindingDescriptorParser::Parse(
			TamperedDescriptor,
			TamperedPackage,
			TamperedCategory,
			TamperedSource));
	TSharedPtr<const FAvidScriptBindingPackage> TamperedRuntimePackage;
	FAvidScriptBindingPackageLoadResult TamperedLoadResult;
	TestFalse(
		TEXT("Runtime rejects hidden latent identity drift against active reflection"),
		FAvidScriptBindingPackage::LoadDescriptor(
			TamperedDescriptor,
			TamperedRuntimePackage,
			TamperedLoadResult));
	TestEqual(TEXT("Latent package alone raises schema to v12"), Package.SchemaVersion, 12);
	TestEqual(TEXT("All reflected latent producers are published"), Package.Bindings.Num(), 7);
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		TestEqual(
			TEXT("Latent dispatch remains generic ProcessEvent"),
			Binding.DispatchMode,
			FString(TEXT("latent_process_event")));
		TestEqual(
			TEXT("Latent info metadata is frozen"),
			Binding.LatentInfoParameter,
			FString(TEXT("LatentInfo")));
		TestEqual(
			TEXT("World context metadata is frozen"),
			Binding.WorldContextParameter,
			FString(TEXT("WorldContextObject")));
		TestTrue(
			TEXT("C# latent surface uses the Async suffix"),
			Binding.ScriptName.EndsWith(TEXT("Async")));
		TestEqual(
			TEXT("Latent producer has continuation reload semantics"),
			Binding.ReloadEffect,
			EAvidScriptBindingReloadEffect::ContinuationProducer);
		TestEqual(
			TEXT("Completion-only latent has no reflected return value"),
			Binding.ReturnValue.CanonicalType,
			FString(TEXT("void")));
		TestFalse(
			TEXT("Hidden latent info is absent from public parameters"),
			Binding.Parameters.ContainsByPredicate(
				[](const FAvidScriptBindingValueModel& Parameter)
				{
					return Parameter.Name == TEXT("LatentInfo")
						|| Parameter.Name == TEXT("WorldContextObject");
				}));
	}
	const FAvidScriptBindingFunctionModel* Delay = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("Delay");
		});
	const FAvidScriptBindingFunctionModel* NextFrame = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("DelayUntilNextFrame");
		});
	const FAvidScriptBindingFunctionModel* WaitForFlag = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("WaitForFlag");
		});
	const FAvidScriptBindingFunctionModel* WaitForMode = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("WaitForMode");
		});
	const FAvidScriptBindingFunctionModel* WaitForTarget = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("WaitForTarget");
		});
	const FAvidScriptBindingFunctionModel* WaitForLocation = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("WaitForLocation");
		});
	const FAvidScriptBindingFunctionModel* WaitForSettings = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("WaitForSettings");
		});
	if (TestNotNull(TEXT("Delay binding resolves"), Delay))
	{
		TestEqual(TEXT("Delay ABI appends callback and returns token"), Delay->HostImport.Signature, FString(TEXT("(fi)I")));
	}
	if (TestNotNull(TEXT("Next-frame binding resolves"), NextFrame))
	{
		TestEqual(TEXT("Next-frame ABI is callback to token"), NextFrame->HostImport.Signature, FString(TEXT("(i)I")));
	}
	if (TestNotNull(TEXT("Boolean latent binding resolves"), WaitForFlag))
	{
		TestEqual(TEXT("Boolean latent ABI stores bool as i32"), WaitForFlag->HostImport.Signature, FString(TEXT("(ii)I")));
		TestEqual(TEXT("Boolean latent exposes one public parameter"), WaitForFlag->Parameters.Num(), 1);
		if (WaitForFlag->Parameters.Num() == 1)
		{
			TestEqual(
				TEXT("Boolean latent parameter keeps public bool identity"),
				WaitForFlag->Parameters[0].CanonicalType,
				FString(TEXT("scalar:bool")));
		}
	}
	if (TestNotNull(TEXT("Enum latent binding resolves"), WaitForMode))
	{
		TestEqual(TEXT("Enum latent ABI stores enum as i32"), WaitForMode->HostImport.Signature, FString(TEXT("(ii)I")));
		TestEqual(TEXT("Enum latent exposes one public parameter"), WaitForMode->Parameters.Num(), 1);
		if (WaitForMode->Parameters.Num() == 1)
		{
			TestEqual(TEXT("Enum latent parameter keeps enum kind"), WaitForMode->Parameters[0].Kind, FString(TEXT("enum")));
			TestTrue(TEXT("Enum latent parameter keeps its default"), WaitForMode->Parameters[0].bHasDefault);
			TestEqual(TEXT("Enum latent parameter default is canonical"), WaitForMode->Parameters[0].DefaultValue, FString(TEXT("Primary")));
		}
	}
	if (TestNotNull(TEXT("Object latent binding resolves"), WaitForTarget))
	{
		TestEqual(TEXT("Object latent ABI flattens capability cells"), WaitForTarget->HostImport.Signature, FString(TEXT("(iii)I")));
		TestEqual(TEXT("Object latent exposes one typed parameter"), WaitForTarget->Parameters.Num(), 1);
	}
	if (TestNotNull(TEXT("Vector latent binding resolves"), WaitForLocation))
	{
		TestEqual(TEXT("Vector latent ABI flattens components"), WaitForLocation->HostImport.Signature, FString(TEXT("(fffi)I")));
		TestEqual(TEXT("Vector latent exposes one typed parameter"), WaitForLocation->Parameters.Num(), 1);
	}
	if (TestNotNull(TEXT("Struct-wire latent binding resolves"), WaitForSettings))
	{
		TestEqual(TEXT("Struct-wire latent ABI passes one address"), WaitForSettings->HostImport.Signature, FString(TEXT("(ii)I")));
		TestEqual(TEXT("Struct-wire latent exposes one typed parameter"), WaitForSettings->Parameters.Num(), 1);
	}

	FString ReferenceSource;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	const bool bEmitted = FAvidScriptEditorCSharpBindingEmitter::Emit(
		DescriptorJson,
		ReferenceSource,
		ManifestJson,
		EmitResult);
	if (!TestTrue(
			TEXT("Latent descriptor emits the generated C# facade"),
			bEmitted))
	{
		AddError(EmitResult.ErrorCategory + TEXT(": ")
			+ EmitResult.ErrorSource + TEXT(" | ")
			+ EmitResult.ErrorMessage);
		return false;
	}
	else
	{
		TestTrue(
			TEXT("Facade publishes a generated latent marker"),
			ReferenceSource.Contains(TEXT("[AvidLatent(\"avidscript\", \"avid_ue_")));
		TestTrue(
			TEXT("Delay is exposed as an awaitable method"),
			ReferenceSource.Contains(TEXT("public static AvidDelayAwaitable DelayAsync(float Duration) => default;")));
		TestTrue(
			TEXT("Boolean latent is exposed as a typed awaitable method"),
			ReferenceSource.Contains(TEXT("public static AvidDelayAwaitable WaitForFlagAsync(bool bExpected) => default;")));
		TestTrue(
			TEXT("Boolean latent native import uses i32 storage"),
			ReferenceSource.Contains(TEXT("int p0, int callbackId")));
		TestTrue(
			TEXT("Enum latent is exposed with a typed default"),
			ReferenceSource.Contains(TEXT("public static AvidDelayAwaitable WaitForModeAsync(EAvidScriptCSharpEmitterTestMode Mode = EAvidScriptCSharpEmitterTestMode.Primary) => default;")));
		TestTrue(
			TEXT("Object latent is exposed with a typed capability"),
			ReferenceSource.Contains(TEXT("public static AvidDelayAwaitable WaitForTargetAsync(UObject Target) => default;")));
		TestTrue(
			TEXT("Object latent native import flattens capability cells"),
			ReferenceSource.Contains(TEXT("int p0Slot, int p0Generation, int callbackId")));
		TestTrue(
			TEXT("Vector latent is exposed with a typed value"),
			ReferenceSource.Contains(TEXT("public static AvidDelayAwaitable WaitForLocationAsync(FVector Location) => default;")));
		TestTrue(
			TEXT("Vector latent native import flattens components"),
			ReferenceSource.Contains(TEXT("float p0X, float p0Y, float p0Z, int callbackId")));
		TestTrue(
			TEXT("Struct-wire latent is exposed with a typed value"),
			ReferenceSource.Contains(TEXT("public static AvidDelayAwaitable WaitForSettingsAsync(FAvidScriptStructWireRootTestType Settings) => default;")));
		TestTrue(
			TEXT("Struct-wire latent native import uses an address"),
			ReferenceSource.Contains(TEXT("in FAvidScriptStructWireRootTestType p0_Settings, int callbackId")));
		TestTrue(
			TEXT("Native latent import returns an i64 token"),
			ReferenceSource.Contains(TEXT("internal static extern long Invoke")));
	}

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestTrue(
		TEXT("Runtime independently validates the active latent reflection snapshot"),
		FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			RuntimePackage,
			LoadResult));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorLatentV14Test,
	"AvidScript.Editor.BindingDescriptor.LatentV14",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorLatentV14Test::RunTest(
	const FString& Parameters)
{
	UFunction* const Function =
		UAvidScriptEditorLatentFunctionLibraryTestObject::StaticClass()
			->FindFunctionByName(TEXT("WaitForScore"));
	if (!TestNotNull(TEXT("Provider latent function resolves"), Function))
	{
		return false;
	}
	const FString PayloadTypeId =
		FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
			TEXT("scalar:i32"),
			{});
	const TSharedRef<FAvidScriptDescriptorLatentProvider> Provider =
		MakeShared<FAvidScriptDescriptorLatentProvider>(
			Function->GetPathName(),
			PayloadTypeId);
	FString RegistryError;
	bool bRegistered =
		FAvidScriptLatentCompletionProviderRegistry::Register(
			Provider,
			RegistryError);
	if (!TestTrue(TEXT("Explicit completion provider registers"), bRegistered))
	{
		AddError(RegistryError);
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (bRegistered)
		{
			FAvidScriptLatentCompletionProviderRegistry::Unregister(Provider);
		}
	};

	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("Registered provider raises its latent descriptor to v14"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.test.latent_v13"),
			{
				{
					TEXT("/Script/AvidScriptEditor.AvidScriptEditorLatentFunctionLibraryTestObject"),
					TEXT("WaitForScore")
				}
			},
			DescriptorJson,
			GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
		TEXT("Provider descriptor parses through schema v14"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Package,
			ParseCategory,
			ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	TestEqual(TEXT("Provider latent package uses schema v14"), Package.SchemaVersion, 14);
	TestEqual(TEXT("Provider latent package has one binding"), Package.Bindings.Num(), 1);
	if (Package.Bindings.Num() == 1)
	{
		const FAvidScriptBindingLatentCompletionModel& Completion =
			Package.Bindings[0].Completion;
		TestEqual(TEXT("Completion mode is explicit provider"), Completion.Mode, FString(TEXT("provider")));
		TestEqual(TEXT("Provider identity is frozen"), Completion.ProviderId, Provider->GetProviderId());
		TestEqual(TEXT("Payload type identity is frozen"), Completion.PayloadTypeId, PayloadTypeId);
		TestEqual(TEXT("Cancellation policy is frozen"), Completion.StatusPolicy, FString(TEXT("resume_outcome_on_cancel")));
		TestTrue(TEXT("Provider latent call is cancellable"), Completion.bCancellable);
	}

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestTrue(
		TEXT("Runtime validates provider, UFunction, and payload identities"),
		FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			RuntimePackage,
			LoadResult));
	if (RuntimePackage.IsValid() && Package.Bindings.Num() == 1)
	{
		const FAvidScriptBindingTypeModel* FrozenResultType = nullptr;
		TestTrue(
			TEXT("Runtime freezes the provider result type at the binding ordinal"),
			RuntimePackage->TryGetLatentCompletionResultType(
				static_cast<uint32>(Package.Bindings[0].Ordinal),
				FrozenResultType));
		TestTrue(
			TEXT("Frozen provider result type is exact"),
			FrozenResultType != nullptr
				&& FrozenResultType->StableId == PayloadTypeId
				&& FrozenResultType->CanonicalType == TEXT("scalar:i32"));
	}

	FString ReferenceSource;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(
		TEXT("C9 exposes the provider result through the generated Guest facade"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			ReferenceSource,
			ManifestJson,
			EmitResult));
	TestTrue(
		TEXT("Provider facade returns a typed outcome awaitable"),
		ReferenceSource.Contains(
			TEXT("public static AvidOutcomeAwaitable<int> WaitForScoreAsync(int Score) => default;")));
	TestTrue(
		TEXT("Provider facade freezes ordinal and payload identity in compiler metadata"),
		ReferenceSource.Contains(
			TEXT("[AvidLatent(\"avidscript\", \"avid_ue_"))
			&& ReferenceSource.Contains(
				FString::Printf(TEXT(", 0, \"%s\")]"), *PayloadTypeId)));
	TestTrue(
		TEXT("Reference surface includes typed outcomes and one bulk result import"),
		ReferenceSource.Contains(TEXT("public readonly struct AvidOutcome<T>"))
			&& ReferenceSource.Contains(TEXT("public bool Cancelled => StatusValue == AvidContinuationStatus.Cancelled;"))
			&& ReferenceSource.Contains(TEXT("EntryPoint = \"continuation_result_read\""))
			&& ReferenceSource.Contains(
				TEXT("ContinuationResultRead(int bindingOrdinal, int resultSlot, int resultGeneration, int outputAddress, int byteCount)")));

	TestTrue(
		TEXT("Provider unregisters cleanly"),
		FAvidScriptLatentCompletionProviderRegistry::Unregister(Provider));
	bRegistered = false;
	TSharedPtr<const FAvidScriptBindingPackage> MissingProviderPackage;
	FAvidScriptBindingPackageLoadResult MissingProviderResult;
	TestFalse(
		TEXT("Runtime rejects a descriptor after provider removal"),
		FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			MissingProviderPackage,
			MissingProviderResult));
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
	TestFalse(
		TEXT("Schema v7 canonical bytes do not expose the v8 setter function field"),
		CanonicalJson.Contains(TEXT("\"ue_function\"")));

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
	TestTrue(TEXT("Package hash is a complete SHA-256"), IsDescriptorLowerHexSha256(FirstResult.PackageHash));
	TestTrue(TEXT("Selection hash is a complete SHA-256"), IsDescriptorLowerHexSha256(FirstResult.SelectionHash));

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
		TestTrue(TEXT("Binding stable id is SHA-256"), IsDescriptorLowerHexSha256(Binding->GetStringField(TEXT("stable_id"))));
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
	FAvidScriptEditorBindingDescriptorV8PropertySetTest,
	"AvidScript.Editor.BindingDescriptor.V8PropertySet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV8PropertySetTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation"), true }
	};
	FString Json;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Writable reflected property generates a descriptor"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
				TEXT("avidscript.engine.property_set"),
				{},
				Properties,
				Json,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Writable property publishes getter and setter"), GenerateResult.BindingCount, 2);

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("Shared descriptor parser accepts schema v8 property bindings"),
			FAvidScriptBindingDescriptorParser::Parse(
				Json,
				Package,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(":") + ErrorSource);
		return false;
	}

	const auto ParserRejectsWithSource = [this, &Json](
		const TCHAR* Label,
		const TCHAR* ExpectedSource,
		const TFunctionRef<void(TSharedPtr<FJsonObject>&)>& Mutate)
	{
		TSharedPtr<FJsonObject> Root;
		FString MutatedJson;
		FAvidScriptBindingPackageModel MutatedPackage;
		FString Category;
		FString Source;
		const bool bParsed = ParseDescriptor(Json, Root);
		if (bParsed)
		{
			Mutate(Root);
		}
		const bool bRejected = bParsed
			&& SerializeDescriptor(Root, MutatedJson)
			&& !FAvidScriptBindingDescriptorParser::Parse(
				MutatedJson,
				MutatedPackage,
				Category,
				Source);
		TestTrue(Label, bRejected);
		TestEqual(
			TEXT("Invalid schema v8 descriptor uses the stable category"),
			Category,
			FString(TEXT("descriptor_contract_invalid")));
		TestEqual(
			TEXT("Invalid schema v8 descriptor identifies its source"),
			Source,
			FString(ExpectedSource));
	};
	ParserRejectsWithSource(
		TEXT("Schema v22 above the current maximum identifies its header field"),
		TEXT("schema_version"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->SetNumberField(TEXT("schema_version"), 22);
		});
	ParserRejectsWithSource(
		TEXT("Malformed package hash identifies its header field"),
		TEXT("package_hash"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->SetStringField(TEXT("package_hash"), TEXT("invalid"));
		});
	ParserRejectsWithSource(
		TEXT("Malformed type identity identifies its array index"),
		TEXT("types[0]"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->GetArrayField(TEXT("types"))[0]
				->AsObject()
				->SetStringField(TEXT("stable_id"), TEXT("invalid"));
		});
	ParserRejectsWithSource(
		TEXT("Malformed binding identity identifies its array index"),
		TEXT("bindings[0]"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->GetArrayField(TEXT("bindings"))[0]
				->AsObject()
				->SetStringField(TEXT("stable_id"), TEXT("invalid"));
		});
	TestEqual(TEXT("Writable descriptor uses schema v8"), Package.SchemaVersion, 8);
	const FAvidScriptBindingFunctionModel* Getter = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_get")
				&& Binding.UeMember == TEXT("CustomTimeDilation");
		});
	const FAvidScriptBindingFunctionModel* Setter = Package.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_set")
				&& Binding.UeMember == TEXT("CustomTimeDilation");
		});
	if (TestNotNull(TEXT("Schema v8 retains the property getter"), Getter))
	{
		TestEqual(TEXT("Getter remains read-only"), Getter->WritePolicy, FString(TEXT("none")));
	}
	if (TestNotNull(TEXT("Schema v8 retains the property setter"), Setter))
	{
		TestEqual(TEXT("Setter uses direct write policy"), Setter->WritePolicy, FString(TEXT("direct")));
		TestEqual(TEXT("Setter uses cached property dispatch"), Setter->DispatchMode, FString(TEXT("cached_property_set")));
		TestTrue(TEXT("Direct property setter has no BlueprintSetter UFunction"), Setter->UeFunction.IsEmpty());
		TestEqual(TEXT("Setter carries one value parameter"), Setter->Parameters.Num(), 1);
		TestEqual(TEXT("Setter ABI carries owner handle and float"), Setter->HostImport.Signature, FString(TEXT("(iif)i")));
		TestEqual(
			TEXT("Setter declares transactional property effect"),
			Setter->ReloadEffect,
			EAvidScriptBindingReloadEffect::ReflectedProperty);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorNativeDirectAuthorizationTest,
	"AvidScript.Editor.BindingDescriptor.NativeDirectAuthorization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorNativeDirectAuthorizationTest::RunTest(
	const FString& Parameters)
{
	const auto MakeProfile = [](const bool bAuthorizeNativeDirect, const bool bWritable)
	{
		FAvidScriptBindingSelectionProfile Profile;
		Profile.PackageName = TEXT("avidscript.engine.native_direct_descriptor");
		FAvidScriptReflectedClassSelection ActorRule;
		ActorRule.OwnerClassPath = TEXT("/Script/Engine.Actor");
		ActorRule.IncludeFunctions = { TEXT("K2_GetActorLocation") };
		if (bAuthorizeNativeDirect)
		{
			ActorRule.NativeDirectFunctions = { TEXT("K2_GetActorLocation") };
		}
		if (bWritable)
		{
			ActorRule.WritableProperties = { TEXT("CustomTimeDilation") };
		}
		Profile.Classes.Add(MoveTemp(ActorRule));
		return Profile;
	};
	const auto GenerateAndParse = [this](
		const FAvidScriptBindingSelectionProfile& Profile,
		FString& OutJson,
		FAvidScriptBindingPackageModel& OutPackage)
	{
		FAvidScriptBindingSelectionResolveResult SelectionResult;
		FAvidScriptBindingDescriptorGenerateResult GenerateResult;
		if (!TestTrue(
				TEXT("Native-direct descriptor fixture generates"),
				FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
					Profile,
					OutJson,
					SelectionResult,
					GenerateResult)))
		{
			AddError(GenerateResult.ErrorMessage);
			return false;
		}
		FString ErrorCategory;
		FString ErrorSource;
		if (!TestTrue(
				TEXT("Native-direct descriptor fixture parses"),
				FAvidScriptBindingDescriptorParser::Parse(
					OutJson,
					OutPackage,
					ErrorCategory,
					ErrorSource)))
		{
			AddError(ErrorCategory + TEXT(":") + ErrorSource);
			return false;
		}
		return true;
	};
	const auto FindModelBinding = [](
		const FAvidScriptBindingPackageModel& Package,
		const TCHAR* BindingKind,
		const TCHAR* UeMember)
	{
		return Package.Bindings.FindByPredicate(
			[BindingKind, UeMember](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.BindingKind == BindingKind
					&& Binding.UeMember == UeMember;
			});
	};

	FString CachedJson;
	FAvidScriptBindingPackageModel CachedPackage;
	if (!GenerateAndParse(MakeProfile(false, true), CachedJson, CachedPackage))
	{
		return false;
	}
	FString DirectJson;
	FAvidScriptBindingPackageModel DirectPackage;
	if (!GenerateAndParse(MakeProfile(true, true), DirectJson, DirectPackage))
	{
		return false;
	}
	TestEqual(TEXT("Existing writable profile remains descriptor schema v8"), CachedPackage.SchemaVersion, 8);
	TestEqual(TEXT("Native-direct authorization selects descriptor schema v8"), DirectPackage.SchemaVersion, 8);

	const FAvidScriptBindingFunctionModel* CachedFunction =
		FindModelBinding(CachedPackage, TEXT("function"), TEXT("K2_GetActorLocation"));
	const FAvidScriptBindingFunctionModel* DirectFunction =
		FindModelBinding(DirectPackage, TEXT("function"), TEXT("K2_GetActorLocation"));
	if (TestNotNull(TEXT("Cached descriptor retains the selected function"), CachedFunction)
		&& TestNotNull(TEXT("Direct descriptor retains the selected function"), DirectFunction))
	{
		TestEqual(
			TEXT("Unspecified function remains cached ProcessEvent"),
			CachedFunction->DispatchMode,
			FString(TEXT("cached_process_event")));
		TestEqual(
			TEXT("Explicit authorization publishes qualified native direct"),
			DirectFunction->DispatchMode,
			FString(TEXT("qualified_native_direct")));
		TestEqual(
			TEXT("Native-direct mode participates in canonical identity"),
			DirectFunction->CanonicalIdentity,
			FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
				CachedFunction->CanonicalIdentity,
				DirectFunction->DispatchMode));
		TestNotEqual(
			TEXT("Native-direct mode changes stable binding identity"),
			DirectFunction->StableId,
			CachedFunction->StableId);
	}
	TestNotEqual(
		TEXT("Changing only authorization changes descriptor selection hash"),
		DirectPackage.SelectionHash,
		CachedPackage.SelectionHash);
	TestNotEqual(
		TEXT("Changing only authorization changes package hash"),
		DirectPackage.PackageHash,
		CachedPackage.PackageHash);

	for (const TCHAR* PropertyKind : { TEXT("property_get"), TEXT("property_set") })
	{
		const FAvidScriptBindingFunctionModel* CachedProperty =
			FindModelBinding(CachedPackage, PropertyKind, TEXT("CustomTimeDilation"));
		const FAvidScriptBindingFunctionModel* DirectProperty =
			FindModelBinding(DirectPackage, PropertyKind, TEXT("CustomTimeDilation"));
		if (TestNotNull(TEXT("Cached profile retains property binding"), CachedProperty)
			&& TestNotNull(TEXT("Direct profile retains property binding"), DirectProperty))
		{
			TestEqual(
				TEXT("Function authorization does not change property dispatch"),
				DirectProperty->DispatchMode,
				CachedProperty->DispatchMode);
			TestEqual(
				TEXT("Function authorization does not change property canonical identity"),
				DirectProperty->CanonicalIdentity,
				CachedProperty->CanonicalIdentity);
			TestEqual(
				TEXT("Function authorization does not change property stable id"),
				DirectProperty->StableId,
				CachedProperty->StableId);
		}
	}

	FString DirectOnlyJson;
	FAvidScriptBindingPackageModel DirectOnlyPackage;
	if (!GenerateAndParse(MakeProfile(true, false), DirectOnlyJson, DirectOnlyPackage))
	{
		return false;
	}
	FAvidScriptBindingPackageModel V7IdentityModel = DirectOnlyPackage;
	V7IdentityModel.SchemaVersion = 7;
	V7IdentityModel.SelectionHash =
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(V7IdentityModel);
	V7IdentityModel.PackageHash =
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(V7IdentityModel);
	TSharedPtr<FJsonObject> V7Root;
	TestTrue(TEXT("Direct-only descriptor JSON can be decoded"), ParseDescriptor(DirectOnlyJson, V7Root));
	if (V7Root.IsValid())
	{
		V7Root->SetNumberField(TEXT("schema_version"), 7);
		V7Root->SetStringField(TEXT("selection_hash"), V7IdentityModel.SelectionHash);
		V7Root->SetStringField(TEXT("package_hash"), V7IdentityModel.PackageHash);
		FString V7Json;
		FAvidScriptBindingPackageModel RejectedPackage;
		FString ErrorCategory;
		FString ErrorSource;
		TestFalse(
			TEXT("Descriptor schema v7 rejects qualified native-direct function mode"),
			SerializeDescriptor(V7Root, V7Json)
			&& FAvidScriptBindingDescriptorParser::Parse(
				V7Json,
				RejectedPackage,
				ErrorCategory,
				ErrorSource));
		TestEqual(
			TEXT("Legacy mode rejection uses the descriptor contract category"),
			ErrorCategory,
			FString(TEXT("descriptor_contract_invalid")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorInterfaceInteropTest,
	"AvidScript.Editor.BindingDescriptor.InterfaceInterop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorInterfaceInteropTest::RunTest(
	const FString& Parameters)
{
	const FString InterfacePath =
		UAvidScriptCSharpEmitterCallableInterface::StaticClass()->GetPathName();
	const FString ConsumerPath =
		UAvidScriptCSharpEmitterInterfaceConsumer::StaticClass()->GetPathName();

	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.interface.interop");
	FAvidScriptReflectedClassSelection InterfaceRule;
	InterfaceRule.OwnerClassPath = InterfacePath;
	InterfaceRule.IncludeFunctions.Add(TEXT("TransformInterfaceValue"));
	Profile.Classes.Add(MoveTemp(InterfaceRule));
	FAvidScriptReflectedClassSelection ConsumerRule;
	ConsumerRule.OwnerClassPath = ConsumerPath;
	ConsumerRule.IncludeFunctions.Add(TEXT("RoundtripInterface"));
	ConsumerRule.IncludeProperties.Add(TEXT("InterfaceValue"));
	ConsumerRule.WritableProperties.Add(TEXT("InterfaceValue"));
	Profile.Classes.Add(MoveTemp(ConsumerRule));

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Interface functions and properties generate from one profile"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				Profile,
				DescriptorJson,
				SelectionResult,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("Interface descriptor satisfies the shared parser"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Package,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(":") + ErrorSource);
		return false;
	}
	TestEqual(TEXT("Interface profile emits four bindings"), Package.Bindings.Num(), 4);
	const FAvidScriptBindingFunctionModel* InterfaceCall =
		Package.Bindings.FindByPredicate(
			[&InterfacePath](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.OwnerClass == InterfacePath
					&& Binding.UeFunction == TEXT("TransformInterfaceValue");
			});
	const FAvidScriptBindingFunctionModel* Roundtrip =
		Package.Bindings.FindByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.UeFunction == TEXT("RoundtripInterface");
			});
	if (TestNotNull(TEXT("Interface owner call is present"), InterfaceCall))
	{
		TestEqual(
			TEXT("Interface call uses cached ProcessEvent dispatch"),
			InterfaceCall->DispatchMode,
			FString(TEXT("cached_process_event")));
		TestEqual(
			TEXT("Interface owner is preserved in the descriptor"),
			InterfaceCall->OwnerClass,
			InterfacePath);
	}
	if (TestNotNull(TEXT("Interface roundtrip call is present"), Roundtrip))
	{
		if (TestEqual(
				TEXT("Interface roundtrip has one parameter"),
				Roundtrip->Parameters.Num(),
				1))
		{
			TestEqual(
				TEXT("Interface parameter uses an object handle"),
				Roundtrip->Parameters[0].Kind,
				FString(TEXT("object_handle")));
		}
		TestEqual(
			TEXT("Interface return uses an object handle"),
			Roundtrip->ReturnValue.Kind,
			FString(TEXT("object_handle")));
		TestEqual(
			TEXT("Interface identity uses the UInterface class path"),
			Roundtrip->ReturnValue.CanonicalType,
			TEXT("object:") + InterfacePath);
	}
	const FAvidScriptBindingTypeModel* InterfaceType =
		Package.Types.FindByPredicate(
			[&InterfacePath](const FAvidScriptBindingTypeModel& Type)
			{
				return Type.CanonicalType == TEXT("object:") + InterfacePath;
			});
	if (TestNotNull(TEXT("Interface handle type is present"), InterfaceType))
	{
		TestEqual(
			TEXT("C# interface handle uses an I-prefixed type name"),
			InterfaceType->CppType,
			FString(TEXT("IAvidScriptCSharpEmitterCallableInterface")));
		if (TestEqual(
				TEXT("Interface handle keeps two ABI cells"),
				InterfaceType->AbiTypes.Num(),
				2))
		{
			TestEqual(TEXT("Interface slot ABI is i32"), InterfaceType->AbiTypes[0], FString(TEXT("i")));
			TestEqual(TEXT("Interface generation ABI is i32"), InterfaceType->AbiTypes[1], FString(TEXT("i")));
		}
	}

	FString ReferenceSource;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	if (!TestTrue(
			TEXT("Interface descriptor renders a C# facade"),
			FAvidScriptEditorCSharpBindingEmitter::Emit(
				DescriptorJson,
				ReferenceSource,
				ManifestJson,
				EmitResult)))
	{
		AddError(EmitResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("C# facade declares the interface handle"),
		ReferenceSource.Contains(
			TEXT("public readonly struct IAvidScriptCSharpEmitterCallableInterface")));
	TestTrue(
		TEXT("C# facade exposes the interface method"),
		ReferenceSource.Contains(TEXT("TransformInterfaceValue(int value)")));
	TestTrue(
		TEXT("C# facade exposes interface property syntax"),
		ReferenceSource.Contains(
			TEXT("public IAvidScriptCSharpEmitterCallableInterface InterfaceValue")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorNetworkRpcTest,
	"AvidScript.Editor.BindingDescriptor.NetworkRpcSchema15",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorNetworkRpcTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		AAvidScriptBindingRuntimeNetworkTestActor::StaticClass()->GetPathName();
	const TArray<FAvidScriptReflectedFunctionSelection> Functions = {
		{ OwnerPath, TEXT("ServerSubmitValue") },
		{ OwnerPath, TEXT("ClientApplyValue") },
		{ OwnerPath, TEXT("MulticastAnnounceValue") }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("RPC descriptor generates without handwritten bindings"),
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.test.network_rpc"),
				Functions,
				DescriptorJson,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("RPC schema 15 descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Package,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorSource);
		return false;
	}
	TestEqual(TEXT("RPC package selects schema 15"), Package.SchemaVersion, 15);
	TestEqual(
		TEXT("RPC package records the D1 generator"),
		Package.GeneratorVersion,
		FString(TEXT("57.12D1.0")));

	const auto FindFunction = [&Package](const TCHAR* Name)
	{
		return Package.Bindings.FindByPredicate(
			[Name](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.UeFunction == Name;
			});
	};
	const FAvidScriptBindingFunctionModel* Server =
		FindFunction(TEXT("ServerSubmitValue"));
	const FAvidScriptBindingFunctionModel* Client =
		FindFunction(TEXT("ClientApplyValue"));
	const FAvidScriptBindingFunctionModel* Multicast =
		FindFunction(TEXT("MulticastAnnounceValue"));
	if (!TestNotNull(TEXT("Server RPC is present"), Server)
		|| !TestNotNull(TEXT("Client RPC is present"), Client)
		|| !TestNotNull(TEXT("Multicast RPC is present"), Multicast))
	{
		return false;
	}
	TestEqual(
		TEXT("Server mode is explicit"),
		Server->Network.Mode,
		EAvidScriptBindingNetworkMode::Server);
	TestTrue(TEXT("Server reliability is explicit"), Server->Network.bReliable);
	TestEqual(
		TEXT("Client mode is explicit"),
		Client->Network.Mode,
		EAvidScriptBindingNetworkMode::Client);
	TestFalse(TEXT("Client unreliability is explicit"), Client->Network.bReliable);
	TestEqual(
		TEXT("Multicast mode is explicit"),
		Multicast->Network.Mode,
		EAvidScriptBindingNetworkMode::Multicast);
	TestTrue(
		TEXT("RPC identity includes network policy"),
		Server->CanonicalIdentity.Contains(
			TEXT("|network_mode=server|network_reliable=1")));
	TestTrue(
		TEXT("All RPCs remain on UE ProcessEvent routing"),
		Package.Bindings.ContainsByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.Network.IsNetworked()
					&& Binding.DispatchMode != TEXT("cached_process_event");
			}) == false);

	FString ReferenceSource;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(
		TEXT("RPC descriptor emits a typed C# facade"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			ReferenceSource,
			ManifestJson,
			EmitResult));
	TestTrue(
		TEXT("C# facade exposes all project RPCs"),
		ReferenceSource.Contains(TEXT("ServerSubmitValue("))
			&& ReferenceSource.Contains(TEXT("ClientApplyValue("))
			&& ReferenceSource.Contains(TEXT("MulticastAnnounceValue(")));

	FAvidScriptBindingPackageModel Tampered = Package;
	FAvidScriptBindingFunctionModel* TamperedServer =
		Tampered.Bindings.FindByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.UeFunction == TEXT("ServerSubmitValue");
			});
	check(TamperedServer != nullptr);
	TamperedServer->Network.bReliable = false;
	Tampered.SelectionHash =
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Tampered);
	Tampered.PackageHash =
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(Tampered);
	FString TamperedJson;
	TestTrue(
		TEXT("Canonical serializer can encode the tampered drift fixture"),
		FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
			Tampered,
			TamperedJson));
	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestFalse(
		TEXT("Runtime rejects RPC reliability drift"),
		FAvidScriptBindingPackage::LoadDescriptor(
			TamperedJson,
			RuntimePackage,
			LoadResult));
	TestEqual(
		TEXT("RPC drift has a stable failure category"),
		LoadResult.ErrorCategory,
		FString(TEXT("binding_network_contract_mismatch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorReplicatedPropertyTest,
	"AvidScript.Editor.BindingDescriptor.ReplicatedPropertySchema16",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorReplicatedPropertyTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		AAvidScriptBindingRuntimeNetworkTestActor::StaticClass()->GetPathName();
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ OwnerPath, TEXT("ReplicatedScore"), true },
		{ OwnerPath, TEXT("ReplicatedRoutedValue"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Replicated properties generate without handwritten wrappers"),
			FAvidScriptEditorBindingDescriptorGenerator::
				GenerateWithReadableProperties(
					TEXT("avidscript.test.replicated_property"),
					{},
					Properties,
					DescriptorJson,
					GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("Replicated property schema 16 descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Package,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorSource);
		return false;
	}
	TestEqual(
		TEXT("Replicated property package selects schema 16"),
		Package.SchemaVersion,
		16);
	TestEqual(
		TEXT("Replicated property package records the D2 generator"),
		Package.GeneratorVersion,
		FString(TEXT("57.12D2.0")));

	const auto FindBinding = [&Package](const TCHAR* Member, const TCHAR* Kind)
	{
		return Package.Bindings.FindByPredicate(
			[Member, Kind](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.UeMember == Member
					&& Binding.BindingKind == Kind;
			});
	};
	const FAvidScriptBindingFunctionModel* ScoreGetter =
		FindBinding(TEXT("ReplicatedScore"), TEXT("property_get"));
	const FAvidScriptBindingFunctionModel* ScoreSetter =
		FindBinding(TEXT("ReplicatedScore"), TEXT("property_set"));
	const FAvidScriptBindingFunctionModel* RoutedSetter =
		FindBinding(TEXT("ReplicatedRoutedValue"), TEXT("property_set"));
	if (!TestNotNull(TEXT("RepNotify getter is present"), ScoreGetter)
		|| !TestNotNull(TEXT("RepNotify setter is present"), ScoreSetter)
		|| !TestNotNull(TEXT("Replicated BlueprintSetter is present"), RoutedSetter))
	{
		return false;
	}
	TestEqual(
		TEXT("RepNotify mode is explicit"),
		ScoreSetter->PropertyReplication.Mode,
		EAvidScriptBindingPropertyReplicationMode::RepNotify);
	TestEqual(
		TEXT("RepNotify function is explicit"),
		ScoreSetter->PropertyReplication.RepNotifyFunction,
		FName(TEXT("OnRep_ReplicatedScore")));
	TestEqual(
		TEXT("Plain replicated mode is explicit"),
		RoutedSetter->PropertyReplication.Mode,
		EAvidScriptBindingPropertyReplicationMode::Replicated);
	TestTrue(
		TEXT("Replicated identity includes notification semantics"),
		ScoreGetter->CanonicalIdentity.Contains(
			TEXT("|property_replication=rep_notify|rep_notify=OnRep_ReplicatedScore")));
	TestEqual(
		TEXT("Replicated direct writes are not reload-reversible"),
		ScoreSetter->ReloadEffect,
		EAvidScriptBindingReloadEffect::Unsupported);
	TestEqual(
		TEXT("Replicated BlueprintSetter keeps routed dispatch"),
		RoutedSetter->DispatchMode,
		FString(TEXT("cached_blueprint_setter")));

	FString ReferenceSource;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(
		TEXT("Replicated properties emit a typed C# facade"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			ReferenceSource,
			ManifestJson,
			EmitResult));
	TestTrue(
		TEXT("C# facade preserves ordinary property syntax"),
		ReferenceSource.Contains(TEXT("ReplicatedScore"))
			&& ReferenceSource.Contains(TEXT("ReplicatedRoutedValue")));

	FAvidScriptBindingPackageModel Tampered = Package;
	FAvidScriptBindingFunctionModel* TamperedScore =
		Tampered.Bindings.FindByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.UeMember == TEXT("ReplicatedScore")
					&& Binding.BindingKind == TEXT("property_set");
			});
	check(TamperedScore != nullptr);
	TamperedScore->PropertyReplication.RepNotifyFunction =
		TEXT("OnRep_Tampered");
	Tampered.SelectionHash =
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Tampered);
	Tampered.PackageHash =
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(Tampered);
	FString TamperedJson;
	TestTrue(
		TEXT("Canonical serializer encodes the RepNotify drift fixture"),
		FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
			Tampered,
			TamperedJson));
	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestFalse(
		TEXT("Runtime rejects RepNotify drift"),
		FAvidScriptBindingPackage::LoadDescriptor(
			TamperedJson,
			RuntimePackage,
			LoadResult));
	TestEqual(
		TEXT("RepNotify identity drift is rejected by the descriptor contract"),
		LoadResult.ErrorCategory,
		FString(TEXT("descriptor_contract_invalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorInboundHandlerTest,
	"AvidScript.Editor.BindingDescriptor.InboundHandlerSchema18",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorInboundHandlerTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		AAvidScriptBindingRuntimeNetworkTestActor::StaticClass()->GetPathName();
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.test.inbound_handlers");
	Profile.SelfClassPath = OwnerPath;
	FAvidScriptReflectedClassSelection& Rule =
		Profile.Classes.AddDefaulted_GetRef();
	Rule.OwnerClassPath = OwnerPath;
	Rule.BeforeHandlers = {TEXT("ServerSubmitValue")};
	Rule.AfterHandlers = {TEXT("OnRep_ReplicatedScore")};

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("RPC and RepNotify handlers generate from one profile rule"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				Profile,
				DescriptorJson,
				SelectionResult,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("Inbound handler schema 18 descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Package,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorSource);
		return false;
	}
	TestEqual(TEXT("Inbound handlers select schema 18"), Package.SchemaVersion, 18);
	TestEqual(
		TEXT("Inbound handlers select the D4 generator"),
		Package.GeneratorVersion,
		FString(TEXT("57.12D4.0")));
	TestEqual(TEXT("Both callbacks are described"), Package.DelegateEvents.Num(), 2);

	const FAvidScriptBindingDelegateEventModel* Rpc =
		Package.DelegateEvents.FindByPredicate(
			[](const FAvidScriptBindingDelegateEventModel& Event)
			{
				return Event.DelegateKind == TEXT("network_rpc");
			});
	const FAvidScriptBindingDelegateEventModel* RepNotify =
		Package.DelegateEvents.FindByPredicate(
			[](const FAvidScriptBindingDelegateEventModel& Event)
			{
				return Event.DelegateKind == TEXT("rep_notify");
			});
	if (!TestNotNull(TEXT("Server RPC callback is described"), Rpc)
		|| !TestNotNull(TEXT("RepNotify callback is described"), RepNotify))
	{
		return false;
	}
	TestEqual(TEXT("Server direction is preserved"), Rpc->Network.Mode, EAvidScriptBindingNetworkMode::Server);
	TestTrue(TEXT("Server reliability is preserved"), Rpc->Network.bReliable);
	TestEqual(TEXT("Server before mode is preserved"), Rpc->HandlerMode, FString(TEXT("before")));
	TestEqual(
		TEXT("RepNotify property identity is preserved"),
		RepNotify->RepNotifyProperty,
		FName(TEXT("ReplicatedScore")));
	TestEqual(TEXT("RepNotify after mode is preserved"), RepNotify->HandlerMode, FString(TEXT("after")));

	FString ReferenceSource;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(
		TEXT("Schema 18 emits the shared C# callback surface"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			ReferenceSource,
			ManifestJson,
			EmitResult));
	TestTrue(
		TEXT("C# exposes event constants for both inbound handlers"),
		ReferenceSource.Contains(TEXT("ServerSubmitValue"))
			&& ReferenceSource.Contains(TEXT("OnRep_ReplicatedScore")));
	TestFalse(
		TEXT("Inbound handlers do not expose guest subscription commands"),
		ReferenceSource.Contains(TEXT("AvidSubscription ServerSubmitValue"))
			|| ReferenceSource.Contains(TEXT("AvidSubscription OnRep_ReplicatedScore")));

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestTrue(
		TEXT("Runtime prepares the schema 18 reflection snapshot"),
		FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			RuntimePackage,
			LoadResult));
	if (!RuntimePackage.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("Runtime separates inbound handlers from delegates"),
		RuntimePackage->GetInboundHandlerCount(),
		2);
	TestEqual(
		TEXT("Runtime does not misclassify handlers as multicast events"),
		RuntimePackage->GetMulticastDelegateEventCount(),
		0);

	FString TamperedJson = DescriptorJson;
	TamperedJson.ReplaceInline(
		TEXT("\"handler_mode\": \"before\""),
		TEXT("\"handler_mode\": \"sideways\""),
		ESearchCase::CaseSensitive);
	TestFalse(
		TEXT("Handler mode tampering is rejected"),
		FAvidScriptBindingDescriptorParser::Parse(
			TamperedJson,
			Package,
			ErrorCategory,
			ErrorSource));

	FAvidScriptBindingSelectionProfile ConflictProfile = Profile;
	ConflictProfile.Classes[0].IncludeHandlers.Add(TEXT("ServerSubmitValue"));
	FString ConflictDescriptor;
	FAvidScriptBindingSelectionResolveResult ConflictSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult ConflictGenerateResult;
	TestFalse(
		TEXT("A handler cannot appear in more than one chain-mode group"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			ConflictProfile,
			ConflictDescriptor,
			ConflictSelectionResult,
			ConflictGenerateResult));
	TestEqual(
		TEXT("Chain-mode conflicts use a stable category"),
		ConflictSelectionResult.ErrorCategory,
		FString(TEXT("handler_mode_conflict")));

	const FString SampleProfilePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("AvidScript/Samples/CSharp/InboundNetworkHandlers/InboundNetworkHandlers.csharp-profile.json")));
	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!TestTrue(
			TEXT("Readable inbound handler sample profile loads"),
			FAvidScriptEditorCSharpProfileService::LoadProfile(
				SampleProfilePath,
				ProfileResult)))
	{
		AddError(ProfileResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("Inbound handler sample uses C# profile schema 10"),
		ProfileResult.SchemaVersion,
		10);
	if (!TestEqual(
			TEXT("Inbound handler sample resolves one class rule"),
			ProfileResult.ResolvedBindingSelection.Classes.Num(),
			1))
	{
		return false;
	}
	TestEqual(
		TEXT("Sample preserves one before handler"),
		ProfileResult.ResolvedBindingSelection.Classes[0].BeforeHandlers.Num(),
		1);
	TestEqual(
		TEXT("Sample preserves one after handler"),
		ProfileResult.ResolvedBindingSelection.Classes[0].AfterHandlers.Num(),
		1);
	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
			TEXT("Inbound handler sample builds through the production C# pipeline"),
			FAvidScriptEditorCSharpBuildService::BuildProfile(
				FAvidScriptEditorCSharpProfileService::MakeBuildRequest(
					ProfileResult),
				BuildResult)))
	{
		AddError(BuildResult.ErrorMessage + TEXT("\n") + BuildResult.Stderr);
		return false;
	}
	TestTrue(TEXT("Inbound handler sample publishes a VM artifact"), BuildResult.bVmArtifactPublished);
	TestTrue(TEXT("Inbound handler VM artifact exists"), FPaths::FileExists(BuildResult.VmArtifactPath));
	TestTrue(TEXT("Inbound handler sample publishes its Runtime manifest"), FPaths::FileExists(BuildResult.ManifestPath));
	TestTrue(TEXT("Inbound handler sample publishes its binding package"), FPaths::FileExists(BuildResult.BindingPackagePath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorBlueprintInboundHandlerTest,
	"AvidScript.Editor.BindingDescriptor.BlueprintInboundHandlerSchema18",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorBlueprintInboundHandlerTest::RunTest(
	const FString& Parameters)
{
	const FName BlueprintName(*FString::Printf(
		TEXT("AvidScriptInboundHandler_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UBlueprint> Blueprint(
		FKismetEditorUtilities::CreateBlueprint(
			AAvidScriptBindingRuntimeNetworkTestActor::StaticClass(),
			GetTransientPackage(),
			BlueprintName,
			BPTYPE_Normal,
			TEXT("AvidScriptInboundHandlerTest")));
	if (!TestNotNull(TEXT("Transient network Blueprint is created"), Blueprint.Get()))
	{
		return false;
	}

	UEdGraph* const OverrideGraph =
		FBlueprintEditorUtils::CreateNewGraph(
			Blueprint.Get(),
			GET_FUNCTION_NAME_CHECKED(
				AAvidScriptBindingRuntimeNetworkTestActor,
				OnRep_BlueprintScore),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph(
		Blueprint.Get(),
		OverrideGraph,
		false,
		AAvidScriptBindingRuntimeNetworkTestActor::StaticClass());
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	UClass* const BlueprintClass = Blueprint->GeneratedClass;
	if (!TestNotNull(TEXT("Transient network Blueprint compiles"), BlueprintClass))
	{
		return false;
	}
	UFunction* const BlueprintFunction = BlueprintClass->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(
			AAvidScriptBindingRuntimeNetworkTestActor,
			OnRep_BlueprintScore),
		EIncludeSuperFlag::ExcludeSuper);
	if (!TestNotNull(TEXT("Blueprint owns the RepNotify override"), BlueprintFunction))
	{
		return false;
	}
	TestFalse(
		TEXT("Blueprint override is not native"),
		BlueprintFunction->HasAnyFunctionFlags(FUNC_Native));
	TestTrue(
		TEXT("Blueprint override owns bytecode"),
		!BlueprintFunction->Script.IsEmpty());

	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.test.blueprint_inbound_handler");
	Profile.SelfClassPath = BlueprintClass->GetPathName();
	FAvidScriptReflectedClassSelection& Rule =
		Profile.Classes.AddDefaulted_GetRef();
	Rule.OwnerClassPath = BlueprintClass->GetPathName();
	Rule.AfterHandlers = {
		GET_FUNCTION_NAME_CHECKED(
			AAvidScriptBindingRuntimeNetworkTestActor,
			OnRep_BlueprintScore)
	};

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Blueprint RepNotify handler generates"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				Profile,
				DescriptorJson,
				SelectionResult,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("Blueprint handler schema 18 parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Package,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorSource);
		return false;
	}
	if (!TestEqual(TEXT("One Blueprint handler is emitted"), Package.DelegateEvents.Num(), 1))
	{
		return false;
	}
	const FAvidScriptBindingDelegateEventModel& Event = Package.DelegateEvents[0];
	TestEqual(TEXT("Blueprint handler keeps after mode"), Event.HandlerMode, FString(TEXT("after")));
	TestEqual(TEXT("Blueprint handler keeps RepNotify property"), Event.RepNotifyProperty, FName(TEXT("BlueprintScore")));
	TestEqual(TEXT("Blueprint handler owner is the generated class"), Event.OwnerClass, BlueprintClass->GetPathName());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorV8BlueprintSetterIdentityTest,
	"AvidScript.Editor.BindingDescriptor.V8BlueprintSetterIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorV8BlueprintSetterIdentityTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerClassPath =
		AAvidScriptBindingRuntimeProcessEventTestActor::StaticClass()->GetPathName();
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ OwnerClassPath, TEXT("RoutedValue"), true }
	};
	FString Json;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("BlueprintSetter property generates a schema v8 descriptor"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
				TEXT("avidscript.test.blueprint_setter_identity"),
				{},
				Properties,
				Json,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("BlueprintSetter descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				Json,
				Package,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(":") + ErrorSource);
		return false;
	}

	const int32 SetterIndex = Package.Bindings.IndexOfByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_set");
		});
	if (!TestTrue(TEXT("BlueprintSetter descriptor contains a setter"), SetterIndex != INDEX_NONE))
	{
		return false;
	}
	const FAvidScriptBindingFunctionModel& Setter = Package.Bindings[SetterIndex];
	TestEqual(
		TEXT("Descriptor fixes the concrete BlueprintSetter UFunction"),
		Setter.UeFunction,
		FString(TEXT("SetRoutedValue")));
	const FString ExpectedIdentity =
		FAvidScriptBindingDescriptorIdentity::MakePropertySetCanonicalIdentity(
			OwnerClassPath,
			TEXT("RoutedValue"),
			Setter.Parameters[0].CanonicalType,
			TEXT("SetRoutedValue"));
	TestEqual(
		TEXT("BlueprintSetter name participates in canonical identity"),
		Setter.CanonicalIdentity,
		ExpectedIdentity);
	TestEqual(
		TEXT("BlueprintSetter name participates in stable identity"),
		Setter.StableId,
		FAvidScriptHash::Sha256HexUtf8(ExpectedIdentity));

	FAvidScriptBindingPackageModel AlternateSetterPackage = Package;
	AlternateSetterPackage.Bindings[SetterIndex].UeFunction =
		TEXT("SetAlternateRoutedValue");
	TestNotEqual(
		TEXT("BlueprintSetter name participates in package identity"),
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(
			AlternateSetterPackage),
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package));

	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("BlueprintSetter descriptor is JSON"), ParseDescriptor(Json, Root))
		|| !Root.IsValid())
	{
		return false;
	}
	TSharedPtr<FJsonObject> SetterObject;
	for (const TSharedPtr<FJsonValue>& BindingValue :
		Root->GetArrayField(TEXT("bindings")))
	{
		const TSharedPtr<FJsonObject> Candidate =
			BindingValue.IsValid() ? BindingValue->AsObject() : nullptr;
		if (Candidate.IsValid()
			&& Candidate->GetStringField(TEXT("binding_kind"))
				== TEXT("property_set"))
		{
			SetterObject = Candidate;
			break;
		}
	}
	if (!TestTrue(
			TEXT("Canonical JSON contains the property setter"),
			SetterObject.IsValid()))
	{
		return false;
	}
	TestEqual(
		TEXT("Canonical JSON carries the BlueprintSetter UFunction"),
		SetterObject->GetStringField(TEXT("ue_function")),
		FString(TEXT("SetRoutedValue")));

	SetterObject->RemoveField(TEXT("ue_function"));
	FString MissingSetterJson;
	FAvidScriptBindingPackageModel MissingSetterPackage;
	TestFalse(
		TEXT("Schema v8 parser rejects a missing setter function field"),
		SerializeDescriptor(Root, MissingSetterJson)
			&& FAvidScriptBindingDescriptorParser::Parse(
				MissingSetterJson,
				MissingSetterPackage,
				ErrorCategory,
				ErrorSource));
	TestEqual(
		TEXT("Missing setter function field has a stable source"),
		ErrorSource,
		FString(TEXT("ue_function")));

	if (!TestTrue(
		TEXT("BlueprintSetter descriptor JSON can be restored"),
		ParseDescriptor(Json, Root)))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& BindingValue :
		Root->GetArrayField(TEXT("bindings")))
	{
		const TSharedPtr<FJsonObject> Candidate =
			BindingValue.IsValid() ? BindingValue->AsObject() : nullptr;
		if (Candidate.IsValid()
			&& Candidate->GetStringField(TEXT("binding_kind"))
				== TEXT("property_set"))
		{
			Candidate->SetStringField(
				TEXT("ue_function"),
				TEXT("SetAlternateRoutedValue"));
			break;
		}
	}
	FString TamperedSetterJson;
	FAvidScriptBindingPackageModel TamperedSetterPackage;
	TestFalse(
		TEXT("Parser rejects a setter function not bound by stable identity"),
		SerializeDescriptor(Root, TamperedSetterJson)
			&& FAvidScriptBindingDescriptorParser::Parse(
				TamperedSetterJson,
				TamperedSetterPackage,
				ErrorCategory,
				ErrorSource));
	TestEqual(
		TEXT("Setter stable identity tampering is a descriptor contract failure"),
		ErrorCategory,
		FString(TEXT("descriptor_contract_invalid")));

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestTrue(
		TEXT("Runtime accepts the descriptor before metadata drift"),
		FAvidScriptBindingPackage::LoadDescriptor(
			Json,
			RuntimePackage,
			LoadResult));

	FProperty* RoutedValueProperty = FindFProperty<FProperty>(
		AAvidScriptBindingRuntimeProcessEventTestActor::StaticClass(),
		TEXT("RoutedValue"));
	if (!TestNotNull(
			TEXT("BlueprintSetter drift fixture property exists"),
			RoutedValueProperty))
	{
		return false;
	}
	const FString OriginalBlueprintSetter =
		RoutedValueProperty->GetMetaData(TEXT("BlueprintSetter"));
	RoutedValueProperty->SetMetaData(
		TEXT("BlueprintSetter"),
		TEXT("SetAlternateRoutedValue"));
	TSharedPtr<const FAvidScriptBindingPackage> DriftedRuntimePackage;
	FAvidScriptBindingPackageLoadResult DriftedLoadResult;
	const bool bDriftedLoadSucceeded =
		FAvidScriptBindingPackage::LoadDescriptor(
			Json,
			DriftedRuntimePackage,
			DriftedLoadResult);
	RoutedValueProperty->SetMetaData(
		TEXT("BlueprintSetter"),
		*OriginalBlueprintSetter);
	TestFalse(
		TEXT("Runtime rejects BlueprintSetter metadata drift to a same-signature function"),
		bDriftedLoadSucceeded);
	TestEqual(
		TEXT("BlueprintSetter metadata drift has a stable category"),
		DriftedLoadResult.ErrorCategory,
		FString(TEXT("binding_property_blueprint_setter_mismatch")));
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
	const TArray<FAvidScriptReflectedFunctionSelection> StringSelections = {
		{ FixtureOwner, TEXT("ReturnFName") },
		{ FixtureOwner, TEXT("OutFName") },
		{ FixtureOwner, TEXT("RefFName") },
		{ FixtureOwner, TEXT("ConstRefFName") },
		{ FixtureOwner, TEXT("ReturnFString") },
		{ FixtureOwner, TEXT("OutFString") },
		{ FixtureOwner, TEXT("RefFString") },
		{ FixtureOwner, TEXT("ConstRefFString") },
		{ FixtureOwner, TEXT("FStringValueDefault") }
	};
	const TArray<FAvidScriptReflectedPropertySelection> StringProperties = {
		{ FixtureOwner, TEXT("ReadableFName"), true },
		{ FixtureOwner, TEXT("ReadableFString"), true }
	};
	FString StringJson;
	FAvidScriptBindingDescriptorGenerateResult StringResult;
	TestTrue(
		TEXT("FName and FString functions and properties project in every supported direction"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.engine.name_string"),
			StringSelections,
			StringProperties,
			StringJson,
			StringResult));

	TSharedPtr<FJsonObject> StringRoot;
	if (TestTrue(TEXT("FName and FString descriptor parses"), ParseDescriptor(StringJson, StringRoot)) && StringRoot.IsValid())
	{
		const auto AssertStringType = [this, &StringRoot](const FString& CanonicalType, const FString& Kind, const FString& CppType)
		{
			const TSharedPtr<FJsonObject> Type = FindType(StringRoot->GetArrayField(TEXT("types")), CanonicalType);
			TestNotNull(TEXT("String type descriptor is present: ") + CanonicalType, Type.Get());
			if (!Type.IsValid())
			{
				return;
			}
			TestEqual(TEXT("String type kind is exact: ") + CanonicalType, Type->GetStringField(TEXT("kind")), Kind);
			TestEqual(TEXT("String type C++ name is exact: ") + CanonicalType, Type->GetStringField(TEXT("cpp_type")), CppType);
			TestEqual(TEXT("String type size is exact: ") + CanonicalType, Type->GetIntegerField(TEXT("size")), 4);
			TestEqual(TEXT("String type alignment is exact: ") + CanonicalType, Type->GetIntegerField(TEXT("alignment")), 4);
			const TArray<TSharedPtr<FJsonValue>>& AbiTypes = Type->GetArrayField(TEXT("abi_types"));
			TestTrue(TEXT("String type ABI is one address cell: ") + CanonicalType,
				AbiTypes.Num() == 1 && AbiTypes[0]->AsString() == TEXT("i"));
		};
		AssertStringType(TEXT("name:fname"), TEXT("name_utf8"), TEXT("FName"));
		AssertStringType(TEXT("string:fstring"), TEXT("string_utf8"), TEXT("FString"));

		for (const FAvidScriptReflectedFunctionSelection& Selection : StringSelections)
		{
			const FString FunctionName = Selection.FunctionName.ToString();
			const TSharedPtr<FJsonObject> Binding = FindBinding(StringRoot->GetArrayField(TEXT("bindings")), FixtureOwner, FunctionName);
			TestNotNull(TEXT("String function binding projects: ") + FunctionName, Binding.Get());
		}
		TestTrue(TEXT("String projection does not raise the descriptor schema"), StringRoot->GetIntegerField(TEXT("schema_version")) <= 9);
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
		TEXT("GetAttachedActors")
	};
	TestTrue(
		TEXT("Supported TArray projection generates the universal value graph"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.core"),
			{ UnsupportedSelection },
			Json,
			Result));
	FAvidScriptBindingPackageModel ArrayPackage;
	FString ArrayErrorCategory;
	FString ArrayErrorSource;
	TestTrue(
		TEXT("Generated TArray descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			Json,
			ArrayPackage,
			ArrayErrorCategory,
			ArrayErrorSource));
	TestEqual(TEXT("TArray descriptor activates schema 19"), ArrayPackage.SchemaVersion, 19);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingDescriptorStructWireTest,
	"AvidScript.Editor.BindingDescriptor.StructWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingDescriptorStructWireTest::RunTest(const FString& Parameters)
{
	const FString OwnerPath = UAvidScriptCSharpBindingEmitterTestObject::StaticClass()->GetPathName();
	FString Json;
	FAvidScriptBindingDescriptorGenerateResult Result;
	TestTrue(
		TEXT("Nested fixed-width USTRUCT descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.test.struct_wire"),
			{ { OwnerPath, TEXT("StructWireRoundTrip") } },
			Json,
			Result));

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("Nested fixed-width USTRUCT descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(Json, Package, ErrorCategory, ErrorSource)))
	{
		return true;
	}
	TestEqual(TEXT("Struct-wire descriptor uses schema v9"), Package.SchemaVersion, 9);
	TestEqual(
		TEXT("Struct-wire descriptor uses the recursive generator version"),
		Package.GeneratorVersion,
		FString(TEXT("57.11B1.0")));
	const FAvidScriptBindingTypeModel* ParsedRootType = Package.Types.FindByPredicate(
		[](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.CanonicalType == TEXT("struct_wire:")
				+ FAvidScriptStructWireRootTestType::StaticStruct()->GetPathName();
		});
	if (TestNotNull(TEXT("Parsed root struct-wire type is present"), ParsedRootType))
	{
		TestEqual(
			TEXT("Schema v9 struct stable id binds size and alignment"),
			ParsedRootType->StableId,
			FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
				ParsedRootType->CanonicalType,
				ParsedRootType->EnumValues,
				ParsedRootType->StructFields,
				ParsedRootType->Size,
				ParsedRootType->Alignment));
	}

	TSharedPtr<FJsonObject> Root;
	if (!ParseDescriptor(Json, Root) || !Root.IsValid())
	{
		AddError(TEXT("Struct-wire descriptor could not be decoded for fixture assertions."));
		return true;
	}
	const TArray<TSharedPtr<FJsonValue>>& Types = Root->GetArrayField(TEXT("types"));
	const TSharedPtr<FJsonObject> RootType = FindType(
		Types,
		TEXT("struct_wire:") + FAvidScriptStructWireRootTestType::StaticStruct()->GetPathName());
	const TSharedPtr<FJsonObject> NestedType = FindType(
		Types,
		TEXT("struct_wire:") + FAvidScriptStructWireNestedTestType::StaticStruct()->GetPathName());
	TestNotNull(TEXT("Root struct-wire type is present"), RootType.Get());
	TestNotNull(TEXT("Nested struct-wire type is present"), NestedType.Get());
	if (!RootType.IsValid() || !NestedType.IsValid())
	{
		return true;
	}
	TestEqual(TEXT("Root struct kind is struct_wire"), RootType->GetStringField(TEXT("kind")), FString(TEXT("struct_wire")));
	TestEqual(TEXT("Root struct ABI is address-only"), RootType->GetArrayField(TEXT("abi_types")).Num(), 1);
	const TArray<TSharedPtr<FJsonValue>>& RootFields = RootType->GetArrayField(TEXT("fields"));
	const TArray<TSharedPtr<FJsonValue>>& NestedFields = NestedType->GetArrayField(TEXT("fields"));
	TestEqual(TEXT("Root struct projects nested, bool, byte and scalar fields"), RootFields.Num(), 4);
	TestEqual(TEXT("Nested struct projects safe leaf fields"), NestedFields.Num(), 4);
	if (RootFields.Num() == 4 && NestedFields.Num() == 4)
	{
		const TSharedPtr<FJsonObject> NestedField = RootFields[0]->AsObject();
		TestEqual(TEXT("Nested field retains reflection name"), NestedField->GetStringField(TEXT("name")), FString(TEXT("Nested")));
		TestEqual(TEXT("Nested field starts at wire offset zero"), static_cast<int32>(NestedField->GetNumberField(TEXT("wire_offset"))), 0);
		TestEqual(TEXT("Bool field follows nested wire payload"), static_cast<int32>(RootFields[1]->AsObject()->GetNumberField(TEXT("wire_offset"))), 28);
		TestEqual(TEXT("Byte field does not collapse bool wire width"), static_cast<int32>(RootFields[2]->AsObject()->GetNumberField(TEXT("wire_offset"))), 32);
		TestEqual(TEXT("Float field remains aligned after bool and byte"), static_cast<int32>(RootFields[3]->AsObject()->GetNumberField(TEXT("wire_offset"))), 36);
		TestEqual(TEXT("Nested scalar fixture field is Count"), NestedFields[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Count")));
		TestEqual(TEXT("Nested enum fixture field is Mode"), NestedFields[1]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Mode")));
		TestEqual(TEXT("Nested FVector fixture field is Location"), NestedFields[2]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Location")));
		TestEqual(TEXT("Nested UObject fixture field is Target"), NestedFields[3]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Target")));

		const double OriginalRootAlignment = RootType->GetNumberField(TEXT("alignment"));
		const double OriginalRootSize = RootType->GetNumberField(TEXT("size"));
		NestedField->SetNumberField(TEXT("wire_offset"), 4);
		FString TamperedJson;
		FAvidScriptBindingPackageModel TamperedPackage;
		TestTrue(TEXT("Struct-wire offset tamper serializes"), SerializeDescriptor(Root, TamperedJson));
		TestFalse(
			TEXT("Struct-wire offset tamper fails closed"),
			FAvidScriptBindingDescriptorParser::Parse(
				TamperedJson,
				TamperedPackage,
				ErrorCategory,
				ErrorSource));

		NestedField->SetNumberField(TEXT("wire_offset"), 0);
		RootType->SetNumberField(TEXT("alignment"), 1);
		TestTrue(TEXT("Struct-wire alignment tamper serializes"), SerializeDescriptor(Root, TamperedJson));
		TestFalse(
			TEXT("Struct-wire smaller parent alignment fails closed"),
			FAvidScriptBindingDescriptorParser::Parse(
				TamperedJson,
				TamperedPackage,
				ErrorCategory,
				ErrorSource));
		TestTrue(
			TEXT("Parent alignment tamper fails during indexed type identity validation"),
			ErrorSource.StartsWith(TEXT("types[")) && ErrorSource.EndsWith(TEXT("]")));

		RootType->SetNumberField(TEXT("alignment"), OriginalRootAlignment);
		RootType->SetNumberField(TEXT("size"), OriginalRootSize + OriginalRootAlignment);
		TestTrue(TEXT("Struct-wire size tamper serializes"), SerializeDescriptor(Root, TamperedJson));
		TestFalse(
			TEXT("Struct-wire trailing parent size fails closed"),
			FAvidScriptBindingDescriptorParser::Parse(
				TamperedJson,
				TamperedPackage,
				ErrorCategory,
				ErrorSource));
		TestTrue(
			TEXT("Parent size tamper fails during indexed type identity validation"),
			ErrorSource.StartsWith(TEXT("types[")) && ErrorSource.EndsWith(TEXT("]")));
	}

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestTrue(
		TEXT("Schema v9 struct-wire descriptor compiles into an immutable runtime package"),
		FAvidScriptBindingPackage::LoadDescriptor(Json, RuntimePackage, LoadResult));
	if (RuntimePackage.IsValid())
	{
		TArray<FAvidScriptPreparedDynamicBinding> PreparedBindings;
		FString PreparedError;
		TestTrue(
			TEXT("Schema v9 runtime package exposes a prepared recursive codec target"),
			RuntimePackage->BuildPreparedDynamicBindings(PreparedBindings, PreparedError));
		TestEqual(TEXT("Schema v9 fixture has one prepared recursive target"), PreparedBindings.Num(), 1);
		if (PreparedBindings.Num() == 1)
		{
			TestEqual(
				TEXT("Prepared recursive target retains its expected receiver class"),
				PreparedBindings[0].ExpectedClass,
				UAvidScriptCSharpBindingEmitterTestObject::StaticClass());
		}
	}

	FString ObjectLeafJson;
	FAvidScriptBindingDescriptorGenerateResult ObjectLeafResult;
	TestTrue(
		TEXT("Nested non-owner object leaves generate"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.test.struct_wire_object_leaves"),
			{ { OwnerPath, TEXT("StructWireObjectLeaves") } },
			ObjectLeafJson,
			ObjectLeafResult));
	FAvidScriptBindingPackageModel ObjectLeafPackage;
	TestTrue(
		TEXT("Nested object-leaf descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			ObjectLeafJson,
			ObjectLeafPackage,
			ErrorCategory,
			ErrorSource));
	TSharedPtr<const FAvidScriptBindingPackage> ObjectLeafRuntimePackage;
	TestTrue(
		TEXT("Nested object-leaf descriptor loads"),
		FAvidScriptBindingPackage::LoadDescriptor(
			ObjectLeafJson,
			ObjectLeafRuntimePackage,
			LoadResult));
	if (ObjectLeafRuntimePackage.IsValid())
	{
		TArray<FAvidScriptPreparedDynamicBinding> PreparedBindings;
		FString PreparedError;
		TestTrue(
			TEXT("Nested object-leaf codec prepares"),
			ObjectLeafRuntimePackage->BuildPreparedDynamicBindings(
				PreparedBindings,
				PreparedError));
		TestEqual(TEXT("Nested object-leaf package has one prepared target"), PreparedBindings.Num(), 1);
		for (UClass* ExpectedObjectClass : {
			UTexture::StaticClass(),
			UMaterialInterface::StaticClass() })
		{
			const FAvidScriptBindingTypeModel* ObjectType = ObjectLeafPackage.Types.FindByPredicate(
				[ExpectedObjectClass](const FAvidScriptBindingTypeModel& Type)
				{
					return Type.ClassPath == ExpectedObjectClass->GetPathName();
				});
			if (TestNotNull(
				*FString::Printf(TEXT("Nested object leaf publishes %s"), *ExpectedObjectClass->GetPathName()),
				ObjectType))
			{
				TestTrue(
					TEXT("Nested object leaf has a class ordinal"),
					ObjectType->ObjectTypeOrdinal != INDEX_NONE);
				UClass* ResolvedClass = nullptr;
				TestTrue(
					TEXT("Runtime resolves nested object expected class"),
					ObjectLeafRuntimePackage->TryResolveObjectType(
						static_cast<uint32>(ObjectType->ObjectTypeOrdinal),
						ResolvedClass));
				TestEqual(TEXT("Nested object expected class is exact"), ResolvedClass, ExpectedObjectClass);
			}
		}
	}

	FString InheritedJson;
	FAvidScriptBindingDescriptorGenerateResult InheritedResult;
	TestTrue(
		TEXT("Struct-wire projection includes reflected super fields"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.test.struct_wire_inherited"),
			{ { OwnerPath, TEXT("StructWireInheritedRoundTrip") } },
			InheritedJson,
			InheritedResult));
	FAvidScriptBindingPackageModel InheritedPackage;
	TestTrue(
		TEXT("Inherited struct-wire descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			InheritedJson,
			InheritedPackage,
			ErrorCategory,
			ErrorSource));
	const FAvidScriptBindingTypeModel* InheritedType = InheritedPackage.Types.FindByPredicate(
		[](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.CanonicalType == TEXT("struct_wire:")
				+ FAvidScriptStructWireDerivedTestType::StaticStruct()->GetPathName();
		});
	if (TestNotNull(TEXT("Inherited struct-wire type is published"), InheritedType))
	{
		TestEqual(TEXT("Inherited struct contains base and derived fields"), InheritedType->StructFields.Num(), 2);
		if (InheritedType->StructFields.Num() == 2)
		{
			TestTrue(
				TEXT("Base field is retained"),
				InheritedType->StructFields.ContainsByPredicate(
					[](const FAvidScriptBindingStructFieldModel& Field)
					{
						return Field.Name == TEXT("BaseCount");
					}));
			TestTrue(
				TEXT("Derived field is retained"),
				InheritedType->StructFields.ContainsByPredicate(
					[](const FAvidScriptBindingStructFieldModel& Field)
					{
						return Field.Name == TEXT("DerivedWeight");
					}));
		}
	}
	TSharedPtr<const FAvidScriptBindingPackage> InheritedRuntimePackage;
	TestTrue(
		TEXT("Runtime accepts the same inherited field set as the Editor"),
		FAvidScriptBindingPackage::LoadDescriptor(
			InheritedJson,
			InheritedRuntimePackage,
			LoadResult));

	TestFalse(
		TEXT("Unsafe FString struct field is rejected"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.test.struct_wire_unsafe"),
			{ { OwnerPath, TEXT("StructWireUnsafe") } },
			Json,
			Result));
	TestEqual(TEXT("Unsafe struct field reports property rejection"), Result.ErrorCategory, FString(TEXT("unsupported_property")));
	TestTrue(TEXT("Unsafe struct field source includes reflected field"), Result.ErrorSource.Contains(TEXT("Label")));

	TestFalse(
		TEXT("Fixed-array struct field is rejected during Editor projection"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.test.struct_wire_fixed_array"),
			{ { OwnerPath, TEXT("StructWireFixedArray") } },
			Json,
			Result));
	TestEqual(TEXT("Fixed-array field reports property rejection"), Result.ErrorCategory, FString(TEXT("unsupported_property")));
	TestTrue(
		TEXT("Fixed-array field uses a stable diagnostic"),
		Result.ErrorSource.EndsWith(TEXT(".Values:fixed_array:Values[2]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorDelegateEventDescriptorTest,
	"AvidScript.Editor.BindingDescriptor.DelegateEventSchema11",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorDelegateEventDescriptorTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		AAvidScriptEditorDelegateEventTestActor::StaticClass()->GetPathName();
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.test.delegate_event");
	Profile.SelfClassPath = OwnerPath;
	Profile.ExplicitDelegateEvents.Add({ OwnerPath, TEXT("OnScriptSignal") });

	FString Json;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	TestTrue(
		TEXT("Supported multicast delegate event generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			Json,
			SelectionResult,
			GenerateResult));
	TestEqual(
		TEXT("One delegate event is accepted"),
		SelectionResult.AcceptedDelegateEventCount,
		1);
	TestEqual(
		TEXT("Generator reports one delegate event"),
		GenerateResult.DelegateEventCount,
		1);

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	TestTrue(
		TEXT("Delegate event descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			Json,
			Package,
			ErrorCategory,
			ErrorSource));
	TestEqual(TEXT("Delegate event activates schema 11"), Package.SchemaVersion, 11);
	TestEqual(TEXT("Descriptor publishes one event"), Package.DelegateEvents.Num(), 1);
	if (Package.DelegateEvents.Num() == 1)
	{
		const FAvidScriptBindingDelegateEventModel& Event =
			Package.DelegateEvents[0];
		TestEqual(TEXT("Event owner is canonical"), Event.OwnerClass, OwnerPath);
		TestEqual(TEXT("Event member is retained"), Event.UeMember, FString(TEXT("OnScriptSignal")));
		TestEqual(TEXT("Event script name is retained"), Event.ScriptName, FString(TEXT("OnScriptSignal")));
		TestEqual(TEXT("Event source is self"), Event.SourceMode, FString(TEXT("self")));
		TestEqual(TEXT("Event has three parameters"), Event.Parameters.Num(), 3);
		TestEqual(
			TEXT("Event export derives from stable id"),
			Event.ExportName,
			TEXT("avid_on_delegate_") + Event.StableId.Left(16));
	}
	TestTrue(
		TEXT("Event parameter object type enters the type graph"),
		Package.Types.ContainsByPredicate(
			[](const FAvidScriptBindingTypeModel& Type)
			{
				return Type.CanonicalType == TEXT("object:/Script/Engine.Actor");
			}));

	Profile.ExplicitDelegateEvents[0].EventName = TEXT("OnRefOutSignal");
	TestTrue(
		TEXT("Ref/out multicast delegate event generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			Json,
			SelectionResult,
			GenerateResult));
	TestTrue(
		TEXT("Ref/out delegate descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			Json,
			Package,
			ErrorCategory,
			ErrorSource));
	TestEqual(TEXT("Delegate outputs activate schema 19"), Package.SchemaVersion, 19);
	TestEqual(
		TEXT("Delegate outputs record the 58.2 generator"),
		Package.GeneratorVersion,
		FString(TEXT("58.2.0")));
	if (Package.DelegateEvents.Num() == 1)
	{
		TestEqual(
			TEXT("First output parameter is ref"),
			Package.DelegateEvents[0].Parameters[0].Direction,
			FString(TEXT("ref")));
		TestEqual(
			TEXT("Second output parameter is out"),
			Package.DelegateEvents[0].Parameters[1].Direction,
			FString(TEXT("out")));
	}
	FString ReferenceSource;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(
		TEXT("Ref/out delegate emits a typed C# contract"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			Json,
			ReferenceSource,
			ManifestJson,
			EmitResult));
	TestTrue(
		TEXT("Generated event contract preserves ref/out directions"),
		ReferenceSource.Contains(TEXT("OnRefOutSignal"))
			&& ReferenceSource.Contains(TEXT("ref;out")));

	Profile.ExplicitDelegateEvents[0].EventName = TEXT("OnSinglecastSignal");
	Profile.ExplicitDelegateEvents[0].CallbackKind = TEXT("singlecast");
	TestTrue(
		TEXT("Singlecast return/ref/out delegate event generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			Json,
			SelectionResult,
			GenerateResult));
	TestTrue(
		TEXT("Singlecast delegate descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			Json,
			Package,
			ErrorCategory,
			ErrorSource));
	TestEqual(TEXT("Singlecast return activates schema 20"), Package.SchemaVersion, 20);
	TestEqual(
		TEXT("Singlecast return records the current generator"),
		Package.GeneratorVersion,
		FString(TEXT("60.2.0")));
	if (Package.DelegateEvents.Num() == 1)
	{
		const FAvidScriptBindingDelegateEventModel& Event =
			Package.DelegateEvents[0];
		TestEqual(
			TEXT("Singlecast kind is retained"),
			Event.DelegateKind,
			FString(TEXT("singlecast")));
		TestEqual(
			TEXT("Singlecast return type is retained"),
			Event.ReturnValue.CppType,
			FString(TEXT("int32")));
		TestEqual(
			TEXT("Singlecast return direction is retained"),
			Event.ReturnValue.Direction,
			FString(TEXT("return")));
	}
	TestTrue(
		TEXT("Singlecast delegate emits a typed C# contract and bind facade"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			Json,
			ReferenceSource,
			ManifestJson,
			EmitResult));
	TestTrue(
		TEXT("Generated singlecast contract preserves return/ref/out"),
		ReferenceSource.Contains(TEXT("ref;out\", \"global::System.Int32"))
			&& ReferenceSource.Contains(TEXT("BindOnSinglecastSignal"))
			&& ReferenceSource.Contains(TEXT(
				"public int ExecuteOnSinglecastSignal(ref int Value, out int Doubled)")));
	if (Package.DelegateEvents.Num() == 1)
	{
		FAvidScriptBindingDelegateInvokeSpec InvokeSpec;
		TestTrue(
			TEXT("Singlecast facade derives active invoke authority"),
			FAvidScriptBindingDescriptorIdentity::TryMakeDelegateInvokeSpec(
				Package.DelegateEvents[0],
				0,
				InvokeSpec));
		TestTrue(
			TEXT("Singlecast source and manifest publish the prepared import"),
			ReferenceSource.Contains(InvokeSpec.ImportName)
				&& ManifestJson.Contains(InvokeSpec.StableId));
	}

	TSharedPtr<const FAvidScriptBindingPackage> RuntimePackage;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestTrue(
		TEXT("Runtime loads generated schema 20 singlecast descriptor"),
		FAvidScriptBindingPackage::LoadDescriptor(
			Json,
			RuntimePackage,
			LoadResult));
	TArray<FAvidScriptPreparedDelegateEvent> PreparedEvents;
	FString BuildError;
	TestTrue(
		TEXT("Runtime publishes the generated singlecast prepared plan"),
		RuntimePackage.IsValid()
			&& RuntimePackage->BuildPreparedDelegateEvents(
				PreparedEvents,
				BuildError));
	TestEqual(TEXT("Runtime publishes one singlecast plan"), PreparedEvents.Num(), 1);
	TArray<FAvidScriptPreparedDynamicBinding> PreparedInvokeBindings;
	TestTrue(
		TEXT("Runtime publishes one active singlecast invoke plan"),
		RuntimePackage.IsValid()
			&& RuntimePackage->BuildPreparedDynamicBindings(
				PreparedInvokeBindings,
				BuildError));
	TestEqual(
		TEXT("Runtime exposes one generated delegate invoke import"),
		PreparedInvokeBindings.Num(),
		1);
	if (PreparedEvents.Num() == 1)
	{
		TestEqual(
			TEXT("Prepared plan uses singlecast storage"),
			PreparedEvents[0].Signature.Kind,
			EAvidScriptPreparedDelegateKind::Singlecast);
		TestNotNull(
			TEXT("Prepared plan resolves FDelegateProperty"),
			PreparedEvents[0].Signature.SinglecastProperty);
		TestEqual(
			TEXT("Prepared singlecast prepends one output transaction cell"),
			PreparedEvents[0].Signature.ParameterCellCount,
			2u);
		TestEqual(
			TEXT("Prepared singlecast exposes ref, out, and return outputs"),
			PreparedEvents[0].Signature.OutputValueCount,
			3u);
	}

	Profile.ExplicitDelegateEvents[0].EventName = TEXT("OnStringSignal");
	Profile.ExplicitDelegateEvents[0].CallbackKind = TEXT("multicast");
	TestFalse(
		TEXT("String delegate event is rejected"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			Json,
			SelectionResult,
			GenerateResult));
	TestEqual(
		TEXT("String rejection category is stable"),
		GenerateResult.ErrorCategory,
		FString(TEXT("delegate_event_type_unsupported")));

	Profile.ExplicitDelegateEvents[0].EventName = TEXT("OnLargeSignal");
	TestFalse(
		TEXT("Delegate event over eight cells is rejected"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			Json,
			SelectionResult,
			GenerateResult));
	TestEqual(
		TEXT("Oversized event category is stable"),
		GenerateResult.ErrorCategory,
		FString(TEXT("delegate_event_abi_cells_exceeded")));
	return true;
}

#endif
