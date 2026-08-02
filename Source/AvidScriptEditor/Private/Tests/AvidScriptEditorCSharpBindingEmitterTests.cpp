#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptHash.h"
#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptObjectTypeBinding.h"
#include "AvidScriptSceneAttachmentBinding.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorCSharpBindingEmitterTestTypes.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "BindingGeneration/AvidScriptEditorCSharpDefaultValueFormatter.h"
#include "BindingGeneration/AvidScriptEditorCSharpBindingRenderer.h"
#include "BindingGeneration/AvidScriptEditorCSharpStateContractRenderer.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Actor.h"
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

bool SerializeTamperedDescriptorType(
	const FString& DescriptorJson,
	const FAvidScriptBindingPackageModel& TamperedPackage,
	const FString& CanonicalType,
	TFunctionRef<void(FJsonObject&)> MutateType,
	FString& OutJson)
{
	TSharedPtr<FJsonObject> Root;
	if (!ParseJsonObject(DescriptorJson, Root))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* TypeValues = nullptr;
	if (!Root->TryGetArrayField(TEXT("types"), TypeValues) || TypeValues == nullptr)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& TypeValue : *TypeValues)
	{
		const TSharedPtr<FJsonObject> TypeObject = TypeValue.IsValid()
			? TypeValue->AsObject()
			: nullptr;
		if (TypeObject.IsValid()
			&& TypeObject->GetStringField(TEXT("canonical_type")) == CanonicalType)
		{
			MutateType(*TypeObject);
			Root->SetStringField(TEXT("selection_hash"), TamperedPackage.SelectionHash);
			Root->SetStringField(TEXT("package_hash"), TamperedPackage.PackageHash);
			return SerializeJsonObject(Root.ToSharedRef(), OutJson);
		}
	}
	return false;
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

FString ExtractTypedProjectFacadeSurface(const FString& Source)
{
	const FString StartToken = TEXT("[StructLayout(LayoutKind.Sequential)]\npublic readonly struct TSubclassOfAActor");
	const int32 StartIndex = Source.Find(StartToken);
	return StartIndex == INDEX_NONE ? FString() : Source.Mid(StartIndex);
}

FString ExtractNameStringProjectionSurface(const FString& Source)
{
	TArray<FString> Lines;
	Source.ParseIntoArrayLines(Lines);
	TArray<FString> ProjectionLines;
	for (FString& Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.StartsWith(TEXT("public "))
			&& (Line.Contains(TEXT("FName")) || Line.Contains(TEXT("FString"))
				|| Line.Contains(TEXT("ReadableF"))))
		{
			ProjectionLines.Add(MoveTemp(Line));
		}
	}
	ProjectionLines.Sort();
	return FString::Join(ProjectionLines, TEXT("\n"));
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
	TestTrue(TEXT("Generated facade provides one-shot timers"), FirstSource.Contains(TEXT("public static int SetTimer(float delaySeconds, int callbackId)")));
	TestTrue(TEXT("Generated facade provides timer cancellation"), FirstSource.Contains(TEXT("public static bool CancelTimer(int timerHandle)")));
	TestTrue(TEXT("Generated facade imports the timer service"), FirstSource.Contains(TEXT("EntryPoint = \"timer_set_once\"")));
	TestTrue(TEXT("Generated facade imports timer cancellation"), FirstSource.Contains(TEXT("EntryPoint = \"timer_cancel\"")));
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
	FAvidScriptEditorCSharpBindingEmitterStructWireTest,
	"AvidScript.Editor.CSharpBindingEmitter.StructWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterStructWireTest::RunTest(const FString& Parameters)
{
	const FString OwnerPath = UAvidScriptCSharpBindingEmitterTestObject::StaticClass()->GetPathName();
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
			TEXT("Recursive fixed-width USTRUCT descriptor generates for C# emission"),
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.test.struct_wire.csharp"),
				{ { OwnerPath, TEXT("StructWireRoundTrip") } },
				DescriptorJson,
				DescriptorResult)))
	{
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ParseErrorCategory;
	FString ParseErrorSource;
	if (!TestTrue(
			TEXT("Recursive fixed-width USTRUCT descriptor parses for C# emission"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Package,
				ParseErrorCategory,
				ParseErrorSource)))
	{
		return false;
	}
	TestEqual(TEXT("Recursive fixed-width USTRUCT descriptor uses schema v9"), Package.SchemaVersion, 9);
	if (!TestEqual(TEXT("Recursive fixed-width USTRUCT fixture has one source binding"), Package.Bindings.Num(), 1))
	{
		return false;
	}

	FAvidScriptBindingFunctionModel& RoundTrip = Package.Bindings[0];
	if (!TestTrue(
			TEXT("Recursive fixed-width USTRUCT fixture has one source parameter"),
			RoundTrip.Parameters.Num() == 1))
	{
		return false;
	}

	FAvidScriptBindingFunctionModel Directions = RoundTrip;
	Directions.Ordinal = 1;
	Directions.ScriptName = TEXT("StructWireDirections");
	Directions.UeMember = TEXT("StructWireDirections");
	Directions.CanonicalIdentity = TEXT("test.struct_wire.directions");
	Directions.HostImport.Name = TEXT("test_struct_wire_directions");
	Directions.Parameters[0].Name = TEXT("Value");
	FAvidScriptBindingValueModel ConstReference = Directions.Parameters[0];
	ConstReference.Name = TEXT("ConstReference");
	ConstReference.Direction = TEXT("const_ref");
	FAvidScriptBindingValueModel InOut = Directions.Parameters[0];
	InOut.Name = TEXT("InOut");
	InOut.Direction = TEXT("ref");
	FAvidScriptBindingValueModel Output = Directions.Parameters[0];
	Output.Name = TEXT("Output");
	Output.Direction = TEXT("out");
	Directions.Parameters = { Directions.Parameters[0], ConstReference, InOut, Output };
	Directions.HostImport.Signature = TEXT("(iiiiiii)i");
	Package.Bindings.Add(Directions);

	FAvidScriptBindingFunctionModel Getter = RoundTrip;
	Getter.Ordinal = 2;
	Getter.BindingKind = TEXT("property_get");
	Getter.ScriptName = TEXT("StructWireProperty");
	Getter.UeMember = TEXT("StructWireProperty");
	Getter.CanonicalIdentity = TEXT("test.struct_wire.property_get");
	Getter.HostImport.Name = TEXT("test_struct_wire_property_get");
	Getter.HostImport.Signature = TEXT("(iii)i");
	Getter.Parameters.Reset();
	Package.Bindings.Add(Getter);

	FAvidScriptBindingFunctionModel Setter = RoundTrip;
	Setter.Ordinal = 3;
	Setter.BindingKind = TEXT("property_set");
	Setter.bConst = false;
	Setter.ScriptName = TEXT("StructWireProperty");
	Setter.UeMember = TEXT("StructWireProperty");
	Setter.CanonicalIdentity = TEXT("test.struct_wire.property_set");
	Setter.HostImport.Name = TEXT("test_struct_wire_property_set");
	Setter.HostImport.Signature = TEXT("(iii)i");
	Setter.ReturnValue = {};
	Setter.ReturnValue.CanonicalType = TEXT("void");
	Setter.Parameters[0].Name = TEXT("Value");
	Setter.Parameters[0].Direction = TEXT("value");
	Package.Bindings.Add(Setter);

	FString FirstSource;
	FString FirstErrorCategory;
	FString FirstErrorSource;
	if (!TestTrue(
			TEXT("Recursive fixed-width USTRUCT facade emits"),
			FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
				Package,
				TEXT("struct-wire-descriptor-hash"),
				FirstSource,
				FirstErrorCategory,
				FirstErrorSource)))
	{
		AddError(FirstErrorCategory + TEXT(": ") + FirstErrorSource);
		return false;
	}
	TestTrue(
		TEXT("Nested struct-wire type has its exact explicit layout"),
		FirstSource.Contains(TEXT("[StructLayout(LayoutKind.Explicit, Size = 28)]\npublic readonly struct FAvidScriptStructWireNestedTestType")));
	TestTrue(
		TEXT("Root struct-wire type has its exact explicit layout"),
		FirstSource.Contains(TEXT("[StructLayout(LayoutKind.Explicit, Size = 40)]\npublic readonly struct FAvidScriptStructWireRootTestType")));
	TestTrue(TEXT("Nested struct-wire scalar field has its wire offset"), FirstSource.Contains(TEXT("[FieldOffset(0)]\n    public readonly int Count;")));
	TestTrue(TEXT("Nested struct-wire enum field has its wire offset"), FirstSource.Contains(TEXT("[FieldOffset(4)]\n    public readonly EAvidScriptCSharpEmitterTestMode Mode;")));
	TestTrue(TEXT("Nested struct-wire FVector field has its wire offset"), FirstSource.Contains(TEXT("[FieldOffset(8)]\n    public readonly FVector Location;")));
	TestTrue(TEXT("Nested struct-wire object field has its wire offset"), FirstSource.Contains(TEXT("[FieldOffset(20)]\n    public readonly UObject Target;")));
	TestTrue(TEXT("Root struct-wire nested field has its wire offset"), FirstSource.Contains(TEXT("[FieldOffset(0)]\n    public readonly FAvidScriptStructWireNestedTestType Nested;")));
	TestTrue(TEXT("Root struct-wire bool uses four-byte wire storage"), FirstSource.Contains(TEXT("[FieldOffset(28)]\n    private readonly int __avidscript_bool_1;\n    public bool bEnabled => __avidscript_bool_1 != 0;")));
	TestTrue(TEXT("Root struct-wire byte retains its post-bool offset"), FirstSource.Contains(TEXT("[FieldOffset(32)]\n    public readonly byte Level;")));
	TestTrue(TEXT("Root struct-wire scalar field has its wire offset"), FirstSource.Contains(TEXT("[FieldOffset(36)]\n    public readonly float Weight;")));
	TestTrue(TEXT("Root struct-wire bool constructor stores i32 wire truth"), FirstSource.Contains(TEXT("this.__avidscript_bool_1 = bEnabled ? 1 : 0;")));
	TestTrue(
		TEXT("Struct-wire method keeps ordinary public value types with mixed directions"),
		FirstSource.Contains(TEXT("public FAvidScriptStructWireRootTestType StructWireDirections(FAvidScriptStructWireRootTestType Value, FAvidScriptStructWireRootTestType ConstReference, ref FAvidScriptStructWireRootTestType InOut, out FAvidScriptStructWireRootTestType Output)")));
	TestTrue(
		TEXT("Struct-wire method maps value and const-ref inputs to native in parameters"),
		FirstSource.Contains(TEXT("in FAvidScriptStructWireRootTestType p0_Value, in FAvidScriptStructWireRootTestType p1_ConstReference")));
	TestTrue(
		TEXT("Struct-wire method maps ref out and return to native by-reference parameters"),
		FirstSource.Contains(TEXT("ref FAvidScriptStructWireRootTestType p2_InOut, out FAvidScriptStructWireRootTestType p3_Output, out FAvidScriptStructWireRootTestType returnValue")));
	TestTrue(
		TEXT("Struct-wire method invokes native value and const-ref inputs with in"),
		FirstSource.Contains(TEXT("in Value, in ConstReference, ref InOut, out Output, out __returnValue")));
	TestTrue(
		TEXT("Struct-wire property getter uses native out storage"),
		FirstSource.Contains(TEXT("Invoke0002(int selfSlot, int selfGeneration, out FAvidScriptStructWireRootTestType returnValue)")));
	TestTrue(
		TEXT("Struct-wire property setter uses native in storage"),
		FirstSource.Contains(TEXT("Invoke0003(int selfSlot, int selfGeneration, in FAvidScriptStructWireRootTestType value)")));

	FString SecondSource;
	FString SecondErrorCategory;
	FString SecondErrorSource;
	TestTrue(
		TEXT("Recursive fixed-width USTRUCT facade emits deterministically"),
		FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
			Package,
			TEXT("struct-wire-descriptor-hash"),
			SecondSource,
			SecondErrorCategory,
			SecondErrorSource));
	TestEqual(TEXT("Recursive fixed-width USTRUCT source is deterministic"), SecondSource, FirstSource);

	const FAvidScriptBindingTypeModel* const OriginalRootType = Package.Types.FindByPredicate(
		[](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.Kind == TEXT("struct_wire")
				&& Type.CppType == TEXT("FAvidScriptStructWireRootTestType");
		});
	if (!TestNotNull(TEXT("Struct-wire root type is available for identity checks"), OriginalRootType))
	{
		return false;
	}
	const FString OriginalLayoutStableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		OriginalRootType->CanonicalType,
		OriginalRootType->EnumValues,
		OriginalRootType->StructFields,
		OriginalRootType->Size,
		OriginalRootType->Alignment);
	TestTrue(
		TEXT("Struct-wire stable identity changes with wire size"),
		OriginalLayoutStableId != FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
			OriginalRootType->CanonicalType,
			OriginalRootType->EnumValues,
			OriginalRootType->StructFields,
			OriginalRootType->Size + 4,
			OriginalRootType->Alignment));
	TestTrue(
		TEXT("Struct-wire stable identity changes with wire alignment"),
		OriginalLayoutStableId != FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
			OriginalRootType->CanonicalType,
			OriginalRootType->EnumValues,
			OriginalRootType->StructFields,
			OriginalRootType->Size,
			OriginalRootType->Alignment * 2));

	const auto ExpectParserLayoutTamperRejected = [this, &DescriptorJson, &Package](
		const FString& Label,
		const FString& CanonicalType,
		TFunctionRef<void(FAvidScriptBindingTypeModel&)> MutateModel,
		TFunctionRef<void(FJsonObject&)> MutateJson)
	{
		FAvidScriptBindingPackageModel TamperedPackage = Package;
		FAvidScriptBindingTypeModel* Type = TamperedPackage.Types.FindByPredicate(
			[&CanonicalType](const FAvidScriptBindingTypeModel& Candidate)
			{
				return Candidate.CanonicalType == CanonicalType;
			});
		if (!TestNotNull(Label + TEXT(" type exists"), Type))
		{
			return;
		}
		MutateModel(*Type);
		TamperedPackage.SelectionHash =
			FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(TamperedPackage);
		TamperedPackage.PackageHash =
			FAvidScriptBindingDescriptorIdentity::MakePackageHash(TamperedPackage);

		FString TamperedJson;
		if (!TestTrue(
				Label + TEXT(" descriptor JSON is rewritten with valid outer hashes"),
				SerializeTamperedDescriptorType(
					DescriptorJson,
					TamperedPackage,
					CanonicalType,
					MutateJson,
					TamperedJson)))
		{
			return;
		}
		FAvidScriptBindingPackageModel ParsedTamper;
		FString ErrorCategory;
		FString ErrorSource;
		TestFalse(
			Label + TEXT(" parser rejects canonical-layout tamper"),
			FAvidScriptBindingDescriptorParser::Parse(
				TamperedJson,
				ParsedTamper,
				ErrorCategory,
				ErrorSource));
	};
	ExpectParserLayoutTamperRejected(
		TEXT("Struct-wire bool alignment tamper"),
		TEXT("scalar:bool"),
		[](FAvidScriptBindingTypeModel& Type) { Type.Alignment = 2; },
		[](FJsonObject& Type) { Type.SetNumberField(TEXT("alignment"), 2); });
	ExpectParserLayoutTamperRejected(
		TEXT("Struct-wire bool size tamper"),
		TEXT("scalar:bool"),
		[](FAvidScriptBindingTypeModel& Type) { Type.Size = 1; },
		[](FJsonObject& Type) { Type.SetNumberField(TEXT("size"), 1); });
	ExpectParserLayoutTamperRejected(
		TEXT("Struct-wire parent size tamper"),
		OriginalRootType->CanonicalType,
		[](FAvidScriptBindingTypeModel& Type) { Type.Size += 4; },
		[](FJsonObject& Type)
		{
			Type.SetNumberField(TEXT("size"), Type.GetNumberField(TEXT("size")) + 4);
		});
	ExpectParserLayoutTamperRejected(
		TEXT("Struct-wire parent alignment tamper"),
		OriginalRootType->CanonicalType,
		[](FAvidScriptBindingTypeModel& Type) { Type.Alignment *= 2; },
		[](FJsonObject& Type)
		{
			Type.SetNumberField(TEXT("alignment"), Type.GetNumberField(TEXT("alignment")) * 2);
		});

	const auto ExpectRendererLayoutTamperRejected = [this, &Package](
		const FString& Label,
		TFunctionRef<void(FAvidScriptBindingPackageModel&)> Mutate)
	{
		FAvidScriptBindingPackageModel TamperedPackage = Package;
		Mutate(TamperedPackage);
		FString TamperedSource(TEXT("stale-source"));
		FString ErrorCategory;
		FString ErrorSource;
		TestFalse(
			Label + TEXT(" renderer fails closed"),
			FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
				TamperedPackage,
				TEXT("struct-wire-layout-tamper"),
				TamperedSource,
				ErrorCategory,
				ErrorSource));
		TestEqual(
			Label + TEXT(" renderer emits no partial source"),
			TamperedSource,
			FString());
	};
	ExpectRendererLayoutTamperRejected(
		TEXT("Struct-wire bool alignment tamper"),
		[](FAvidScriptBindingPackageModel& TamperedPackage)
		{
			TamperedPackage.Types.FindByPredicate([](const FAvidScriptBindingTypeModel& Type)
			{
				return Type.CanonicalType == TEXT("scalar:bool");
			})->Alignment = 2;
		});
	ExpectRendererLayoutTamperRejected(
		TEXT("Struct-wire bool size tamper"),
		[](FAvidScriptBindingPackageModel& TamperedPackage)
		{
			TamperedPackage.Types.FindByPredicate([](const FAvidScriptBindingTypeModel& Type)
			{
				return Type.CanonicalType == TEXT("scalar:bool");
			})->Size = 1;
		});
	ExpectRendererLayoutTamperRejected(
		TEXT("Struct-wire field gap tamper"),
		[](FAvidScriptBindingPackageModel& TamperedPackage)
		{
			FAvidScriptBindingTypeModel* Type = TamperedPackage.Types.FindByPredicate([](const FAvidScriptBindingTypeModel& Candidate)
			{
				return Candidate.CppType == TEXT("FAvidScriptStructWireRootTestType");
			});
			Type->StructFields[2].WireOffset += 1;
		});
	ExpectRendererLayoutTamperRejected(
		TEXT("Struct-wire parent size tamper"),
		[](FAvidScriptBindingPackageModel& TamperedPackage)
		{
			TamperedPackage.Types.FindByPredicate([](const FAvidScriptBindingTypeModel& Type)
			{
				return Type.CppType == TEXT("FAvidScriptStructWireRootTestType");
			})->Size += 4;
		});
	ExpectRendererLayoutTamperRejected(
		TEXT("Struct-wire parent alignment tamper"),
		[](FAvidScriptBindingPackageModel& TamperedPackage)
		{
			TamperedPackage.Types.FindByPredicate([](const FAvidScriptBindingTypeModel& Type)
			{
				return Type.CppType == TEXT("FAvidScriptStructWireRootTestType");
			})->Alignment *= 2;
		});

	FAvidScriptBindingPackageModel Tampered = Package;
	FAvidScriptBindingTypeModel* RootType = Tampered.Types.FindByPredicate(
		[](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.Kind == TEXT("struct_wire")
				&& Type.CppType == TEXT("FAvidScriptStructWireRootTestType");
		});
	if (!TestNotNull(TEXT("Recursive fixed-width USTRUCT root type remains available for tamper"), RootType))
	{
		return false;
	}
	RootType->StructFields[0].TypeId = TEXT("missing-struct-wire-child-id");
	FString TamperedSource;
	FString TamperedErrorCategory;
	FString TamperedErrorSource;
	TestFalse(
		TEXT("Struct-wire child type-id tamper fails closed"),
		FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
			Tampered,
			TEXT("struct-wire-descriptor-hash"),
			TamperedSource,
			TamperedErrorCategory,
			TamperedErrorSource));
	TestEqual(TEXT("Struct-wire child type-id tamper reports descriptor contract failure"), TamperedErrorCategory, FString(TEXT("descriptor_contract_invalid")));
	TestTrue(TEXT("Struct-wire child type-id tamper emits no partial source"), TamperedSource.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterClassReferenceTest,
	"AvidScript.Editor.CSharpBindingEmitter.ClassReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterClassReferenceTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	TestTrue(
		TEXT("Class reference descriptor generates for facade emission"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
			TEXT("avidscript.project.class_facade"),
			{ { TEXT("/Script/Engine.Actor"), TEXT("GetActorScale3D") } },
			{},
			{
				{ TEXT("ProjectileClass"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") }
			},
			DescriptorJson,
			DescriptorResult));

	FString Source;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	TestTrue(
		TEXT("Class reference facade emits"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			Source,
			ManifestJson,
			EmitResult));
	TestEqual(TEXT("Emitter reports one class reference"), EmitResult.ClassReferenceCount, 1);
	TestTrue(TEXT("Facade declares the nominal Actor class wrapper"),
		Source.Contains(TEXT("public readonly struct TSubclassOfAActor")));
	TestTrue(TEXT("Class wrapper stores one private ordinal cell"),
		Source.Contains(TEXT("private readonly int Ordinal;")));
	TestTrue(TEXT("Class wrapper exposes only the internal ABI ordinal"),
		Source.Contains(TEXT("internal int AvidScriptOrdinal => Ordinal;")));
	TestTrue(TEXT("Facade publishes the project class by descriptor ordinal"),
		Source.Contains(TEXT("public static TSubclassOfAActor ProjectileClass => new(0);")));
	TestFalse(TEXT("Class facade does not expose an implicit integer conversion"),
		Source.Contains(TEXT("implicit operator int")));
	TestTrue(TEXT("Lifecycle facade unwraps class and transform values for SpawnActor"),
		Source.Contains(TEXT("public static AActor SpawnActor(TSubclassOfAActor actorClass, FTransform transform)"))
		&& Source.Contains(TEXT("actorClass.AvidScriptOrdinal, in transform"))
		&& Source.Contains(TEXT("out FAvidScriptObjectHandle actorHandle"))
		&& Source.Contains(TEXT("new(actorHandle.Slot, actorHandle.Generation)")));
	TestTrue(TEXT("Lifecycle facade unwraps Actor handles for DestroyActor"),
		Source.Contains(TEXT("public static bool DestroyActor(AActor actor)"))
		&& Source.Contains(TEXT("actor.AvidScriptSlot, actor.AvidScriptGeneration")));
	TestTrue(TEXT("Lifecycle facade unwraps Actor and class values for IsA"),
		Source.Contains(TEXT("public static bool IsA(AActor actor, TSubclassOfAActor actorClass)"))
		&& Source.Contains(TEXT("actorClass.AvidScriptOrdinal")));
	TestFalse(TEXT("Lifecycle facade exposes no unsafe pointer conversion"), Source.Contains(TEXT("IntPtr")));

	FAvidScriptBindingPackageModel LegacyClassReferencePackage;
	FString LegacyClassReferenceParseCategory;
	FString LegacyClassReferenceParseSource;
	if (TestTrue(
		TEXT("Class reference descriptor parses before schema v5 projection"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			LegacyClassReferencePackage,
			LegacyClassReferenceParseCategory,
			LegacyClassReferenceParseSource)))
	{
		LegacyClassReferencePackage.SchemaVersion = 5;
		LegacyClassReferencePackage.SelfTypeId.Empty();
		for (FAvidScriptBindingTypeModel& Type : LegacyClassReferencePackage.Types)
		{
			Type.ObjectTypeOrdinal = INDEX_NONE;
			Type.ClassPath.Empty();
			Type.BaseTypeId.Empty();
		}
		for (FAvidScriptBindingClassReferenceModel& Reference : LegacyClassReferencePackage.ClassReferences)
		{
			Reference.ResultTypeId.Empty();
		}
		LegacyClassReferencePackage.SelectionHash =
			FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(LegacyClassReferencePackage);
		LegacyClassReferencePackage.PackageHash =
			FAvidScriptBindingDescriptorIdentity::MakePackageHash(LegacyClassReferencePackage);

		FString LegacyClassReferenceDescriptor;
		TestTrue(
			TEXT("Schema v5 class reference descriptor serializes canonically"),
			FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
				LegacyClassReferencePackage,
				LegacyClassReferenceDescriptor));
		FString LegacyClassReferenceSource;
		FString LegacyClassReferenceManifest;
		FAvidScriptCSharpBindingEmitResult LegacyClassReferenceEmitResult;
		TestTrue(
			TEXT("Schema v5 class reference descriptor regenerates a legacy facade"),
			FAvidScriptEditorCSharpBindingEmitter::Emit(
				LegacyClassReferenceDescriptor,
				LegacyClassReferenceSource,
				LegacyClassReferenceManifest,
				LegacyClassReferenceEmitResult));
		TestTrue(
			TEXT("Schema v5 class reference uses base_class_path for its nominal wrapper"),
			LegacyClassReferenceSource.Contains(
				TEXT("public static TSubclassOfAActor ProjectileClass => new(0);")));
		TestTrue(
			TEXT("Schema v5 class reference keeps the Actor lifecycle facade"),
			LegacyClassReferenceSource.Contains(
				TEXT("public static AActor SpawnActor(TSubclassOfAActor actorClass, FTransform transform)")));
	}

	const auto& LifecycleSpecs = FAvidScriptObjectLifecycleBindings::GetSpecs();
	for (int32 SpecIndex = 0; SpecIndex < LifecycleSpecs.Num(); ++SpecIndex)
	{
		const FAvidScriptObjectLifecycleBindingSpec& Spec = LifecycleSpecs[SpecIndex];
		TestTrue(
			TEXT("Every shared lifecycle import is emitted once"),
			Source.Contains(FString::Printf(
				TEXT("[DllImport(\"%s\", EntryPoint = \"%s\")]"),
				*Spec.ModuleName,
				*Spec.ImportName)));
	}

	FString ClassOnlyDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult ClassOnlyDescriptorResult;
	TestTrue(
		TEXT("Class-only descriptor generates for lifecycle facade emission"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
			TEXT("avidscript.project.class_only_facade"),
			{},
			{},
			{
				{ TEXT("ProjectileClass"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") }
			},
			ClassOnlyDescriptorJson,
			ClassOnlyDescriptorResult));
	FString ClassOnlySource;
	FString ClassOnlyManifest;
	FAvidScriptCSharpBindingEmitResult ClassOnlyEmitResult;
	TestTrue(
		TEXT("Class-only lifecycle facade emits"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			ClassOnlyDescriptorJson,
			ClassOnlySource,
			ClassOnlyManifest,
			ClassOnlyEmitResult));
	TestTrue(TEXT("Class-only facade forces FVector support"),
		ClassOnlySource.Contains(TEXT("public readonly struct FVector")));
	TestTrue(TEXT("Class-only facade forces FRotator support"),
		ClassOnlySource.Contains(TEXT("public readonly struct FRotator")));
	TestTrue(TEXT("Class-only facade forces FTransform support"),
		ClassOnlySource.Contains(TEXT("public readonly struct FTransform")));
	TestTrue(TEXT("Class-only facade synthesizes the nominal Actor handle proxy"),
		ClassOnlySource.Contains(TEXT("public readonly struct AActor"))
		&& ClassOnlySource.Contains(TEXT("internal int AvidScriptSlot => Slot;"))
		&& ClassOnlySource.Contains(TEXT("internal int AvidScriptGeneration => Generation;")));

	TSharedPtr<FJsonObject> Manifest;
	TestTrue(TEXT("Class reference manifest parses"), ParseJsonObject(ManifestJson, Manifest));
	if (Manifest.IsValid())
	{
		TestEqual(TEXT("Manifest records one class reference"), Manifest->GetIntegerField(TEXT("class_reference_count")), 1);
		TestEqual(TEXT("Manifest identifies descriptor schema v6"), Manifest->GetIntegerField(TEXT("descriptor_schema_version")), 6);
		const TArray<TSharedPtr<FJsonValue>>& RequiredImports = Manifest->GetArrayField(TEXT("required_imports"));
		TestEqual(
			TEXT("Manifest appends lifecycle, object-type, and owner capabilities after reflected imports"),
			RequiredImports.Num(),
			DescriptorResult.BindingCount + LifecycleSpecs.Num() + 2);
		TestTrue(
			TEXT("Manifest includes the packed owner import"),
			RequiredImports.ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value)
			{
				const TSharedPtr<FJsonObject> Import = Value.IsValid() ? Value->AsObject() : nullptr;
				return Import.IsValid()
					&& Import->GetStringField(TEXT("module")) == TEXT("avidscript")
					&& Import->GetStringField(TEXT("name")) == TEXT("avid_owner_get_handle")
					&& Import->GetStringField(TEXT("signature")) == TEXT("()I");
			}));
		for (int32 SpecIndex = 0;
			SpecIndex < LifecycleSpecs.Num() && DescriptorResult.BindingCount + SpecIndex < RequiredImports.Num();
			++SpecIndex)
		{
			const FAvidScriptObjectLifecycleBindingSpec& Spec = LifecycleSpecs[SpecIndex];
			const int32 ImportOrdinal = DescriptorResult.BindingCount + SpecIndex;
			const TSharedPtr<FJsonObject> Import = RequiredImports[ImportOrdinal]->AsObject();
			TestTrue(TEXT("Lifecycle manifest import remains an object"), Import.IsValid());
			if (!Import.IsValid())
			{
				continue;
			}
			TestEqual(TEXT("Lifecycle manifest preserves shared stable id"), Import->GetStringField(TEXT("stable_id")), Spec.StableId);
			TestEqual(TEXT("Lifecycle manifest appends a stable ordinal"), Import->GetIntegerField(TEXT("ordinal")), ImportOrdinal);
			TestEqual(TEXT("Lifecycle manifest preserves shared module"), Import->GetStringField(TEXT("module")), Spec.ModuleName);
			TestEqual(TEXT("Lifecycle manifest preserves shared import name"), Import->GetStringField(TEXT("name")), Spec.ImportName);
			TestEqual(TEXT("Lifecycle manifest preserves shared ABI signature"), Import->GetStringField(TEXT("signature")), Spec.Signature);
		}
		const TConstArrayView<FAvidScriptObjectTypeBindingSpec> ObjectTypeSpecs =
			FAvidScriptObjectTypeBindings::GetSpecs();
		if (TestEqual(TEXT("Object-type capability has one shared specification"), ObjectTypeSpecs.Num(), 1))
		{
			const int32 ObjectTypeImportOrdinal =
				DescriptorResult.BindingCount + LifecycleSpecs.Num();
			const TSharedPtr<FJsonObject> Import =
				RequiredImports[ObjectTypeImportOrdinal]->AsObject();
			TestTrue(TEXT("Object-type manifest import remains an object"), Import.IsValid());
			if (Import.IsValid())
			{
				TestEqual(TEXT("Object-type manifest preserves shared stable id"), Import->GetStringField(TEXT("stable_id")), ObjectTypeSpecs[0].StableId);
				TestEqual(TEXT("Object-type manifest appends a stable ordinal"), Import->GetIntegerField(TEXT("ordinal")), ObjectTypeImportOrdinal);
				TestEqual(TEXT("Object-type manifest preserves shared import name"), Import->GetStringField(TEXT("name")), ObjectTypeSpecs[0].ImportName);
				TestEqual(TEXT("Object-type manifest preserves shared ABI signature"), Import->GetStringField(TEXT("signature")), ObjectTypeSpecs[0].Signature);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterObjectFactoryManifestTest,
	"AvidScript.Editor.CSharpBindingEmitter.ObjectFactoryManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterObjectFactoryManifestTest::RunTest(
	const FString& Parameters)
{
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
		TEXT("Factory-only descriptor v7 generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithObjectFactories(
			TEXT("avidscript.project.factory_manifest"),
			{},
			{},
			{
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
			},
			{
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
			},
			DescriptorJson,
			DescriptorResult)))
	{
		AddError(DescriptorResult.ErrorMessage);
		return false;
	}

	FString Source;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	if (!TestTrue(
		TEXT("Factory-only descriptor v7 emits canonically"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			Source,
			ManifestJson,
			EmitResult)))
	{
		AddError(EmitResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("Emitter reports the factory table"),
		EmitResult.ObjectFactoryCount,
		2);
	TestFalse(
		TEXT("Factory class references do not publish Actor lifecycle facade"),
		Source.Contains(TEXT("public static class ProjectClasses"))
			|| Source.Contains(TEXT("SpawnActor("))
			|| Source.Contains(TEXT("avid_object_spawn_actor")));
	TestTrue(
		TEXT("Factory object graph still publishes object type checks"),
		Source.Contains(TEXT("avid_object_type_is_a")));
	TestTrue(
		TEXT("Factory descriptor publishes nominal project capability tokens"),
		Source.Contains(TEXT("public static class ProjectFactories"))
			&& Source.Contains(TEXT("public static class ProjectTypes"))
			&& Source.Contains(TEXT("public readonly struct TObjectFactoryOfInventoryState"))
			&& Source.Contains(TEXT("public readonly struct TObjectTypeOfSceneComponent")));
	TestTrue(
		TEXT("Factory tokens expose stable generated literals only"),
		Source.Contains(TEXT("internal TObjectFactoryOfInventoryState(int ordinal)"))
			&& Source.Contains(TEXT("internal TObjectTypeOfSceneComponent(int ordinal)"))
			&& !Source.Contains(TEXT("public TObjectFactoryOfInventoryState(int ordinal)"))
			&& !Source.Contains(TEXT("implicit operator int")));
	TestTrue(
		TEXT("Factory facade exposes typed construction, query, attachment, and release"),
		Source.Contains(TEXT(" NewObject(UObject outer, TObjectFactoryOfInventoryState factory)"))
			&& Source.Contains(TEXT(" CreateComponent(AActor outer, TObjectFactoryOfSceneComponent factory)"))
			&& Source.Contains(TEXT(" FindComponent(AActor actor, TObjectTypeOfSceneComponent type)"))
			&& Source.Contains(TEXT("public enum AttachmentRule : int"))
			&& Source.Contains(TEXT("public enum DetachmentRule : int"))
			&& Source.Contains(TEXT("public static bool AttachTo("))
			&& Source.Contains(TEXT("public static bool Detach("))
			&& Source.Contains(TEXT(" Release(UAvidScriptCSharpBindingEmitterTestObject value)"))
			&& Source.Contains(TEXT(" Release(USceneComponent value)")));
	TestTrue(
		TEXT("Factory facade uses packed i64 handles without guest scratch memory"),
		Source.Contains(TEXT("long packedHandle = AvidScriptNative.ObjectConstruct("))
			&& Source.Contains(TEXT("long packedHandle = AvidScriptNative.ActorFindComponent("))
			&& Source.Contains(TEXT("return new((int)packedHandle, (int)(packedHandle >> 32));"))
			&& Source.Contains(TEXT("internal static extern long ObjectConstruct("))
			&& Source.Contains(TEXT("internal static extern int ObjectRelease("))
			&& Source.Contains(TEXT("internal static extern long ActorFindComponent("))
			&& Source.Contains(TEXT("internal static extern int SceneComponentAttach("))
			&& Source.Contains(TEXT("internal static extern int SceneComponentDetach(")));

	TSharedPtr<FJsonObject> Manifest;
	if (!TestTrue(
		TEXT("Factory manifest parses"),
		ParseJsonObject(ManifestJson, Manifest))
		|| !TestNotNull(TEXT("Factory manifest object exists"), Manifest.Get()))
	{
		return false;
	}
	TestEqual(
		TEXT("Factory manifest identifies descriptor schema v7"),
		Manifest->GetIntegerField(TEXT("descriptor_schema_version")),
		7);
	TestEqual(
		TEXT("Factory manifest preserves class-reference provenance"),
		Manifest->GetIntegerField(TEXT("class_reference_count")),
		2);
	TestEqual(
		TEXT("Factory manifest preserves object-factory provenance"),
		Manifest->GetIntegerField(TEXT("object_factory_count")),
		2);
	const TArray<TSharedPtr<FJsonValue>>& RequiredImports =
		Manifest->GetArrayField(TEXT("required_imports"));
	const bool bFactoryImportCountValid = TestEqual(
		TEXT("Factory-only package requires type, factory, attachment, and owner capabilities"),
		RequiredImports.Num(),
		7);
	const TConstArrayView<FAvidScriptObjectFactoryBindingSpec> FactorySpecs =
		FAvidScriptObjectFactoryBinding::GetSpecs();
	if (bFactoryImportCountValid
		&& TestEqual(TEXT("Factory capability has three shared specifications"), FactorySpecs.Num(), 3))
	{
		for (int32 SpecIndex = 0; SpecIndex < FactorySpecs.Num(); ++SpecIndex)
		{
			const TSharedPtr<FJsonObject> Import =
				RequiredImports[1 + SpecIndex]->AsObject();
			TestTrue(TEXT("Factory manifest import remains an object"), Import.IsValid());
			if (Import.IsValid())
			{
				TestEqual(TEXT("Factory manifest preserves shared stable id"), Import->GetStringField(TEXT("stable_id")), FactorySpecs[SpecIndex].StableId);
				TestEqual(TEXT("Factory manifest appends a stable ordinal"), Import->GetIntegerField(TEXT("ordinal")), 1 + SpecIndex);
				TestEqual(TEXT("Factory manifest preserves shared import name"), Import->GetStringField(TEXT("name")), FactorySpecs[SpecIndex].ImportName);
				TestEqual(TEXT("Factory manifest preserves shared ABI signature"), Import->GetStringField(TEXT("signature")), FactorySpecs[SpecIndex].Signature);
			}
		}
	}
	const TConstArrayView<FAvidScriptSceneAttachmentBindingSpec> AttachmentSpecs =
		FAvidScriptSceneAttachmentBinding::GetSpecs();
	if (bFactoryImportCountValid
		&& TestEqual(TEXT("Scene attachment has two shared specifications"),
			AttachmentSpecs.Num(), 2))
	{
		for (int32 SpecIndex = 0; SpecIndex < AttachmentSpecs.Num(); ++SpecIndex)
		{
			const TSharedPtr<FJsonObject> Import =
				RequiredImports[4 + SpecIndex]->AsObject();
			TestTrue(TEXT("Attachment manifest import remains an object"), Import.IsValid());
			if (Import.IsValid())
			{
				TestEqual(TEXT("Attachment manifest preserves shared stable id"),
					Import->GetStringField(TEXT("stable_id")),
					AttachmentSpecs[SpecIndex].StableId);
				TestEqual(TEXT("Attachment manifest appends a stable ordinal"),
					Import->GetIntegerField(TEXT("ordinal")),
					4 + SpecIndex);
				TestEqual(TEXT("Attachment manifest preserves shared import name"),
					Import->GetStringField(TEXT("name")),
					AttachmentSpecs[SpecIndex].ImportName);
				TestEqual(TEXT("Attachment manifest preserves shared ABI signature"),
					Import->GetStringField(TEXT("signature")),
					AttachmentSpecs[SpecIndex].Signature);
			}
		}
	}

	FString MixedDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult MixedDescriptorResult;
	if (!TestTrue(
		TEXT("Mixed Actor and factory descriptor v7 generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithObjectFactories(
			TEXT("avidscript.project.mixed_factory_manifest"),
			{},
			{},
			{
				{
					TEXT("InventoryStateClass"),
					TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"),
					TEXT("/Script/CoreUObject.Object"),
					TEXT("EditorLoad")
				},
				{
					TEXT("ProjectileClass"),
					TEXT("/Script/Engine.StaticMeshActor"),
					TEXT("/Script/Engine.Actor"),
					TEXT("EditorLoad")
				},
				{
					TEXT("SceneComponentClass"),
					TEXT("/Script/Engine.SceneComponent"),
					TEXT("/Script/Engine.ActorComponent"),
					TEXT("EditorLoad")
				}
			},
			{
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
			},
			MixedDescriptorJson,
			MixedDescriptorResult)))
	{
		AddError(MixedDescriptorResult.ErrorMessage);
		return false;
	}
	FAvidScriptBindingPackageModel MixedModel;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
		TEXT("Mixed descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			MixedDescriptorJson,
			MixedModel,
			ParseCategory,
			ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	const FAvidScriptBindingClassReferenceModel* ProjectileReference =
		MixedModel.ClassReferences.FindByPredicate(
			[](const FAvidScriptBindingClassReferenceModel& Reference)
			{
				return Reference.ScriptName == TEXT("ProjectileClass");
			});
	if (!TestNotNull(
		TEXT("Mixed descriptor retains Actor lifecycle class reference"),
		ProjectileReference))
	{
		return false;
	}

	FString MixedSource;
	FString MixedManifestJson;
	FAvidScriptCSharpBindingEmitResult MixedEmitResult;
	if (!TestTrue(
		TEXT("Mixed descriptor emits"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			MixedDescriptorJson,
			MixedSource,
			MixedManifestJson,
			MixedEmitResult)))
	{
		AddError(MixedEmitResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Mixed facade preserves the Actor class-reference ordinal"),
		MixedSource.Contains(FString::Printf(
			TEXT("public static TSubclassOfAActor ProjectileClass => new(%d);"),
			ProjectileReference->Ordinal)));
	TestFalse(
		TEXT("Mixed facade does not expose factory classes as spawn capabilities"),
		MixedSource.Contains(TEXT(" InventoryStateClass =>"))
			|| MixedSource.Contains(TEXT(" SceneComponentClass =>")));

	TSharedPtr<const FAvidScriptBindingPackage> MixedRuntimePackage;
	FAvidScriptBindingPackageLoadResult MixedLoadResult;
	if (!TestTrue(
		TEXT("Mixed descriptor loads immutable runtime plans"),
		FAvidScriptBindingPackage::LoadDescriptor(
			MixedDescriptorJson,
			MixedRuntimePackage,
			MixedLoadResult))
		|| !TestNotNull(
			TEXT("Mixed runtime package exists"),
			MixedRuntimePackage.Get()))
	{
		AddError(MixedLoadResult.ErrorCategory + TEXT(": ")
			+ MixedLoadResult.ErrorDetails);
		return false;
	}
	UClass* ResolvedClass = nullptr;
	UClass* ResolvedBaseClass = nullptr;
	TestTrue(
		TEXT("Actor lifecycle ordinal resolves through its original mixed table slot"),
		MixedRuntimePackage->TryResolveClassReference(
			ProjectileReference->Ordinal,
			ResolvedClass,
			ResolvedBaseClass));
	TestEqual(
		TEXT("Mixed Actor lifecycle class is cached"),
		ResolvedClass,
		AStaticMeshActor::StaticClass());
	TestEqual(
		TEXT("Mixed Actor lifecycle base is cached"),
		ResolvedBaseClass,
		AActor::StaticClass());
	for (const FAvidScriptBindingClassReferenceModel& Reference :
		MixedModel.ClassReferences)
	{
		if (Reference.ScriptName == TEXT("ProjectileClass"))
		{
			continue;
		}
		TestFalse(
			TEXT("Factory-owned mixed table slot is not a lifecycle capability"),
			MixedRuntimePackage->TryResolveClassReference(
				Reference.Ordinal,
				ResolvedClass,
				ResolvedBaseClass));
	}
	TestEqual(
		TEXT("Mixed runtime publishes lifecycle, object-type, and factory imports"),
		MixedRuntimePackage->GetVmPackage().Imports.Num(),
		FAvidScriptObjectLifecycleBindings::GetSpecs().Num()
			+ FAvidScriptObjectTypeBindings::GetSpecs().Num()
			+ FAvidScriptObjectFactoryBinding::GetSpecs().Num()
			+ FAvidScriptSceneAttachmentBinding::GetSpecs().Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterTypedProjectApiTest,
	"AvidScript.Editor.CSharpBindingEmitter.TypedProjectApi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterTypedProjectApiTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.project.typed_facade");
	Profile.SelfClassPath = TEXT("/Script/Engine.StaticMeshActor");
	const TArray<FAvidScriptProjectBindingClassSpec> ClassReferences = {
		{ TEXT("ProjectileClass"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("EditorLoad") }
	};
	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
		TEXT("Typed project API descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			ClassReferences,
			DescriptorJson,
			SelectionResult,
			DescriptorResult)))
	{
		AddError(DescriptorResult.ErrorMessage);
		return false;
	}

	FString Source;
	FString ManifestJson;
	FAvidScriptCSharpBindingEmitResult EmitResult;
	if (!TestTrue(
		TEXT("Typed project API facade emits"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(DescriptorJson, Source, ManifestJson, EmitResult)))
	{
		AddError(EmitResult.ErrorMessage);
		return false;
	}

	TestTrue(TEXT("Typed facade declares a nominal StaticMeshActor handle"),
		Source.Contains(TEXT("public readonly struct AStaticMeshActor")));
	TestTrue(TEXT("Typed facade keeps two-cell object ABI"),
		Source.Contains(TEXT("internal readonly int Slot;"))
		&& Source.Contains(TEXT("internal readonly int Generation;"))
		&& Source.Contains(TEXT("public bool HasHandle => Slot > 0 && Generation > 0;"))
		&& Source.Contains(TEXT("public bool IsNull => Slot == 0 && Generation == 0;")));
	TestTrue(TEXT("Typed facade emits direct-base upcast without a host import"),
		Source.Contains(TEXT("public static implicit operator AActor(AStaticMeshActor value)"))
		&& Source.Contains(TEXT("return new(value.Slot, value.Generation);")));
	TestTrue(TEXT("Typed facade emits checked cast on the target-derived wrapper"),
		Source.Contains(TEXT("public static AStaticMeshActor TryCast(AActor value)"))
		&& Source.Contains(TEXT("AvidScriptNative.ObjectTypeIsA(value.Slot, value.Generation, 2) != 0"))
		&& Source.Contains(TEXT("return new(value.Slot, value.Generation);"))
		&& Source.Contains(TEXT("return default;")));
	TestFalse(TEXT("Typed facade removes the old reversed instance cast"),
		Source.Contains(TEXT("public AActor TryCast()")));
	TestTrue(TEXT("Typed facade emits one packed owner import and typed Self"),
		Source.Contains(TEXT("public static AStaticMeshActor Self"))
		&& Source.Contains(TEXT("[DllImport(\"avidscript\", EntryPoint = \"avid_owner_get_handle\")]"))
		&& Source.Contains(TEXT("private static extern long OwnerGetHandle();"))
		&& Source.Contains(TEXT("return new((int)packedHandle, (int)(packedHandle >> 32));"))
		&& !Source.Contains(TEXT("owner_get_slot"))
		&& !Source.Contains(TEXT("owner_get_generation")));
	TestTrue(TEXT("Typed facade emits nominal class references and typed Spawn"),
		Source.Contains(TEXT("public readonly struct TSubclassOfAStaticMeshActor"))
		&& Source.Contains(TEXT("public static TSubclassOfAStaticMeshActor ProjectileClass => new(0);"))
		&& Source.Contains(TEXT("public static AStaticMeshActor SpawnActor(TSubclassOfAStaticMeshActor actorClass, FTransform transform)"))
		&& Source.Contains(TEXT("public static AActor SpawnActor(TSubclassOfAActor actorClass, FTransform transform)")));
	TestTrue(TEXT("Typed class references upcast through the reflected inheritance graph"),
		Source.Contains(TEXT("public static implicit operator TSubclassOfAActor(TSubclassOfAStaticMeshActor value)"))
		&& Source.Contains(TEXT("return new(value.Ordinal);")));
	TestTrue(TEXT("Typed facade declares the object-type import exactly once"),
		Source.Contains(TEXT("[DllImport(\"avidscript\", EntryPoint = \"avid_object_type_is_a\")]"))
		&& Source.Contains(TEXT("internal static extern int ObjectTypeIsA(int slot, int generation, int targetOrdinal);")));

	FAvidScriptBindingPackageModel MissingClassReferenceResultPackage;
	FString MissingClassReferenceResultParseCategory;
	FString MissingClassReferenceResultParseSource;
	if (TestTrue(
		TEXT("Typed project API descriptor parses for fail-closed renderer coverage"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			MissingClassReferenceResultPackage,
			MissingClassReferenceResultParseCategory,
			MissingClassReferenceResultParseSource)))
	{
		FAvidScriptBindingClassReferenceModel* MissingResultReference =
			MissingClassReferenceResultPackage.ClassReferences.FindByPredicate(
				[](const FAvidScriptBindingClassReferenceModel& Reference)
				{
					return Reference.ScriptName == TEXT("ProjectileClass");
				});
		if (TestNotNull(
			TEXT("Fail-closed renderer coverage finds the derived class reference"),
			MissingResultReference))
		{
			MissingResultReference->ResultTypeId = FString::ChrN(64, TEXT('f'));
			FString InvalidSource;
			FString InvalidCategory;
			FString InvalidErrorSource;
			TestFalse(
				TEXT("Renderer rejects a class reference without its result type"),
				FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
					MissingClassReferenceResultPackage,
					FAvidScriptHash::Sha256HexUtf8(DescriptorJson),
					InvalidSource,
					InvalidCategory,
					InvalidErrorSource));
			TestEqual(
				TEXT("Missing class-reference result uses the descriptor contract category"),
				InvalidCategory,
				FString(TEXT("descriptor_contract_invalid")));
			TestEqual(
				TEXT("Missing result identifies the class-reference result contract"),
				InvalidErrorSource,
				FString(TEXT("class_references.result_type_id")));
		}
	}

	FAvidScriptBindingSelectionProfile InheritedMemberProfile = Profile;
	InheritedMemberProfile.ExplicitFunctions.Add({ TEXT("/Script/Engine.Actor"), TEXT("GetActorScale3D") });
	FString InheritedDescriptorJson;
	FAvidScriptBindingSelectionResolveResult InheritedSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult InheritedDescriptorResult;
	FString InheritedSource;
	FString InheritedManifest;
	FAvidScriptCSharpBindingEmitResult InheritedEmitResult;
	if (TestTrue(
		TEXT("Inherited-member descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			InheritedMemberProfile,
			ClassReferences,
			InheritedDescriptorJson,
			InheritedSelectionResult,
			InheritedDescriptorResult))
		&& TestTrue(
			TEXT("Inherited-member facade emits"),
			FAvidScriptEditorCSharpBindingEmitter::Emit(
				InheritedDescriptorJson,
				InheritedSource,
				InheritedManifest,
				InheritedEmitResult)))
	{
		const int32 DerivedStart = InheritedSource.Find(TEXT("public readonly struct AStaticMeshActor"));
		const int32 DerivedEnd = DerivedStart == INDEX_NONE ? INDEX_NONE : InheritedSource.Find(TEXT("\n}"), DerivedStart);
		const FString DerivedBlock = DerivedEnd == INDEX_NONE ? FString() : InheritedSource.Mid(DerivedStart, DerivedEnd - DerivedStart);
		TestTrue(TEXT("Base wrapper keeps its reflected member"), InheritedSource.Contains(TEXT("public FVector GetActorScale3D()")));
		TestFalse(TEXT("Derived wrapper does not copy base reflected members"), DerivedBlock.Contains(TEXT("GetActorScale3D")));
	}

	const FString FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/BindingGeneration/P50_TypedProjectApi.generated.cs")));
	FString FixtureSource;
	if (!TestTrue(TEXT("Typed project API golden fixture loads"), FFileHelper::LoadFileToString(FixtureSource, *FixturePath)))
	{
		return false;
	}
	const FString TypedSurface = ExtractTypedProjectFacadeSurface(Source);
	if (TypedSurface != FixtureSource)
	{
		const FString ActualPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScriptGeneratedBindingsTests/P50_TypedProjectApi.actual.cs")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ActualPath), true);
		TestTrue(
			TEXT("Changed typed project API facade is saved for deterministic fixture review"),
			FFileHelper::SaveStringToFile(
				TypedSurface,
				*ActualPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		AddInfo(FString::Printf(TEXT("Changed typed project API facade: %s"), *ActualPath));
	}
	TestEqual(TEXT("Typed project API facade matches the C# golden contract"), TypedSurface, FixtureSource);

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
		TestEqual(TEXT("Property manifest declares reflected, object-type, and packed owner imports"), ManifestImports.Num(), 3);
		if (ManifestImports.Num() == 3)
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
			TestTrue(
				TEXT("Property manifest includes object-type support"),
				ManifestImports.ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Value)
					{
						const TSharedPtr<FJsonObject> Import = Value->AsObject();
						return Import.IsValid()
							&& Import->GetStringField(TEXT("name")) == TEXT("avid_object_type_is_a")
							&& Import->GetStringField(TEXT("signature")) == TEXT("(iii)i");
					}));
			TestTrue(
				TEXT("Property manifest includes the packed owner import"),
				ManifestImports.ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Value)
					{
						const TSharedPtr<FJsonObject> Import = Value->AsObject();
						return Import.IsValid()
							&& Import->GetStringField(TEXT("stable_id")) == TEXT("avidscript.owner_get_handle.v1")
							&& Import->GetIntegerField(TEXT("ordinal")) == -1
							&& Import->GetStringField(TEXT("module")) == TEXT("avidscript")
							&& Import->GetStringField(TEXT("name")) == TEXT("avid_owner_get_handle")
							&& Import->GetStringField(TEXT("signature")) == TEXT("()I");
					}));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterPropertySetTest,
	"AvidScript.Editor.CSharpBindingEmitter.PropertySet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterPropertySetTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
			TEXT("Writable property descriptor generates for C# emission"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
				TEXT("avidscript.engine.property_set_facade"),
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
			TEXT("Schema v8 property descriptor emits through the canonical C# pipeline"),
			FAvidScriptEditorCSharpBindingEmitter::Emit(
				DescriptorJson,
				Source,
				Manifest,
				EmitResult)))
	{
		AddError(EmitResult.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("Writable property facade exposes two bindings"), EmitResult.BindingCount, 2);
	TestTrue(
		TEXT("Generated property exposes natural get and set accessors"),
		Source.Contains(
			TEXT("public float CustomTimeDilation\n    {\n        get"))
		&& Source.Contains(TEXT("        set\n        {")));
	TestTrue(
		TEXT("Generated setter calls a cached native import"),
		Source.Contains(TEXT("int selfSlot, int selfGeneration, float value")));
	TestTrue(
		TEXT("Writable package manifest carries schema v8"),
		Manifest.Contains(TEXT("\"descriptor_schema_version\": 8")));
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
	TestEqual(TEXT("Gameplay profile preserves every accepted binding"), EmitResult.BindingCount, 351);
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
			TEXT("Gameplay manifest declares 351 reflected imports and two shared capabilities"),
			ManifestObject->GetArrayField(TEXT("required_imports")).Num(),
			353);
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
	if (Source != FixtureSource)
	{
		const FString ActualPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScriptGeneratedBindingsTests/P48_4_FNameActorHasTag.actual.cs")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ActualPath), true);
		TestTrue(
			TEXT("Changed FName facade is saved for deterministic fixture review"),
			FFileHelper::SaveStringToFile(
				Source,
				*ActualPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		AddInfo(FString::Printf(TEXT("Changed FName facade: %s"), *ActualPath));
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
		TEXT("direction_return") })
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
		if (Mutation == TEXT("direction_return"))
		{
			TestEqual(TEXT("Parameter return direction reports a descriptor contract failure"),
				ErrorCategory,
				FString(TEXT("descriptor_contract_invalid")));
		}
		else
		{
			TestTrue(TEXT("Tampered FName metadata has a stable rejection category: ") + Mutation,
				ErrorCategory == TEXT("unsupported_csharp_type") || ErrorCategory == TEXT("abi_signature_mismatch"));
		}
		TestTrue(TEXT("Tampered FName metadata emits no partial source: ") + Mutation, TamperedSource.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterNameStringTest,
	"AvidScript.Editor.CSharpBindingEmitter.NameString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterNameStringTest::RunTest(const FString& Parameters)
{
	const FString OwnerPath = UAvidScriptCSharpBindingEmitterTestObject::StaticClass()->GetPathName();
	const TArray<FAvidScriptReflectedFunctionSelection> Functions = {
		{ OwnerPath, TEXT("ReturnFName") },
		{ OwnerPath, TEXT("OutFName") },
		{ OwnerPath, TEXT("RefFName") },
		{ OwnerPath, TEXT("ConstRefFName") },
		{ OwnerPath, TEXT("ReturnFString") },
		{ OwnerPath, TEXT("OutFString") },
		{ OwnerPath, TEXT("RefFString") },
		{ OwnerPath, TEXT("ConstRefFString") },
		{ OwnerPath, TEXT("FStringValueDefault") }
	};
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ OwnerPath, TEXT("ReadableFName"), true },
		{ OwnerPath, TEXT("ReadableFString"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
			TEXT("FName and FString descriptor generates for C# emission"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
				TEXT("avidscript.engine.name_string.roundtrip"),
				Functions,
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
			TEXT("FName and FString descriptor emits C#"),
			FAvidScriptEditorCSharpBindingEmitter::Emit(DescriptorJson, Source, Manifest, EmitResult)))
	{
		AddError(EmitResult.ErrorCategory + TEXT(": ") + EmitResult.ErrorMessage);
		return false;
	}
	FString RepeatedSource;
	FString RepeatedManifest;
	FAvidScriptCSharpBindingEmitResult RepeatedEmitResult;
	TestTrue(TEXT("Repeated FName and FString emission succeeds"),
		FAvidScriptEditorCSharpBindingEmitter::Emit(
			DescriptorJson,
			RepeatedSource,
			RepeatedManifest,
			RepeatedEmitResult));
	TestEqual(TEXT("FName and FString generated source is deterministic"), RepeatedSource, Source);
	TestEqual(TEXT("FName and FString generated manifest is deterministic"), RepeatedManifest, Manifest);
	TestTrue(TEXT("FName return is rendered as string"), Source.Contains(TEXT("public string ReturnFName()")));
	TestTrue(TEXT("FName out is rendered as string"), Source.Contains(TEXT("OutFName(out string OutName)")));
	TestTrue(TEXT("FName ref is rendered as string"), Source.Contains(TEXT("RefFName(ref string InOutName)")));
	TestTrue(TEXT("FName const-ref is rendered as string"), Source.Contains(TEXT("ConstRefFName(string InName)")));
	TestTrue(TEXT("FString return is rendered as string"), Source.Contains(TEXT("public string ReturnFString()")));
	TestTrue(TEXT("FString out is rendered as string"), Source.Contains(TEXT("OutFString(out string OutString)")));
	TestTrue(TEXT("FString ref is rendered as string"), Source.Contains(TEXT("RefFString(ref string InOutString)")));
	TestTrue(TEXT("FString const-ref is rendered as string"), Source.Contains(TEXT("ConstRefFString(string InString)")));
	TestTrue(TEXT("FString value default is emitted as a C# string literal"), Source.Contains(TEXT("FStringValueDefault(string Value = \"Avid\")")));
	TestTrue(TEXT("FName property renders string get and set"), Source.Contains(TEXT("public string ReadableFName\n    {\n        get")) && Source.Contains(TEXT("set\n        {")));
	TestTrue(TEXT("FString property renders string get and set"), Source.Contains(TEXT("public string ReadableFString\n    {\n        get")) && Source.Contains(TEXT("set\n        {")));
	TestTrue(TEXT("FName out uses string address storage"), Source.Contains(TEXT("out string p0_OutName")));
	TestTrue(TEXT("FString out uses string address storage"), Source.Contains(TEXT("out string p0_OutString")));

	FAvidScriptBindingValueModel DefaultValue;
	DefaultValue.bHasDefault = true;
	DefaultValue.Direction = TEXT("value");
	DefaultValue.CanonicalType = TEXT("string:fstring");
	DefaultValue.Kind = TEXT("string_utf8");
	DefaultValue.CppType = TEXT("FString");
	DefaultValue.DefaultValue = TEXT("Escaped \\\"value\\\" \\\\ slash\n");
	FString FormattedDefault;
	const TMap<FString, const FAvidScriptBindingTypeModel*> NoTypes;
	TestTrue(TEXT("FString default formatter escapes C# syntax losslessly"),
		FAvidScriptEditorCSharpDefaultValueFormatter::TryFormat(DefaultValue, NoTypes, FormattedDefault));
	TestEqual(TEXT("FString default formatter preserves escaped content"),
		FormattedDefault,
		FString(TEXT("\"Escaped \\\\\\\"value\\\\\\\" \\\\\\\\ slash\\n\"")));
	DefaultValue.Direction = TEXT("const_ref");
	DefaultValue.CanonicalType = TEXT("name:fname");
	DefaultValue.Kind = TEXT("name_utf8");
	DefaultValue.CppType = TEXT("FName");
	TestTrue(TEXT("FName const-ref default formatter emits a C# string literal"),
		FAvidScriptEditorCSharpDefaultValueFormatter::TryFormat(DefaultValue, NoTypes, FormattedDefault));
	TestEqual(TEXT("FName default formatter preserves escaped content"),
		FormattedDefault,
		FString(TEXT("\"Escaped \\\\\\\"value\\\\\\\" \\\\\\\\ slash\\n\"")));
	DefaultValue.Direction = TEXT("ref");
	DefaultValue.CanonicalType = TEXT("string:fstring");
	DefaultValue.Kind = TEXT("string_utf8");
	DefaultValue.CppType = TEXT("FString");
	TestFalse(TEXT("FString ref does not receive a C# default"),
		FAvidScriptEditorCSharpDefaultValueFormatter::TryFormat(DefaultValue, NoTypes, FormattedDefault));
	DefaultValue.Direction = TEXT("out");
	TestFalse(TEXT("FString out does not receive a C# default"),
		FAvidScriptEditorCSharpDefaultValueFormatter::TryFormat(DefaultValue, NoTypes, FormattedDefault));

	const FString FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/BindingGeneration/P57_11B2_NameStringRoundtrip.generated.cs")));
	FString FixtureSource;
	if (!TestTrue(TEXT("Name/string generated facade fixture loads"), FFileHelper::LoadFileToString(FixtureSource, *FixturePath)))
	{
		return false;
	}
	FixtureSource.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	TArray<FString> FixtureLines;
	FixtureSource.ParseIntoArrayLines(FixtureLines);
	TArray<FString> ExpectedProjectionLines;
	for (FString FixtureLine : FixtureLines)
	{
		FixtureLine.TrimStartAndEndInline();
		if (FixtureLine.StartsWith(TEXT("// ")))
		{
			ExpectedProjectionLines.Add(FixtureLine.RightChop(3));
		}
	}
	ExpectedProjectionLines.Sort();
	TestEqual(TEXT("Name/string generated facade matches the deterministic projection fixture"),
		ExtractNameStringProjectionSurface(Source),
		FString::Join(ExpectedProjectionLines, TEXT("\n")));

	FAvidScriptBindingPackageModel Package;
	FString ParseErrorCategory;
	FString ParseErrorSource;
	if (!TestTrue(TEXT("Name/string descriptor parses into a renderer package"),
		FAvidScriptBindingDescriptorParser::Parse(DescriptorJson, Package, ParseErrorCategory, ParseErrorSource)))
	{
		return false;
	}
	for (const FString& CanonicalType : { TEXT("name:fname"), TEXT("string:fstring") })
	{
		const FString TypeName = CanonicalType == TEXT("name:fname") ? TEXT("FName") : TEXT("FString");
		for (const FString& Direction : { TEXT("ref"), TEXT("out") })
		{
			const FString ScriptName = (Direction == TEXT("ref") ? TEXT("Ref") : TEXT("Out")) + TypeName;
			const FAvidScriptBindingFunctionModel* Binding = Package.Bindings.FindByPredicate(
				[&ScriptName](const FAvidScriptBindingFunctionModel& Candidate)
				{
					return Candidate.ScriptName == ScriptName;
				});
			TestNotNull(TEXT("UTF-8 parameter binding is present: ") + ScriptName, Binding);
			if (Binding != nullptr)
			{
				const FAvidScriptBindingValueModel* Value = Binding->Parameters.FindByPredicate(
					[&CanonicalType](const FAvidScriptBindingValueModel& Candidate)
					{
						return Candidate.CanonicalType == CanonicalType;
					});
				TestNotNull(TEXT("UTF-8 parameter descriptor is present: ") + ScriptName, Value);
				if (Value != nullptr)
				{
					TestEqual(TEXT("UTF-8 parameter direction remains legal: ") + ScriptName, Value->Direction, Direction);
				}
			}
		}

		const FString ReturnScriptName = TEXT("Return") + TypeName;
		const FAvidScriptBindingFunctionModel* ReturnBinding = Package.Bindings.FindByPredicate(
			[&ReturnScriptName](const FAvidScriptBindingFunctionModel& Candidate)
			{
				return Candidate.ScriptName == ReturnScriptName;
			});
		TestNotNull(TEXT("UTF-8 return binding is present: ") + ReturnScriptName, ReturnBinding);
		if (ReturnBinding != nullptr)
		{
			TestEqual(TEXT("UTF-8 return descriptor uses return direction: ") + ReturnScriptName,
				ReturnBinding->ReturnValue.Direction,
				FString(TEXT("return")));
		}

		FAvidScriptBindingPackageModel ParameterTampered = Package;
		FAvidScriptBindingFunctionModel* ParameterBinding = ParameterTampered.Bindings.FindByPredicate(
			[&TypeName](const FAvidScriptBindingFunctionModel& Candidate)
			{
				return Candidate.ScriptName == TEXT("Ref") + TypeName;
			});
		TestNotNull(TEXT("UTF-8 parameter binding remains available for direction tamper: ") + TypeName, ParameterBinding);
		if (ParameterBinding != nullptr && !ParameterBinding->Parameters.IsEmpty())
		{
			ParameterBinding->Parameters[0].Direction = TEXT("return");
			FString TamperedSource;
			FString ErrorCategory;
			FString ErrorSource;
			TestFalse(TEXT("UTF-8 parameter rejects return direction: ") + TypeName,
				FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
					ParameterTampered,
					TEXT("name-string-parameter-direction-hash"),
					TamperedSource,
					ErrorCategory,
					ErrorSource));
			TestEqual(TEXT("UTF-8 parameter direction reports a stable category: ") + TypeName,
				ErrorCategory,
				FString(TEXT("descriptor_contract_invalid")));
			TestEqual(TEXT("UTF-8 parameter direction reports a stable source: ") + TypeName,
				ErrorSource,
				ParameterBinding->CanonicalIdentity + TEXT(".parameters[0].direction"));
			TestTrue(TEXT("UTF-8 parameter direction emits no partial source: ") + TypeName, TamperedSource.IsEmpty());
		}

		FAvidScriptBindingPackageModel ReturnTampered = Package;
		FAvidScriptBindingFunctionModel* InvalidReturnBinding = ReturnTampered.Bindings.FindByPredicate(
			[&ReturnScriptName](const FAvidScriptBindingFunctionModel& Candidate)
			{
				return Candidate.ScriptName == ReturnScriptName;
			});
		TestNotNull(TEXT("UTF-8 return binding remains available for direction tamper: ") + TypeName, InvalidReturnBinding);
		if (InvalidReturnBinding != nullptr)
		{
			InvalidReturnBinding->ReturnValue.Direction = TEXT("value");
			FString TamperedSource;
			FString ErrorCategory;
			FString ErrorSource;
			TestFalse(TEXT("UTF-8 return rejects parameter direction: ") + TypeName,
				FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
					ReturnTampered,
					TEXT("name-string-return-direction-hash"),
					TamperedSource,
					ErrorCategory,
					ErrorSource));
			TestEqual(TEXT("UTF-8 return direction reports a stable category: ") + TypeName,
				ErrorCategory,
				FString(TEXT("descriptor_contract_invalid")));
			TestEqual(TEXT("UTF-8 return direction reports a stable source: ") + TypeName,
				ErrorSource,
				InvalidReturnBinding->CanonicalIdentity + TEXT(".return.direction"));
			TestTrue(TEXT("UTF-8 return direction emits no partial source: ") + TypeName, TamperedSource.IsEmpty());
		}
	}
	for (const FString& CanonicalType : { TEXT("name:fname"), TEXT("string:fstring") })
	{
		FAvidScriptBindingPackageModel Tampered = Package;
		FAvidScriptBindingTypeModel* Type = Tampered.Types.FindByPredicate(
			[&CanonicalType](const FAvidScriptBindingTypeModel& Candidate) { return Candidate.CanonicalType == CanonicalType; });
		TestNotNull(TEXT("String type remains available for tamper: ") + CanonicalType, Type);
		if (Type == nullptr)
		{
			continue;
		}
		Type->AbiTypes = { TEXT("f") };
		FString TamperedSource;
		FString ErrorCategory;
		FString ErrorSource;
		TestFalse(TEXT("Tampered string ABI fails closed: ") + CanonicalType,
			FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
				Tampered,
				TEXT("name-string-descriptor-hash"),
				TamperedSource,
				ErrorCategory,
				ErrorSource));
		TestTrue(TEXT("Tampered string ABI emits no partial source: ") + CanonicalType, TamperedSource.IsEmpty());
	}

	const FString ProfilePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Source/AvidScriptRuntime/Private/Tests/Fixtures/P57_11B2_NameStringRoundtrip.csharp-profile.json")));
	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!TestTrue(
			TEXT("Name/string C# end-to-end profile loads"),
			FAvidScriptEditorCSharpProfileService::LoadProfile(
				ProfilePath,
				ProfileResult)))
	{
		AddError(ProfileResult.ErrorMessage);
		return false;
	}
	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
			TEXT("Name/string C# fixture builds through the production profile pipeline"),
			FAvidScriptEditorCSharpBuildService::BuildProfile(
				FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ProfileResult),
				BuildResult)))
	{
		AddError(BuildResult.ErrorMessage + TEXT("\n") + BuildResult.Stderr);
		return false;
	}
	TestTrue(
		TEXT("Name/string C# profile publishes its formal report"),
		FPaths::FileExists(BuildResult.ReportPath));
	TestTrue(
		TEXT("Name/string C# profile publishes its runtime manifest"),
		FPaths::FileExists(BuildResult.ManifestPath));
	TestTrue(
		TEXT("Name/string C# profile publishes its runtime binding package"),
		FPaths::FileExists(BuildResult.BindingPackagePath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingEmitterFailureAndPublishTest,
	"AvidScript.Editor.CSharpBindingEmitter.FailureAndPublish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingEmitterFailureAndPublishTest::RunTest(const FString& Parameters)
{
	FString LegacyDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult LegacyDescriptorResult;
	TestTrue(
		TEXT("Legacy-schema fixture starts from a valid descriptor"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(
			LegacyDescriptorJson,
			LegacyDescriptorResult));
	FAvidScriptBindingPackageModel CurrentPackage;
	FString LegacyErrorCategory;
	FString LegacyErrorSource;
	if (TestTrue(
		TEXT("Current descriptor parses before legacy projection"),
		FAvidScriptBindingDescriptorParser::Parse(
			LegacyDescriptorJson,
			CurrentPackage,
			LegacyErrorCategory,
			LegacyErrorSource)))
	{
		for (int32 LegacySchema = 2; LegacySchema <= 5; ++LegacySchema)
		{
			FAvidScriptBindingPackageModel LegacyPackage = CurrentPackage;
			LegacyPackage.SchemaVersion = LegacySchema;
			LegacyPackage.SelfTypeId.Empty();
			if (LegacySchema < 5)
			{
				LegacyPackage.ClassReferences.Empty();
			}
			for (FAvidScriptBindingTypeModel& Type : LegacyPackage.Types)
			{
				Type.ObjectTypeOrdinal = INDEX_NONE;
				Type.ClassPath.Empty();
				Type.BaseTypeId.Empty();
			}
			for (FAvidScriptBindingClassReferenceModel& Reference : LegacyPackage.ClassReferences)
			{
				Reference.ResultTypeId.Empty();
			}
			LegacyPackage.SelectionHash =
				FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(LegacyPackage);
			LegacyPackage.PackageHash =
				FAvidScriptBindingDescriptorIdentity::MakePackageHash(LegacyPackage);

			FString CanonicalLegacyJson;
			TestTrue(
				*FString::Printf(TEXT("Schema v%d canonical descriptor serializes"), LegacySchema),
				FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
					LegacyPackage,
					CanonicalLegacyJson));
			FString LegacySource;
			FString LegacyManifest;
			FAvidScriptCSharpBindingEmitResult LegacyEmitResult;
			TestTrue(
				*FString::Printf(TEXT("Schema v%d descriptor remains compatible with C# emission"), LegacySchema),
				FAvidScriptEditorCSharpBindingEmitter::Emit(
					CanonicalLegacyJson,
					LegacySource,
					LegacyManifest,
					LegacyEmitResult));
			TestTrue(
				*FString::Printf(TEXT("Schema v%d emits a complete legacy facade"), LegacySchema),
				!LegacySource.IsEmpty() && !LegacyManifest.IsEmpty());
		}
	}

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
