#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptHash.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorCSharpBindingEmitterTestTypes.h"
#include "BindingGeneration/AvidScriptEditorCSharpBindingRenderer.h"
#include "BindingGeneration/AvidScriptEditorCSharpStateContractRenderer.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

bool SerializeJsonObject(const TSharedRef<FJsonObject>& Object, FString& OutJson)
{
	OutJson.Empty();
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	return FJsonSerializer::Serialize(Object, Writer);
}

FString MakePackageTestRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests"),
		TEXT("BindingPackages"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

FString ExtractStateContractSurface(const FString& Source)
{
	const FString StartToken = TEXT("public enum AvidStateMode");
	const FString EndToken = TEXT("internal static class AvidScriptBindingPackage");
	const int32 StartIndex = Source.Find(StartToken);
	const int32 EndIndex = Source.Find(EndToken);
	if (StartIndex == INDEX_NONE || EndIndex == INDEX_NONE || EndIndex <= StartIndex)
	{
		return FString();
	}
	return Source.Mid(StartIndex, EndIndex - StartIndex);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterDeterminismTest,
	"AvidScript.Editor.CSharpBindingEmitter.Determinism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterDeterminismTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	TestTrue(
		TEXT("Default descriptor generates for C# emission"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(DescriptorJson, DescriptorResult));

	FString FirstSource;
	FString FirstManifest;
	FAvidScriptCSharpBindingEmitResult FirstResult;
	TestTrue(
		TEXT("Default descriptor emits a C# facade"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(DescriptorJson, FirstSource, FirstManifest, FirstResult));
	TestTrue(TEXT("C# emit result succeeds"), FirstResult.bSucceeded);
	TestEqual(TEXT("Emitter preserves package hash"), FirstResult.PackageHash, DescriptorResult.PackageHash);
	TestEqual(TEXT("Emitter preserves binding count"), FirstResult.BindingCount, DescriptorResult.BindingCount);
	TestTrue(TEXT("Generated source has a SHA-256"), FirstResult.SourceHash.Len() == 64);
	TestTrue(TEXT("Generated manifest has a SHA-256"), FirstResult.ManifestHash.Len() == 64);

	FString SecondSource;
	FString SecondManifest;
	FAvidScriptCSharpBindingEmitResult SecondResult;
	TestTrue(
		TEXT("Repeated C# facade emission succeeds"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(DescriptorJson, SecondSource, SecondManifest, SecondResult));
	TestEqual(TEXT("Generated C# bytes are deterministic"), SecondSource, FirstSource);
	TestEqual(TEXT("Generated package manifest bytes are deterministic"), SecondManifest, FirstManifest);
	TestEqual(TEXT("Generated source hash is deterministic"), SecondResult.SourceHash, FirstResult.SourceHash);

	TestTrue(TEXT("Generated facade declares FVector"), FirstSource.Contains(TEXT("public readonly struct FVector")));
	TestTrue(TEXT("Generated facade declares FRotator"), FirstSource.Contains(TEXT("public readonly struct FRotator")));
	TestTrue(TEXT("Generated facade declares AActor"), FirstSource.Contains(TEXT("public readonly struct AActor")));
	TestTrue(TEXT("Generated facade declares USceneComponent"), FirstSource.Contains(TEXT("public readonly struct USceneComponent")));
	TestTrue(TEXT("Generated facade provides UE.Self"), FirstSource.Contains(TEXT("public static AActor Self")));
	TestFalse(TEXT("Raw handle constructor is not public"), FirstSource.Contains(TEXT("public AActor(int slot, int generation)")));
	TestTrue(TEXT("Native imports use the generated module"), FirstSource.Contains(TEXT("[DllImport(\"avidscript\"")));
	TestTrue(TEXT("Generated facade declares AvidStateMode"), FirstSource.Contains(TEXT("public enum AvidStateMode")));
	TestTrue(TEXT("Generated facade declares compatible state mode"), FirstSource.Contains(TEXT("Compatible = 0")));
	TestTrue(TEXT("Generated facade declares explicit state mode"), FirstSource.Contains(TEXT("Explicit = 1")));
	TestTrue(TEXT("Generated facade declares state contract attribute"), FirstSource.Contains(TEXT("public sealed class AvidStateContractAttribute : Attribute")));
	TestTrue(TEXT("State contract attribute targets one class"), FirstSource.Contains(TEXT("[AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]")));
	TestTrue(TEXT("State contract attribute exposes version"), FirstSource.Contains(TEXT("public int Version { get; set; } = 1;")));
	TestTrue(TEXT("Generated facade declares persist attribute"), FirstSource.Contains(TEXT("public sealed class AvidPersistAttribute : Attribute")));
	TestTrue(TEXT("Generated facade declares transient attribute"), FirstSource.Contains(TEXT("public sealed class AvidTransientAttribute : Attribute")));
	TestTrue(TEXT("Generated facade declares repeatable state alias attribute"), FirstSource.Contains(TEXT("public sealed class AvidStateAliasAttribute : Attribute")));
	TestTrue(TEXT("State alias attribute allows multiple field declarations"), FirstSource.Contains(TEXT("[AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = true)]")));

	TArray<FString> StateContractLines;
	FAvidScriptEditorCSharpStateContractRenderer::AppendReferenceSurface(StateContractLines);
	const FString StateContractSurface = FString::Join(StateContractLines, TEXT("\n"));
	TestFalse(TEXT("Independent state contract renderer emits a surface"), StateContractSurface.IsEmpty());
	TestEqual(
		TEXT("Independent renderer surface matches the composed facade"),
		StateContractSurface.TrimEnd(),
		ExtractStateContractSurface(FirstSource).TrimEnd());

	FAvidScriptBindingPackageModel EmptyPackage;
	EmptyPackage.PackageName = TEXT("avidscript.empty");
	EmptyPackage.PackageHash = TEXT("empty-package-hash");
	FString EmptyPackageSource;
	FString EmptyPackageErrorCategory;
	FString EmptyPackageErrorSource;
	TestTrue(
		TEXT("Empty binding package emits a C# reference surface"),
		FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
			EmptyPackage,
			TEXT("empty-descriptor-hash"),
			EmptyPackageSource,
			EmptyPackageErrorCategory,
			EmptyPackageErrorSource));
	TestFalse(TEXT("Empty package state contract surface is present"), ExtractStateContractSurface(EmptyPackageSource).IsEmpty());
	TestEqual(
		TEXT("Empty and reflected packages emit the identical state contract surface"),
		ExtractStateContractSurface(EmptyPackageSource),
		ExtractStateContractSurface(FirstSource));

	TSharedPtr<FJsonObject> Descriptor;
	TestTrue(TEXT("Descriptor remains parseable"), ParseJsonObject(DescriptorJson, Descriptor));
	if (Descriptor.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& Value : Descriptor->GetArrayField(TEXT("bindings")))
		{
			const TSharedPtr<FJsonObject> Binding = Value->AsObject();
			const TSharedPtr<FJsonObject> HostImport = Binding->GetObjectField(TEXT("host_import"));
			TestTrue(
				TEXT("Every descriptor import is emitted once"),
				FirstSource.Contains(FString::Printf(TEXT("EntryPoint = \"%s\""), *HostImport->GetStringField(TEXT("name")))));
		}
	}

	TSharedPtr<FJsonObject> Manifest;
	TestTrue(TEXT("Generated package manifest is valid JSON"), ParseJsonObject(FirstManifest, Manifest));
	if (Manifest.IsValid())
	{
		TestEqual(TEXT("Manifest schema is stable"), Manifest->GetIntegerField(TEXT("schema_version")), 1);
		TestEqual(TEXT("Manifest carries descriptor package hash"), Manifest->GetStringField(TEXT("package_hash")), DescriptorResult.PackageHash);
		TestEqual(TEXT("Manifest carries generated source hash"), Manifest->GetStringField(TEXT("reference_source_sha256")), FirstResult.SourceHash);
		TestEqual(TEXT("Manifest carries descriptor hash"), Manifest->GetStringField(TEXT("descriptor_sha256")), FirstResult.DescriptorHash);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterPropertyGetTest,
	"AvidScript.Editor.CSharpBindingEmitter.PropertyGet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterPropertyGetTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation") }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
		TEXT("Readable property descriptor generates for C# emission"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.engine.property_facade"),
			{},
			Properties,
			DescriptorJson,
			DescriptorResult)))
	{
		return false;
	}

	FString Source;
	FString Manifest;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	if (!TestTrue(
		TEXT("Schema v4 property descriptor emits through the canonical C# pipeline"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			Source,
			Manifest,
			EmitResult)))
	{
		AddError(EmitResult.ErrorCategory + TEXT(": ") + EmitResult.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("Property facade preserves the package hash"), EmitResult.PackageHash, DescriptorResult.PackageHash);
	TestEqual(TEXT("Property facade exposes exactly one binding"), EmitResult.BindingCount, 1);
	TestTrue(
		TEXT("Generated Actor facade exposes a C# float property"),
		Source.Contains(TEXT("public float CustomTimeDilation")));
	TestTrue(
		TEXT("Generated reflected property is read-only"),
		Source.Contains(TEXT("public float CustomTimeDilation\n    {\n        get")));
	TestFalse(
		TEXT("Generated reflected property is not disguised as a method"),
		Source.Contains(TEXT("CustomTimeDilation()")));

	TSharedPtr<FJsonObject> DescriptorObject;
	TSharedPtr<FJsonObject> ManifestObject;
	if (TestTrue(TEXT("Property descriptor remains parseable"), ParseJsonObject(DescriptorJson, DescriptorObject))
		&& TestTrue(TEXT("Property package manifest is parseable"), ParseJsonObject(Manifest, ManifestObject))
		&& DescriptorObject.IsValid()
		&& ManifestObject.IsValid())
	{
		const TSharedPtr<FJsonObject> DescriptorBinding = DescriptorObject->GetArrayField(TEXT("bindings"))[0]->AsObject();
		const TSharedPtr<FJsonObject> DescriptorImport = DescriptorBinding->GetObjectField(TEXT("host_import"));
		const TArray<TSharedPtr<FJsonValue>>& ManifestImports = ManifestObject->GetArrayField(TEXT("required_imports"));
		TestEqual(TEXT("Property manifest declares one required import"), ManifestImports.Num(), 1);
		if (ManifestImports.Num() == 1)
		{
			const TSharedPtr<FJsonObject> ManifestImport = ManifestImports[0]->AsObject();
			TestTrue(
				TEXT("Property getter retains the descriptor entry point"),
				Source.Contains(FString::Printf(
					TEXT("EntryPoint = \"%s\""),
					*DescriptorImport->GetStringField(TEXT("name")))));
			TestEqual(
				TEXT("Property manifest preserves the stable binding id"),
				ManifestImport->GetStringField(TEXT("stable_id")),
				DescriptorBinding->GetStringField(TEXT("stable_id")));
			TestEqual(
				TEXT("Property manifest preserves the host import name"),
				ManifestImport->GetStringField(TEXT("name")),
				DescriptorImport->GetStringField(TEXT("name")));
			TestEqual(
				TEXT("Property manifest preserves the host ABI signature"),
				ManifestImport->GetStringField(TEXT("signature")),
				DescriptorImport->GetStringField(TEXT("signature")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterGameplayProfileTest,
	"AvidScript.Editor.CSharpBindingEmitter.GameplayProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterGameplayProfileTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	FString Source;
	FString Manifest;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(
		TEXT("Engine gameplay profile emits a complete C# facade"),
		FAvidScriptEditorCSharpBindingEmitter::EmitEngineGameplay(
			DescriptorJson,
			Source,
			Manifest,
			EmitResult));
	TestTrue(TEXT("Gameplay profile C# emit succeeds"), EmitResult.bSucceeded);
	TestEqual(TEXT("Gameplay profile preserves every accepted binding"), EmitResult.BindingCount, 342);
	TSharedPtr<FJsonObject> DescriptorObject;
	TestTrue(TEXT("Gameplay descriptor parses"), ParseJsonObject(DescriptorJson, DescriptorObject));
	if (DescriptorObject.IsValid())
	{
		TSharedPtr<FJsonObject> LineOfSightBinding;
		for (const TSharedPtr<FJsonValue>& BindingValue : DescriptorObject->GetArrayField(TEXT("bindings")))
		{
			const TSharedPtr<FJsonObject> BindingObject = BindingValue.IsValid() ? BindingValue->AsObject() : nullptr;
			if (BindingObject.IsValid()
				&& BindingObject->GetStringField(TEXT("owner_class")) == TEXT("/Script/Engine.Controller")
				&& BindingObject->GetStringField(TEXT("ue_member")) == TEXT("LineOfSightTo"))
			{
				LineOfSightBinding = BindingObject;
				break;
			}
		}

		TestNotNull(TEXT("Gameplay descriptor retains Controller line-of-sight binding"), LineOfSightBinding.Get());
		if (LineOfSightBinding.IsValid())
		{
			TSharedPtr<FJsonObject> ViewPointParameter;
			for (const TSharedPtr<FJsonValue>& ParameterValue : LineOfSightBinding->GetArrayField(TEXT("parameters")))
			{
				const TSharedPtr<FJsonObject> ParameterObject = ParameterValue.IsValid() ? ParameterValue->AsObject() : nullptr;
				if (ParameterObject.IsValid() && ParameterObject->GetStringField(TEXT("name")) == TEXT("ViewPoint"))
				{
					ViewPointParameter = ParameterObject;
					break;
				}
			}

			TestNotNull(TEXT("Line-of-sight binding retains ViewPoint parameter"), ViewPointParameter.Get());
			if (ViewPointParameter.IsValid())
			{
				TestFalse(TEXT("Empty reflected default is not published"), ViewPointParameter->GetBoolField(TEXT("has_default")));
				TestFalse(TEXT("Empty reflected default field is not published"), ViewPointParameter->HasField(TEXT("default_value")));
			}
		}
	}
	TestEqual(
		TEXT("Gameplay profile uses a stable package name"),
		EmitResult.PackageName,
		FString(TEXT("avidscript.engine.gameplay")));
	TestFalse(TEXT("Gameplay profile C# source is not empty"), Source.IsEmpty());
	TestFalse(TEXT("Gameplay profile manifest is not empty"), Manifest.IsEmpty());
	TestTrue(TEXT("Gameplay package carries the state contract facade"), Source.Contains(TEXT("public enum AvidStateMode")));
	TestTrue(TEXT("Gameplay package carries the typed input event facade"), Source.Contains(TEXT("public readonly struct InputEvent")));
	TestTrue(TEXT("Input event exposes action and trigger identifiers"), Source.Contains(TEXT("public readonly int ActionId;")) && Source.Contains(TEXT("public readonly int TriggerEvent;")));
	TestTrue(TEXT("Input event exposes a vector value"), Source.Contains(TEXT("public readonly FVector Value;")));
	TestTrue(TEXT("Input event constructor remains compiler-owned"), Source.Contains(TEXT("internal InputEvent(int actionId, int triggerEvent, FVector value)")));
	TestTrue(TEXT("Gameplay package carries the reflected Actor property"), Source.Contains(TEXT("public float CustomTimeDilation")));
	TestTrue(TEXT("Gameplay package carries the reflected component handle property"), Source.Contains(TEXT("public USceneComponent RootComponent")));
	TestFalse(TEXT("Empty reflected default is not emitted as a C# optional argument"), Source.Contains(TEXT("ViewPoint =")));
	const auto FindFacadeBlock = [&Source](const TCHAR* Declaration)
	{
		const int32 BlockStart = Source.Find(Declaration);
		const int32 BlockEnd = BlockStart == INDEX_NONE
			? INDEX_NONE
			: Source.Find(TEXT("\n}\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BlockStart);
		return BlockStart != INDEX_NONE && BlockEnd > BlockStart
			? Source.Mid(BlockStart, BlockEnd - BlockStart)
			: FString();
	};
	const FString ActorComponentBlock = FindFacadeBlock(TEXT("public readonly struct UActorComponent"));
	const FString PrimitiveComponentBlock = FindFacadeBlock(TEXT("public readonly struct UPrimitiveComponent"));
	const FString PawnBlock = FindFacadeBlock(TEXT("public readonly struct APawn"));
	const FString ControllerBlock = FindFacadeBlock(TEXT("public readonly struct AController"));
	TestFalse(TEXT("Gameplay package declares ActorComponent facade"), ActorComponentBlock.IsEmpty());
	TestFalse(TEXT("Gameplay package declares PrimitiveComponent facade"), PrimitiveComponentBlock.IsEmpty());
	TestFalse(TEXT("Gameplay package declares Pawn facade"), PawnBlock.IsEmpty());
	TestFalse(TEXT("Gameplay package declares Controller facade"), ControllerBlock.IsEmpty());
	if (!ActorComponentBlock.IsEmpty())
	{
		TestTrue(TEXT("ActorComponent facade contains a declared lifecycle method"), ActorComponentBlock.Contains(TEXT("Deactivate(")));
	}
	if (!PrimitiveComponentBlock.IsEmpty())
	{
		TestTrue(TEXT("PrimitiveComponent facade contains a declared physics method"), PrimitiveComponentBlock.Contains(TEXT("SetSimulatePhysics(")));
	}
	if (!PawnBlock.IsEmpty())
	{
		TestTrue(TEXT("Pawn facade contains Pawn-declared movement input"), PawnBlock.Contains(TEXT("AddMovementInput")));
		TestFalse(TEXT("Pawn facade does not copy Actor location query"), PawnBlock.Contains(TEXT("GetActorLocation(")));
	}
	if (!ControllerBlock.IsEmpty())
	{
		TestTrue(TEXT("Controller facade retains Controller line-of-sight call"), ControllerBlock.Contains(TEXT("LineOfSightTo(")));
	}
	TestTrue(TEXT("Gameplay object proxies expose a structural null check"), Source.Contains(TEXT("public bool IsNull => Slot == 0 && Generation == 0;")));
	TestTrue(TEXT("Gameplay object proxies expose a structural handle check"), Source.Contains(TEXT("public bool HasHandle => Slot > 0 && Generation > 0;")));

	FString RepeatedDescriptor;
	FString RepeatedSource;
	FString RepeatedManifest;
	FAvidScriptCSharpBindingEmitResult RepeatedResult;
	TestTrue(
		TEXT("Repeated gameplay profile emission succeeds"),
		FAvidScriptEditorCSharpBindingEmitter::EmitEngineGameplay(
			RepeatedDescriptor,
			RepeatedSource,
			RepeatedManifest,
			RepeatedResult));
	TestEqual(TEXT("Gameplay descriptor bytes are deterministic"), RepeatedDescriptor, DescriptorJson);
	TestEqual(TEXT("Gameplay facade bytes are deterministic"), RepeatedSource, Source);
	TestEqual(TEXT("Gameplay manifest bytes are deterministic"), RepeatedManifest, Manifest);

	TSharedPtr<FJsonObject> ManifestObject;
	TestTrue(TEXT("Gameplay manifest parses"), ParseJsonObject(Manifest, ManifestObject));
	if (ManifestObject.IsValid())
	{
		TestEqual(
			TEXT("Gameplay manifest declares all reflected imports"),
			ManifestObject->GetArrayField(TEXT("required_imports")).Num(),
			342);
	}

	const FString OutputRoot = MakePackageTestRoot();
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);
	FAvidScriptCSharpBindingEmitResult PublishResult;
	TestTrue(
		TEXT("Gameplay package publishes atomically"),
		FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(OutputRoot, PublishResult));
	TestTrue(TEXT("Gameplay package descriptor exists"), FPaths::FileExists(PublishResult.DescriptorPath));
	TestTrue(TEXT("Gameplay package facade exists"), FPaths::FileExists(PublishResult.ReferenceSourcePath));
	TestTrue(TEXT("Gameplay package manifest exists"), FPaths::FileExists(PublishResult.ManifestPath));

	FAvidScriptCSharpBindingEmitResult ReuseResult;
	TestTrue(
		TEXT("Repeated gameplay package publish succeeds"),
		FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(OutputRoot, ReuseResult));
	TestTrue(TEXT("Repeated gameplay publish reuses immutable bytes"), ReuseResult.bReusedExistingPackage);
	TestEqual(TEXT("Gameplay publish reuses the content address"), ReuseResult.PackageDirectory, PublishResult.PackageDirectory);
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterProjectionTest,
	"AvidScript.Editor.CSharpBindingEmitter.Projection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterProjectionTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedFunctionSelection> Selections{
		{ TEXT("/Script/Engine.SceneComponent"), TEXT("SetVisibility") },
		{ TEXT("/Script/Engine.SceneComponent"), TEXT("K2_GetComponentToWorld") },
		{ TEXT("/Script/Engine.KismetMathLibrary"), TEXT("VLerp") },
		{ TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"), TEXT("ReservedHandleNames") },
		{ TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"), TEXT("OptionalProjection") },
		{ TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"), TEXT("InvalidScalarDefault") },
		{ TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"), TEXT("FractionalIntegerDefault") },
		{ TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"), TEXT("InvalidFloatDefault") },
		{ TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"), TEXT("StaticTransform") },
		{ TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterStaticOwnerTestObject"), TEXT("HasValue") }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	TestTrue(
		TEXT("Projection fixture descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.projection"),
			Selections,
			DescriptorJson,
			DescriptorResult));

	TSharedPtr<FJsonObject> ProjectionDescriptor;
	TestTrue(TEXT("Projection descriptor remains parseable"), ParseJsonObject(DescriptorJson, ProjectionDescriptor));
	if (ProjectionDescriptor.IsValid())
	{
		const TSharedPtr<FJsonValue>* EnumTypeValue = ProjectionDescriptor->GetArrayField(TEXT("types")).FindByPredicate(
			[](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid() && Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("enum");
			});
		TestNotNull(TEXT("Projection descriptor contains its reflected enum type"), EnumTypeValue);
		if (EnumTypeValue != nullptr)
		{
			const TSharedPtr<FJsonObject> EnumType = (*EnumTypeValue)->AsObject();
			TestEqual(TEXT("Enum descriptor retains two visible members"), EnumType->GetArrayField(TEXT("enum_values")).Num(), 2);
			TestNotEqual(
				TEXT("Enum stable identity includes its member definition"),
				EnumType->GetStringField(TEXT("stable_id")),
				FAvidScriptHash::Sha256HexUtf8(EnumType->GetStringField(TEXT("canonical_type"))));
		}
	}

	FString Source;
	FString Manifest;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(
		TEXT("Projection fixture emits C#"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(DescriptorJson, Source, Manifest, EmitResult));
	TestTrue(TEXT("Generated facade declares FTransform"), Source.Contains(TEXT("public readonly struct FTransform")));
	TestTrue(TEXT("Static UE owner becomes a static C# class"), Source.Contains(TEXT("public static class UKismetMathLibrary")));
	TestTrue(
		TEXT("Static FVector projection is typed"),
		Source.Contains(TEXT("public static FVector VLerp(FVector A, FVector B, float Alpha)")));
	TestTrue(
		TEXT("Reflected bool default is emitted"),
		Source.Contains(TEXT("public void SetVisibility(bool bNewVisibility, bool bPropagateToChildren = false)")));
	TestTrue(
		TEXT("FTransform return projection is typed"),
		Source.Contains(TEXT("public FTransform GetWorldTransform()")));
	TestTrue(
		TEXT("Proxy receiver cannot be shadowed by Slot and Generation parameters"),
		Source.Contains(TEXT("this.Slot, this.Generation, Slot, Generation, out __returnValue")));
	TestTrue(TEXT("Reflected enum emits Primary"), Source.Contains(TEXT("    Primary = 0,")));
	TestTrue(TEXT("Reflected enum emits Secondary"), Source.Contains(TEXT("    Secondary = 1,")));
	TestTrue(
		TEXT("Trailing enum default preserves the complete optional parameter chain"),
		Source.Contains(TEXT("public void OptionalProjection(bool bEnabled = false, EAvidScriptCSharpEmitterTestMode Mode = EAvidScriptCSharpEmitterTestMode.Primary)")));
	TestTrue(
		TEXT("Partially parsed integer metadata is rejected"),
		Source.Contains(TEXT("public void InvalidScalarDefault(int Count)")));
	TestFalse(
		TEXT("Partially parsed integer metadata never becomes a C# default"),
		Source.Contains(TEXT("InvalidScalarDefault(int Count =")));
	TestTrue(
		TEXT("Fractional integer metadata is rejected"),
		Source.Contains(TEXT("public void FractionalIntegerDefault(int Count)")));
	TestFalse(
		TEXT("Fractional integer metadata never becomes a C# default"),
		Source.Contains(TEXT("FractionalIntegerDefault(int Count =")));
	TestTrue(
		TEXT("Partially parsed floating-point metadata is rejected"),
		Source.Contains(TEXT("public void InvalidFloatDefault(float Ratio)")));
	TestFalse(
		TEXT("Partially parsed floating-point metadata never becomes a C# default"),
		Source.Contains(TEXT("InvalidFloatDefault(float Ratio =")));
	TestTrue(
		TEXT("Static FTransform boundary is represented in generated C#"),
		Source.Contains(TEXT("public static FTransform StaticTransform(FTransform Input)")));
	TestTrue(
		TEXT("Object types used as values remain handle structs even with only static selected methods"),
		Source.Contains(TEXT("public readonly struct UAvidScriptCSharpBindingEmitterStaticOwnerTestObject")));
	TestTrue(
		TEXT("Static method on an object-value owner remains callable"),
		Source.Contains(TEXT("public static bool HasValue(UAvidScriptCSharpBindingEmitterStaticOwnerTestObject Value)")));

	const FString ProjectionReferencePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptGeneratedBindingsTests"),
		TEXT("P42.2ProjectionSmoke.generated.cs")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProjectionReferencePath), true);
	TestTrue(
		TEXT("Projection reference source is published for Roslyn semantic verification"),
		FFileHelper::SaveStringToFile(
			Source,
			*ProjectionReferencePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	AddInfo(FString::Printf(TEXT("Projection C# reference source: %s"), *ProjectionReferencePath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterFNameTest,
	"AvidScript.Editor.CSharpBindingEmitter.FName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterFNameTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	TestTrue(
		TEXT("FName facade descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.fname.facade"),
			{ { TEXT("/Script/Engine.Actor"), TEXT("ActorHasTag") } },
			DescriptorJson,
			DescriptorResult));
	if (!DescriptorResult.bSucceeded)
	{
		return false;
	}

	FString Source;
	FString Manifest;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(TEXT("FName facade emits"), FAvidScriptEditorCSharpBindingEmitter::Emit(DescriptorJson, Source, Manifest, EmitResult));
	TestTrue(TEXT("FName facade maps the public input to string"), Source.Contains(TEXT("public bool ActorHasTag(string Tag)")));
	TestTrue(TEXT("FName facade maps native input storage to string"),
		Source.Contains(TEXT("internal static extern int Invoke0000(int selfSlot, int selfGeneration, string p0, out int returnValue);")));
	const FString FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/BindingGeneration/P48_4_FNameActorHasTag.generated.cs")));
	FString FixtureSource;
	if (!TestTrue(TEXT("FName generated facade fixture loads"), FFileHelper::LoadFileToString(FixtureSource, *FixturePath)))
	{
		return false;
	}
	TestEqual(TEXT("FName generated facade matches the C# source-to-WASM fixture byte for byte"), Source, FixtureSource);

	FAvidScriptBindingPackageModel Package;
	FString ParseErrorCategory;
	FString ParseErrorSource;
	if (!TestTrue(TEXT("FName descriptor parses into a renderer package"),
		FAvidScriptBindingDescriptorParser::Parse(DescriptorJson, Package, ParseErrorCategory, ParseErrorSource)))
	{
		return false;
	}
	TestEqual(TEXT("FName renderer package has one binding"), Package.Bindings.Num(), 1);
	if (Package.Bindings.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("FName renderer binding has one parameter"), Package.Bindings[0].Parameters.Num(), 1);
	if (Package.Bindings[0].Parameters.Num() != 1)
	{
		return false;
	}
	for (const FString& Mutation : {
		TEXT("canonical"), TEXT("kind"), TEXT("cpp_type"), TEXT("size"), TEXT("alignment"), TEXT("abi"),
		TEXT("direction_ref"), TEXT("direction_out"), TEXT("direction_return") })
	{
		FAvidScriptBindingPackageModel Tampered = Package;
		FAvidScriptBindingTypeModel* FNameType = Tampered.Types.FindByPredicate(
			[](const FAvidScriptBindingTypeModel& Type) { return Type.CanonicalType == TEXT("name:fname"); });
		FAvidScriptBindingValueModel* FNameValue = Tampered.Bindings[0].Parameters.FindByPredicate(
			[](const FAvidScriptBindingValueModel& Value) { return Value.CanonicalType == TEXT("name:fname"); });
		TestNotNull(TEXT("FName type remains available for tamper"), FNameType);
		TestNotNull(TEXT("FName value remains available for tamper"), FNameValue);
		if (FNameType == nullptr || FNameValue == nullptr)
		{
			continue;
		}
		if (Mutation == TEXT("canonical")) { FNameType->CanonicalType = TEXT("name:other"); FNameValue->CanonicalType = FNameType->CanonicalType; }
		else if (Mutation == TEXT("kind")) { FNameType->Kind = TEXT("name_utf16"); FNameValue->Kind = FNameType->Kind; }
		else if (Mutation == TEXT("cpp_type")) { FNameType->CppType = TEXT("FString"); FNameValue->CppType = FNameType->CppType; }
		else if (Mutation == TEXT("size")) { FNameType->Size = 8; }
		else if (Mutation == TEXT("alignment")) { FNameType->Alignment = 8; }
		else if (Mutation == TEXT("abi")) { FNameType->AbiTypes = { TEXT("f") }; FNameValue->AbiTypes = FNameType->AbiTypes; }
		else if (Mutation == TEXT("direction_ref")) { FNameValue->Direction = TEXT("ref"); }
		else if (Mutation == TEXT("direction_out")) { FNameValue->Direction = TEXT("out"); }
		else { FNameValue->Direction = TEXT("return"); }

		FString TamperedSource;
		FString ErrorCategory;
		FString ErrorSource;
		TestFalse(TEXT("Tampered FName metadata fails closed: ") + Mutation,
			FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
				Tampered,
				TEXT("fName-descriptor-hash"),
				TamperedSource,
				ErrorCategory,
				ErrorSource));
		TestTrue(TEXT("Tampered FName metadata has a stable rejection category: ") + Mutation,
			ErrorCategory == TEXT("unsupported_csharp_type") || ErrorCategory == TEXT("abi_signature_mismatch"));
		TestTrue(TEXT("Tampered FName metadata emits no partial source: ") + Mutation, TamperedSource.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterFailureAndPublishTest,
	"AvidScript.Editor.CSharpBindingEmitter.FailureAndPublish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterFailureAndPublishTest::RunTest(const FString& Parameters)
{
	FString CollisionDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult CollisionDescriptorResult;
	TestTrue(
		TEXT("Collision fixture descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.collision"),
			{
				{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetActorLocation") },
				{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetActorRotation") }
			},
			CollisionDescriptorJson,
			CollisionDescriptorResult));

	TSharedPtr<FJsonObject> CollisionDescriptor;
	TestTrue(TEXT("Collision fixture parses"), ParseJsonObject(CollisionDescriptorJson, CollisionDescriptor));
	if (CollisionDescriptor.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& Bindings = CollisionDescriptor->GetArrayField(TEXT("bindings"));
		Bindings[0]->AsObject()->SetStringField(TEXT("script_name"), TEXT("GetValue"));
		Bindings[1]->AsObject()->SetStringField(TEXT("script_name"), TEXT("GetValue"));
		TestTrue(TEXT("Collision fixture serializes"), SerializeJsonObject(CollisionDescriptor.ToSharedRef(), CollisionDescriptorJson));
	}

	FString Source;
	FString Manifest;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestFalse(
		TEXT("Non-canonical reflected descriptor fails closed"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(CollisionDescriptorJson, Source, Manifest, EmitResult));
	TestEqual(TEXT("Descriptor tamper reports a stable category"), EmitResult.ErrorCategory, FString(TEXT("descriptor_not_canonical")));
	TestTrue(TEXT("Descriptor tamper emits no partial C#"), Source.IsEmpty());
	TestTrue(TEXT("Descriptor tamper emits no partial manifest"), Manifest.IsEmpty());

	FString AbiDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult AbiDescriptorResult;
	TestTrue(
		TEXT("ABI tamper fixture descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(AbiDescriptorJson, AbiDescriptorResult));
	TSharedPtr<FJsonObject> AbiDescriptor;
	TestTrue(TEXT("ABI tamper fixture parses"), ParseJsonObject(AbiDescriptorJson, AbiDescriptor));
	bool bAbiMutated = false;
	if (AbiDescriptor.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& BindingValue : AbiDescriptor->GetArrayField(TEXT("bindings")))
		{
			for (const TSharedPtr<FJsonValue>& ParameterValue : BindingValue->AsObject()->GetArrayField(TEXT("parameters")))
			{
				const TSharedPtr<FJsonObject> Parameter = ParameterValue->AsObject();
				const TArray<TSharedPtr<FJsonValue>>& AbiTypes = Parameter->GetArrayField(TEXT("abi_types"));
				if (!AbiTypes.IsEmpty())
				{
					TArray<TSharedPtr<FJsonValue>> MutatedAbiTypes = AbiTypes;
					const FString OriginalAbiType = AbiTypes[0]->AsString();
					MutatedAbiTypes[0] = MakeShared<FJsonValueString>(OriginalAbiType == TEXT("i") ? TEXT("f") : TEXT("i"));
					Parameter->SetArrayField(TEXT("abi_types"), MoveTemp(MutatedAbiTypes));
					bAbiMutated = true;
					break;
				}
			}
			if (bAbiMutated)
			{
				break;
			}
		}
		TestTrue(TEXT("ABI tamper fixture changes a projected value"), bAbiMutated);
		TestTrue(TEXT("ABI tamper fixture serializes"), SerializeJsonObject(AbiDescriptor.ToSharedRef(), AbiDescriptorJson));
	}
	FAvidScriptCSharpBindingEmitResult AbiResult;
	TestFalse(
		TEXT("Value ABI metadata cannot diverge from its canonical type"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(AbiDescriptorJson, Source, Manifest, AbiResult));
	TestEqual(TEXT("ABI metadata mismatch is a descriptor contract failure"), AbiResult.ErrorCategory, FString(TEXT("descriptor_contract_invalid")));

	FString ReservedMemberDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult ReservedMemberDescriptorResult;
	TestTrue(
		TEXT("Reserved generated member collision fixture descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.generated-member-collision"),
			{
				{ TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"), TEXT("IsValid") }
			},
			ReservedMemberDescriptorJson,
			ReservedMemberDescriptorResult));
	FAvidScriptCSharpBindingEmitResult ReservedMemberResult;
	TestFalse(
		TEXT("Reflected methods cannot collide with generated proxy members"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			ReservedMemberDescriptorJson,
			Source,
			Manifest,
			ReservedMemberResult));
	TestEqual(
		TEXT("Generated member collision reports a stable category"),
		ReservedMemberResult.ErrorCategory,
		FString(TEXT("generated_member_collision")));

	const FString OutputRoot = MakePackageTestRoot();
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);
	FAvidScriptCSharpBindingEmitResult PublishResult;
	TestTrue(
		TEXT("Default generated package publishes"),
		FAvidScriptEditorCSharpBindingEmitter::PublishDefault(OutputRoot, PublishResult));
	TestTrue(TEXT("Published descriptor exists"), FPaths::FileExists(PublishResult.DescriptorPath));
	TestTrue(TEXT("Published reference source exists"), FPaths::FileExists(PublishResult.ReferenceSourcePath));
	TestTrue(TEXT("Published manifest exists"), FPaths::FileExists(PublishResult.ManifestPath));
	TestTrue(TEXT("Package directory is content addressed by the complete artifact"), PublishResult.PackageDirectory.EndsWith(PublishResult.ManifestHash));

	FAvidScriptCSharpBindingEmitResult ReuseResult;
	TestTrue(
		TEXT("Identical content-addressed package publish is idempotent"),
		FAvidScriptEditorCSharpBindingEmitter::PublishDefault(OutputRoot, ReuseResult));
	TestTrue(TEXT("Second publish reuses immutable package"), ReuseResult.bReusedExistingPackage);
	TestEqual(TEXT("Second publish returns the same package directory"), ReuseResult.PackageDirectory, PublishResult.PackageDirectory);

	TArray<uint8> OriginalSourceBytes;
	TestTrue(TEXT("Published source bytes are readable"), FFileHelper::LoadFileToArray(OriginalSourceBytes, *PublishResult.ReferenceSourcePath));
	TArray<uint8> BomSourceBytes{ 0xEF, 0xBB, 0xBF };
	BomSourceBytes.Append(OriginalSourceBytes);
	TestTrue(TEXT("Conflict fixture can add a UTF-8 BOM"), FFileHelper::SaveArrayToFile(BomSourceBytes, *PublishResult.ReferenceSourcePath));
	FAvidScriptCSharpBindingEmitResult ConflictResult;
	TestFalse(
		TEXT("Byte-distinct immutable package fails closed"),
		FAvidScriptEditorCSharpBindingEmitter::PublishDefault(OutputRoot, ConflictResult));
	TestEqual(TEXT("Byte conflict reports stable category"), ConflictResult.ErrorCategory, FString(TEXT("package_conflict")));
	TArray<uint8> PreservedConflictBytes;
	TestTrue(TEXT("Conflict source bytes remain readable"), FFileHelper::LoadFileToArray(PreservedConflictBytes, *PublishResult.ReferenceSourcePath));
	TestTrue(TEXT("Conflict publish does not overwrite existing bytes"), PreservedConflictBytes == BomSourceBytes);

	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);

	FAvidScriptCSharpBindingEmitResult ProjectPublishResult;
	TestTrue(
		TEXT("Default project binding package publishes for Roslyn consumption"),
		FAvidScriptEditorCSharpBindingEmitter::PublishDefault(ProjectPublishResult));
	TestTrue(TEXT("Project reference source exists"), FPaths::FileExists(ProjectPublishResult.ReferenceSourcePath));
	AddInfo(FString::Printf(TEXT("Generated C# reference source: %s"), *ProjectPublishResult.ReferenceSourcePath));
	return true;
}

#endif
