#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "AvidScriptWorldSubsystem.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "AvidScriptRuntimeBackendTestLanes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/EngineVersion.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include <initializer_list>

namespace
{
const uint8 GAvidScriptMissingTickWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x16, 0x01, 0x12, 0x61, 0x76, 0x69, 0x64,
	0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69,
	0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00,
	0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
};

const uint8 GAvidScriptTrapTickWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x08,
	0x02, 0x02, 0x00, 0x0b, 0x03, 0x00, 0x00, 0x0b
};

const uint8 GAvidScriptTypedOwnerWrongSignatureWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x01, 0x7f, 0x60, 0x00, 0x00,
	0x02, 0x24, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74,
	0x15, 0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x77, 0x6e, 0x65, 0x72, 0x5f, 0x67, 0x65, 0x74,
	0x5f, 0x68, 0x61, 0x6e, 0x64, 0x6c, 0x65, 0x00, 0x00,
	0x03, 0x02, 0x01, 0x01,
	0x07, 0x16, 0x01, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69,
	0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x01,
	0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
};

void AppendTestWasmBytes(TArray<uint8>& OutBytes, const std::initializer_list<uint8> Bytes)
{
	for (const uint8 Byte : Bytes)
	{
		OutBytes.Add(Byte);
	}
}

void AppendTestWasmU32(TArray<uint8>& OutBytes, uint32 Value)
{
	do
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		if (Value != 0)
		{
			Byte |= 0x80;
		}
		OutBytes.Add(Byte);
	}
	while (Value != 0);
}

void AppendTestWasmName(TArray<uint8>& OutBytes, const ANSICHAR* Name)
{
	const int32 Length = FCStringAnsi::Strlen(Name);
	AppendTestWasmU32(OutBytes, static_cast<uint32>(Length));
	OutBytes.Append(reinterpret_cast<const uint8*>(Name), Length);
}

void AppendTestWasmSection(TArray<uint8>& Module, const uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendTestWasmU32(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

TArray<uint8> MakeTestWasmModuleHeader()
{
	TArray<uint8> Module;
	AppendTestWasmBytes(Module, { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 });
	return Module;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptDynamicHostCallTimingPolicyTest,
	"AvidScript.Runtime.DynamicHostCallTimingPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptDynamicHostCallTimingPolicyTest::RunTest(const FString& Parameters)
{
	FAvidScriptDynamicHostCall Call;
	FAvidScriptDynamicHostCallResult Result;

	FAvidScriptWasmRuntimeInstance ProductionRuntime;
	TestFalse(
		TEXT("Production dynamic call fails without a binding package"),
		ProductionRuntime.DispatchDynamicHostCall(Call, Result));
	TestEqual(
		TEXT("Production dynamic call does not sample the high-resolution clock"),
		ProductionRuntime.GetMetrics().TimedDynamicHostCallCount,
		0);
	TestEqual(
		TEXT("Production dynamic call leaves timing metrics untouched"),
		ProductionRuntime.GetMetrics().HostImportCallMs,
		0.0);

	FAvidScriptWasmHostContext DiagnosticContext;
	DiagnosticContext.DynamicHostCallTimingPolicy =
		EAvidScriptDynamicHostCallTimingPolicy::PerCall;
	FAvidScriptWasmRuntimeInstance DiagnosticRuntime;
	DiagnosticRuntime.SetHostContext(DiagnosticContext);
	TestFalse(
		TEXT("Diagnostic dynamic call fails without a binding package"),
		DiagnosticRuntime.DispatchDynamicHostCall(Call, Result));
	TestEqual(
		TEXT("Diagnostic dynamic call records one timed sample"),
		DiagnosticRuntime.GetMetrics().TimedDynamicHostCallCount,
		1);
	TestTrue(
		TEXT("Diagnostic dynamic call captures elapsed time"),
		DiagnosticRuntime.GetMetrics().HostImportCallMs >= 0.0);
	return true;
}

void AppendTestWasmBeginPlayExport(
	TArray<uint8>& Module,
	const uint32 FunctionIndex,
	const bool bExportMemory)
{
	TArray<uint8> ExportSection;
	AppendTestWasmU32(ExportSection, bExportMemory ? 2 : 1);
	if (bExportMemory)
	{
		AppendTestWasmName(ExportSection, "memory");
		AppendTestWasmBytes(ExportSection, { 0x02, 0x00 });
	}
	AppendTestWasmName(ExportSection, "avid_on_begin_play");
	ExportSection.Add(0x00);
	AppendTestWasmU32(ExportSection, FunctionIndex);
	AppendTestWasmSection(Module, 0x07, ExportSection);
}

void AppendTestWasmSingleFunctionCode(TArray<uint8>& Module, const TArray<uint8>& Body)
{
	TArray<uint8> CodeSection;
	AppendTestWasmU32(CodeSection, 1);
	AppendTestWasmU32(CodeSection, static_cast<uint32>(Body.Num()));
	CodeSection.Append(Body);
	AppendTestWasmSection(Module, 0x0a, CodeSection);
}

TArray<uint8> MakeTypedObjectRouteWasmModule()
{
	TArray<uint8> Module = MakeTestWasmModuleHeader();

	TArray<uint8> TypeSection;
	AppendTestWasmU32(TypeSection, 3);
	AppendTestWasmBytes(TypeSection, { 0x60, 0x00, 0x01, 0x7e });
	AppendTestWasmBytes(TypeSection, { 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f });
	AppendTestWasmBytes(TypeSection, { 0x60, 0x00, 0x00 });
	AppendTestWasmSection(Module, 0x01, TypeSection);

	TArray<uint8> ImportSection;
	AppendTestWasmU32(ImportSection, 2);
	AppendTestWasmName(ImportSection, "avidscript");
	AppendTestWasmName(ImportSection, "avid_owner_get_handle");
	AppendTestWasmBytes(ImportSection, { 0x00, 0x00 });
	AppendTestWasmName(ImportSection, "avidscript");
	AppendTestWasmName(ImportSection, "avid_object_type_is_a");
	AppendTestWasmBytes(ImportSection, { 0x00, 0x01 });
	AppendTestWasmSection(Module, 0x02, ImportSection);

	TArray<uint8> FunctionSection;
	AppendTestWasmBytes(FunctionSection, { 0x01, 0x02 });
	AppendTestWasmSection(Module, 0x03, FunctionSection);

	TArray<uint8> MemorySection;
	AppendTestWasmBytes(MemorySection, { 0x01, 0x00, 0x01 });
	AppendTestWasmSection(Module, 0x05, MemorySection);
	AppendTestWasmBeginPlayExport(Module, 2, true);

	TArray<uint8> Body;
	AppendTestWasmBytes(Body, {
		0x01, 0x01, 0x7e,
		0x10, 0x00, 0x21, 0x00,
		0x41, 0x08, 0x20, 0x00, 0x37, 0x03, 0x00,
		0x41, 0x00, 0x20, 0x00, 0xa7, 0x20, 0x00, 0x42, 0x20, 0x88, 0xa7,
		0x41, 0x00, 0x10, 0x01, 0x36, 0x02, 0x00,
		0x41, 0x04, 0x20, 0x00, 0xa7, 0x20, 0x00, 0x42, 0x20, 0x88, 0xa7,
		0x41, 0x01, 0x10, 0x01, 0x36, 0x02, 0x00,
		0x0b
	});
	AppendTestWasmSingleFunctionCode(Module, Body);
	return Module;
}

TArray<uint8> MakeObjectTypeWrongSignatureWasmModule()
{
	TArray<uint8> Module = MakeTestWasmModuleHeader();
	TArray<uint8> TypeSection;
	AppendTestWasmBytes(TypeSection, {
		0x02,
		0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
		0x60, 0x00, 0x00
	});
	AppendTestWasmSection(Module, 0x01, TypeSection);

	TArray<uint8> ImportSection;
	AppendTestWasmU32(ImportSection, 1);
	AppendTestWasmName(ImportSection, "avidscript");
	AppendTestWasmName(ImportSection, "avid_object_type_is_a");
	AppendTestWasmBytes(ImportSection, { 0x00, 0x00 });
	AppendTestWasmSection(Module, 0x02, ImportSection);

	TArray<uint8> FunctionSection;
	AppendTestWasmBytes(FunctionSection, { 0x01, 0x01 });
	AppendTestWasmSection(Module, 0x03, FunctionSection);
	AppendTestWasmBeginPlayExport(Module, 1, false);
	TArray<uint8> Body;
	AppendTestWasmBytes(Body, { 0x00, 0x0b });
	AppendTestWasmSingleFunctionCode(Module, Body);
	return Module;
}

TArray<uint8> MakeEnvTypedOwnerWasmModule()
{
	TArray<uint8> Module = MakeTestWasmModuleHeader();
	TArray<uint8> TypeSection;
	AppendTestWasmBytes(TypeSection, {
		0x02,
		0x60, 0x00, 0x01, 0x7e,
		0x60, 0x00, 0x00
	});
	AppendTestWasmSection(Module, 0x01, TypeSection);

	TArray<uint8> ImportSection;
	AppendTestWasmU32(ImportSection, 1);
	AppendTestWasmName(ImportSection, "env");
	AppendTestWasmName(ImportSection, "avid_owner_get_handle");
	AppendTestWasmBytes(ImportSection, { 0x00, 0x00 });
	AppendTestWasmSection(Module, 0x02, ImportSection);

	TArray<uint8> FunctionSection;
	AppendTestWasmBytes(FunctionSection, { 0x01, 0x01 });
	AppendTestWasmSection(Module, 0x03, FunctionSection);
	AppendTestWasmBeginPlayExport(Module, 1, false);
	TArray<uint8> Body;
	AppendTestWasmBytes(Body, { 0x00, 0x0b });
	AppendTestWasmSingleFunctionCode(Module, Body);
	return Module;
}

TArray<uint8> MakeEnvLegacyOwnerWasmModule()
{
	TArray<uint8> Module = MakeTestWasmModuleHeader();
	TArray<uint8> TypeSection;
	AppendTestWasmBytes(TypeSection, {
		0x02,
		0x60, 0x00, 0x01, 0x7f,
		0x60, 0x00, 0x00
	});
	AppendTestWasmSection(Module, 0x01, TypeSection);

	TArray<uint8> ImportSection;
	AppendTestWasmU32(ImportSection, 2);
	AppendTestWasmName(ImportSection, "env");
	AppendTestWasmName(ImportSection, "owner_get_slot");
	AppendTestWasmBytes(ImportSection, { 0x00, 0x00 });
	AppendTestWasmName(ImportSection, "env");
	AppendTestWasmName(ImportSection, "owner_get_generation");
	AppendTestWasmBytes(ImportSection, { 0x00, 0x00 });
	AppendTestWasmSection(Module, 0x02, ImportSection);

	TArray<uint8> FunctionSection;
	AppendTestWasmBytes(FunctionSection, { 0x01, 0x01 });
	AppendTestWasmSection(Module, 0x03, FunctionSection);
	TArray<uint8> MemorySection;
	AppendTestWasmBytes(MemorySection, { 0x01, 0x00, 0x01 });
	AppendTestWasmSection(Module, 0x05, MemorySection);
	AppendTestWasmBeginPlayExport(Module, 2, true);

	TArray<uint8> Body;
	AppendTestWasmBytes(Body, {
		0x00,
		0x41, 0x00, 0x10, 0x00, 0x36, 0x02, 0x00,
		0x41, 0x04, 0x10, 0x01, 0x36, 0x02, 0x00,
		0x0b
	});
	AppendTestWasmSingleFunctionCode(Module, Body);
	return Module;
}

FAvidScriptBindingTypeModel MakeTypedOwnerObjectType(
	const TCHAR* ClassPath,
	const int32 ObjectTypeOrdinal,
	const FString& BaseTypeId)
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("object:") + FString(ClassPath);
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(Type.CanonicalType, {});
	Type.Kind = TEXT("object_handle");
	Type.CppType = TEXT("UObject*");
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiTypes = { TEXT("i"), TEXT("i") };
	Type.ObjectTypeOrdinal = ObjectTypeOrdinal;
	Type.ClassPath = ClassPath;
	Type.BaseTypeId = BaseTypeId;
	return Type;
}

bool MakeTypedOwnerDescriptor(const bool bPublishObjectType, FString& OutJson)
{
	FAvidScriptBindingPackageModel Package;
	Package.SchemaVersion = 6;
	Package.GeneratorVersion = TEXT("50.0.test");
	Package.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = bPublishObjectType
		? TEXT("avidscript.test.typed_owner")
		: TEXT("avidscript.test.typed_owner_empty");
	if (bPublishObjectType)
	{
		const FAvidScriptBindingTypeModel ObjectType = MakeTypedOwnerObjectType(
			TEXT("/Script/CoreUObject.Object"),
			0,
			FString());
		const FAvidScriptBindingTypeModel ActorType = MakeTypedOwnerObjectType(
			TEXT("/Script/Engine.Actor"),
			1,
			ObjectType.StableId);
		Package.Types = { ObjectType, ActorType };
		Package.SelfTypeId = ActorType.StableId;

		FAvidScriptBindingClassReferenceModel ActorReference;
		ActorReference.Ordinal = 0;
		ActorReference.ScriptName = TEXT("ActorClass");
		ActorReference.ClassPath = ActorType.ClassPath;
		ActorReference.BaseClassPath = ActorType.ClassPath;
		ActorReference.LoadPolicy = TEXT("EditorLoad");
		ActorReference.ResultTypeId = ActorType.StableId;
		ActorReference.StableId =
			FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
				ActorReference.ClassPath,
				ActorReference.BaseClassPath,
				ActorReference.LoadPolicy);
		Package.ClassReferences.Add(MoveTemp(ActorReference));
	}
	Package.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
	Package.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);

	TArray<FString> TypeJsonEntries;
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		TypeJsonEntries.Add(FString::Printf(
			TEXT("{\"stable_id\":\"%s\",\"canonical_type\":\"%s\",\"kind\":\"object_handle\",\"cpp_type\":\"UObject*\",\"size\":8,\"alignment\":4,\"abi_types\":[\"i\",\"i\"],\"object_type_ordinal\":%d,\"class_path\":\"%s\",\"base_type_id\":\"%s\"}"),
			*Type.StableId,
			*Type.CanonicalType,
			Type.ObjectTypeOrdinal,
			*Type.ClassPath,
			*Type.BaseTypeId));
	}
	const FString TypesJson = TEXT("[") + FString::Join(TypeJsonEntries, TEXT(",")) + TEXT("]");
	FString ClassReferencesJson = TEXT("[]");
	if (!Package.ClassReferences.IsEmpty())
	{
		const FAvidScriptBindingClassReferenceModel& Reference = Package.ClassReferences[0];
		ClassReferencesJson = FString::Printf(
			TEXT("[{\"stable_id\":\"%s\",\"ordinal\":0,\"script_name\":\"%s\",\"class_path\":\"%s\",\"base_class_path\":\"%s\",\"load_policy\":\"%s\",\"result_type_id\":\"%s\"}]"),
			*Reference.StableId,
			*Reference.ScriptName,
			*Reference.ClassPath,
			*Reference.BaseClassPath,
			*Reference.LoadPolicy,
			*Reference.ResultTypeId);
	}

	OutJson = FString::Printf(
		TEXT("{\"schema_version\":6,\"generator_version\":\"%s\",\"engine_version\":\"%s\",\"source\":\"ue_reflection\",\"package_name\":\"%s\",\"package_hash\":\"%s\",\"selection_hash\":\"%s\",\"self_type_id\":\"%s\",\"types\":%s,\"class_references\":%s,\"bindings\":[]}"),
		*Package.GeneratorVersion,
		*Package.EngineVersion,
		*Package.PackageName,
		*Package.PackageHash,
		*Package.SelectionHash,
		*Package.SelfTypeId,
		*TypesJson,
		*ClassReferencesJson);
	return true;
}

TArray<const FAvidScriptVmDynamicImport*> CollectTypedOwnerObjectTypeImports(
	const FAvidScriptBindingPackage& Package)
{
	TArray<const FAvidScriptVmDynamicImport*> Imports;
	for (const FAvidScriptVmDynamicImport& Import : Package.GetVmPackage().Imports)
	{
		if (Import.ImportName == TEXT("avid_object_type_is_a"))
		{
			Imports.Add(&Import);
		}
	}
	return Imports;
}

bool CreateSmokeWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::PIE, false, TEXT("AvidScriptSmokeWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::PIE);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroySmokeWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}

	if (World->HasBegunPlay())
	{
		World->EndPlay(EEndPlayReason::Quit);
	}

	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}

	World->DestroyWorld(false);
	World = nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTypedOwnerImportsTest,
	"AvidScript.Runtime.TypedOwnerImports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedOwnerImportsTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext HostContext;
	HostContext.OwnerHandle = { 0x89abcdefu, 0x01234567u };
	Runtime.SetHostContext(HostContext);
	TestEqual(
		TEXT("Packed typed owner handle requires package authorization"),
		Runtime.HandleOwnerGetHandleImport(),
		static_cast<int64>(0));
	FString FailureModule;
	FString FailureName;
	FString FailureDetails;
	TestTrue(
		TEXT("Missing typed owner package reports a pending failure"),
		Runtime.ConsumePendingHostImportFailure(
			FailureModule,
			FailureName,
			FailureDetails));
	TestEqual(
		TEXT("Missing package failure keeps the canonical import"),
		FailureName,
		FString(TEXT("avid_owner_get_handle")));

	HostContext.OwnerHandle = { 7, 0 };
	Runtime.SetHostContext(HostContext);
	TestEqual(TEXT("Typed owner rejects a zero generation"), Runtime.HandleOwnerGetHandleImport(), static_cast<int64>(0));
	TestTrue(TEXT("Zero generation reports a pending failure"),
		Runtime.ConsumePendingHostImportFailure(FailureModule, FailureName, FailureDetails));
	TestEqual(TEXT("Typed owner failure keeps the canonical module"), FailureModule, FString(TEXT("avidscript")));
	TestEqual(TEXT("Typed owner failure keeps the canonical import"), FailureName, FString(TEXT("avid_owner_get_handle")));

	HostContext.OwnerHandle = { 0, 9 };
	Runtime.SetHostContext(HostContext);
	TestEqual(TEXT("Typed owner rejects a zero slot"), Runtime.HandleOwnerGetHandleImport(), static_cast<int64>(0));
	TestTrue(TEXT("Zero slot reports a pending failure"),
		Runtime.ConsumePendingHostImportFailure(FailureModule, FailureName, FailureDetails));

	FAvidScriptWasmSmokeResult WrongSignatureResult;
	TestFalse(TEXT("Typed owner import rejects an i32 signature before instantiation"), Runtime.LoadModule(
		GAvidScriptTypedOwnerWrongSignatureWasmModule,
		UE_ARRAY_COUNT(GAvidScriptTypedOwnerWrongSignatureWasmModule),
		TEXT("typed_owner_wrong_signature"),
		WrongSignatureResult));
	TestFalse(TEXT("Wrong typed owner signature never instantiates a module"), WrongSignatureResult.bModuleInstantiated);

	const TArray<uint8> EnvTypedOwnerModule = MakeEnvTypedOwnerWasmModule();
	FAvidScriptWasmSmokeResult EnvTypedOwnerResult;
	TestFalse(TEXT("env does not authorize the canonical typed owner import"), Runtime.LoadModule(
		EnvTypedOwnerModule.GetData(),
		EnvTypedOwnerModule.Num(),
		TEXT("env_typed_owner_import"),
		EnvTypedOwnerResult));
	TestFalse(TEXT("Rejected env typed owner import never instantiates a module"),
		EnvTypedOwnerResult.bModuleInstantiated);

	FString EmptyDescriptorJson;
	TestTrue(TEXT("Empty v6 descriptor serializes"), MakeTypedOwnerDescriptor(false, EmptyDescriptorJson));
	TSharedPtr<const FAvidScriptBindingPackage> EmptyPackage;
	FAvidScriptBindingPackageLoadResult EmptyPackageResult;
	TestFalse(TEXT("Empty v6 descriptor fails closed"),
		FAvidScriptBindingPackage::LoadDescriptor(
			EmptyDescriptorJson,
			EmptyPackage,
			EmptyPackageResult));
	TestNull(TEXT("Rejected empty descriptor produces no package"), EmptyPackage.Get());
	TestEqual(TEXT("Empty descriptor rejection keeps the contract category"),
		EmptyPackageResult.ErrorCategory,
		FString(TEXT("descriptor_contract_invalid")));
	TestEqual(TEXT("Empty descriptor rejection identifies its missing capability"),
		EmptyPackageResult.ErrorSource,
		FString(TEXT("bindings|class_references")));

	FString PublishedDescriptorJson;
	TestTrue(TEXT("Published v6 descriptor serializes"), MakeTypedOwnerDescriptor(true, PublishedDescriptorJson));
	TSharedPtr<const FAvidScriptBindingPackage> PublishedPackage;
	FAvidScriptBindingPackageLoadResult PublishedPackageResult;
	if (!TestTrue(TEXT("Published v6 descriptor loads"),
		FAvidScriptBindingPackage::LoadDescriptor(PublishedDescriptorJson, PublishedPackage, PublishedPackageResult))
		|| !TestNotNull(TEXT("Published descriptor produces a package"), PublishedPackage.Get()))
	{
		AddError(PublishedPackageResult.ErrorCategory + TEXT(": ") + PublishedPackageResult.ErrorDetails);
		return false;
	}
	const TArray<const FAvidScriptVmDynamicImport*> PublishedObjectTypeImports =
		CollectTypedOwnerObjectTypeImports(*PublishedPackage);
	if (!TestEqual(TEXT("Published v6 descriptor authorizes exactly one same-name object-type import"),
		PublishedObjectTypeImports.Num(), 1))
	{
		return false;
	}
	const FAvidScriptVmDynamicImport& ObjectTypeImport = *PublishedObjectTypeImports[0];
	TestEqual(TEXT("Object-type import uses the canonical module"),
		ObjectTypeImport.ModuleName, FString(TEXT("avidscript")));
	TestEqual(TEXT("Object-type import uses the canonical name"),
		ObjectTypeImport.ImportName, FString(TEXT("avid_object_type_is_a")));
	TestEqual(TEXT("Object-type import uses the fixed signature"),
		ObjectTypeImport.Signature, FString(TEXT("(iii)i")));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	UObject* OwnerObject =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	const FAvidScriptObjectHandle OwnerHandle = Registry.RegisterObject(OwnerObject, RegisterResult, false);
	if (!TestTrue(TEXT("Typed owner target registers in the runtime registry"),
		RegisterResult.bSucceeded && OwnerHandle.IsValid()))
	{
		return false;
	}
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = OwnerHandle;

	const TArray<uint8> TypedObjectRouteModule = MakeTypedObjectRouteWasmModule();
	const TArray<uint8> WrongObjectTypeSignatureModule = MakeObjectTypeWrongSignatureWasmModule();
	const TArray<uint8> EnvLegacyOwnerModule = MakeEnvLegacyOwnerWasmModule();
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance LaneRuntime(Lane.Selection);
		LaneRuntime.SetHostContext(HostContext);
		FAvidScriptWasmSmokeResult RouteLoadResult;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("typed owner and object-type imports instantiate")),
			LaneRuntime.LoadModule(
				TypedObjectRouteModule.GetData(),
				TypedObjectRouteModule.Num(),
				TEXT("typed_owner_object_route"),
				PublishedPackage,
				RouteLoadResult)))
		{
			AddError(RouteLoadResult.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, RouteLoadResult);

		FAvidScriptWasmSmokeResult RouteBeginPlayResult;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("typed owner and object-type package routes execute")),
			LaneRuntime.BeginPlay(RouteBeginPlayResult)))
		{
			AddError(RouteBeginPlayResult.ErrorMessage);
			continue;
		}
		TArray<uint8> RouteState;
		RouteState.SetNumZeroed(16);
		FString StateReadError;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("route writes observable guest state")),
			LaneRuntime.ReadStateBytes(0, MakeArrayView(RouteState), StateReadError)))
		{
			AddError(StateReadError);
			continue;
		}
		int32 ObjectMatch = 0;
		int32 ObjectMismatch = 0;
		uint64 PackedOwnerHandle = 0;
		FMemory::Memcpy(&ObjectMatch, RouteState.GetData(), sizeof(ObjectMatch));
		FMemory::Memcpy(&ObjectMismatch, RouteState.GetData() + 4, sizeof(ObjectMismatch));
		FMemory::Memcpy(&PackedOwnerHandle, RouteState.GetData() + 8, sizeof(PackedOwnerHandle));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("UObject type match")), ObjectMatch, 1);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Actor type mismatch")), ObjectMismatch, 0);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("packed owner handle")), PackedOwnerHandle, OwnerHandle.ToUInt64());
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("static plus dynamic host call count")), RouteBeginPlayResult.HostImportCallCount, 3);
		TestEqual(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("last dynamic binding ordinal")),
			RouteBeginPlayResult.LastHostImportInput,
			static_cast<int32>(ObjectTypeImport.Ordinal));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("last dynamic result")), RouteBeginPlayResult.LastHostImportResult, 0);
		LaneRuntime.Unload();

		FAvidScriptWasmSmokeResult WrongObjectTypeSignatureResult;
		TestFalse(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("wrong dynamic signature is rejected")),
			LaneRuntime.LoadModule(
				WrongObjectTypeSignatureModule.GetData(),
				WrongObjectTypeSignatureModule.Num(),
				TEXT("object_type_wrong_signature"),
				PublishedPackage,
				WrongObjectTypeSignatureResult));
		TestFalse(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("wrong signature never instantiates")),
			WrongObjectTypeSignatureResult.bModuleInstantiated);

		FAvidScriptWasmSmokeResult LegacyLoadResult;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("legacy env owner imports remain compatible")),
			LaneRuntime.LoadModule(
				EnvLegacyOwnerModule.GetData(),
				EnvLegacyOwnerModule.Num(),
				TEXT("env_legacy_owner_imports"),
				LegacyLoadResult)))
		{
			AddError(LegacyLoadResult.ErrorMessage);
			continue;
		}
		FAvidScriptWasmSmokeResult LegacyBeginPlayResult;
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("legacy env owner imports execute")),
			LaneRuntime.BeginPlay(LegacyBeginPlayResult));
		TArray<uint8> LegacyState;
		LegacyState.SetNumZeroed(8);
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("legacy owner imports write guest state")),
			LaneRuntime.ReadStateBytes(0, MakeArrayView(LegacyState), StateReadError)))
		{
			AddError(StateReadError);
			continue;
		}
		uint32 LegacySlot = 0;
		uint32 LegacyGeneration = 0;
		FMemory::Memcpy(&LegacySlot, LegacyState.GetData(), sizeof(LegacySlot));
		FMemory::Memcpy(&LegacyGeneration, LegacyState.GetData() + 4, sizeof(LegacyGeneration));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("legacy owner slot")), LegacySlot, OwnerHandle.Slot);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("legacy owner generation")), LegacyGeneration, OwnerHandle.Generation);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBindingDescriptorLegacyIdentityTest,
	"AvidScript.Runtime.Binding.DescriptorLegacyIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBindingDescriptorLegacyIdentityTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingPackageModel Package;
	Package.GeneratorVersion = TEXT("legacy.generator");
	Package.EngineVersion = TEXT("5.8.0");
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = TEXT("legacy.package");

	FAvidScriptBindingTypeModel Type;
	Type.StableId = TEXT("type-id");
	Type.CanonicalType = TEXT("bool");
	Type.Kind = TEXT("scalar");
	Type.CppType = TEXT("bool");
	Type.Size = 1;
	Type.Alignment = 1;
	Type.AbiTypes = { TEXT("i") };
	Package.Types.Add(Type);

	FAvidScriptBindingFunctionModel Binding;
	Binding.StableId = TEXT("binding-id");
	Binding.CanonicalIdentity = TEXT("owner::fn(void)");
	Binding.Ordinal = 0;
	Binding.OwnerClass = TEXT("/Script/Engine.Actor");
	Binding.UeMember = TEXT("Fn");
	Binding.UeFunction = TEXT("Fn");
	Binding.ScriptName = TEXT("Fn");
	Binding.DispatchMode = TEXT("cached_process_event");
	Binding.bStatic = false;
	Binding.bConst = true;
	Binding.ReloadEffect = EAvidScriptBindingReloadEffect::None;
	Binding.ReturnValue.Name = TEXT("return");
	Binding.ReturnValue.Direction = TEXT("return");
	Binding.ReturnValue.CanonicalType = TEXT("void");
	Binding.ReturnValue.TypeId = TEXT("void-id");
	Binding.ReturnValue.Kind = TEXT("void");
	Binding.ReturnValue.CppType = TEXT("void");
	Binding.HostImport.Module = TEXT("avidscript");
	Binding.HostImport.Name = TEXT("avid_ue_test");
	Binding.HostImport.Signature = TEXT("(ii)i");
	Package.Bindings.Add(Binding);

	const TCHAR* ExpectedSelectionHashes[] = {
		TEXT(""),
		TEXT(""),
		TEXT("267679b8a642f30d296bd8645580f271ef25bb7bd4e4c42244eeceae83ba4d54"),
		TEXT("267679b8a642f30d296bd8645580f271ef25bb7bd4e4c42244eeceae83ba4d54"),
		TEXT("267679b8a642f30d296bd8645580f271ef25bb7bd4e4c42244eeceae83ba4d54"),
		TEXT("bcd25af7b336196501ff44962e5c23e032d23cd8d9bb0c0792b1aaad32a1f75f"),
		TEXT("32c282b301d29454ee56e2044bd9e349c22304664a744a2e5eb01004d01198f1")
	};
	const TCHAR* ExpectedPackageHashes[] = {
		TEXT(""),
		TEXT(""),
		TEXT("54fe33c5d11181fee26c57d10b8a64e210fd0f0b4c58bf61b3a05825c1d6ffc1"),
		TEXT("beb2669bb1a4c51c206884876db5bf7a0f370e963cc1b70343163beb13095c6b"),
		TEXT("beb2669bb1a4c51c206884876db5bf7a0f370e963cc1b70343163beb13095c6b"),
		TEXT("1f973d1960e15c88c361b7b88855ebaa5de502b36c2542dfd99a286986aad3da"),
		TEXT("0dcfaccbcb47831268e6b16a5da658970817659c76c7ae4efb22c8425d294c81")
	};

	for (int32 SchemaVersion = 2; SchemaVersion <= 6; ++SchemaVersion)
	{
		Package.SchemaVersion = SchemaVersion;
		Package.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
		TestEqual(
			*FString::Printf(TEXT("Schema v%d selection hash keeps legacy bytes"), SchemaVersion),
			Package.SelectionHash,
			FString(ExpectedSelectionHashes[SchemaVersion]));
		TestEqual(
			*FString::Printf(TEXT("Schema v%d package hash keeps legacy bytes"), SchemaVersion),
			FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package),
			FString(ExpectedPackageHashes[SchemaVersion]));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptMinimalWasmSmokeTest,
	"AvidScript.Runtime.MinimalWasmSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptMinimalWasmSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmSmokeResult Result;
	const bool bSucceeded = FAvidScriptWasmRuntime::RunEmbeddedSmokeTest(Result);

	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("WAMR embedded smoke test succeeds"), bSucceeded);
	TestTrue(TEXT("avid_on_begin_play export is called"), Result.bBeginPlayCalled);
	TestTrue(TEXT("avid_on_tick export is called"), Result.bTickCalled);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPackagedTimingSmokeTest,
	"AvidScript.Runtime.PackagedTimingSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPackagedTimingSmokeTest::RunTest(const FString& Parameters)
{
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;

		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("embedded module loads")),
			Runtime.LoadEmbeddedSmokeModule(Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("runtime init timing is captured")), Result.Metrics.RuntimeInitMs >= 0.0);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("module load timing is captured")), Result.Metrics.ModuleLoadMs > 0.0);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("module instantiate timing is captured")), Result.Metrics.ModuleInstantiateMs > 0.0);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("exec env creation timing is captured")), Result.Metrics.ExecEnvCreateMs >= 0.0);

		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("BeginPlay export succeeds")), Runtime.BeginPlay(Result));
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("BeginPlay timing is captured")), Result.Metrics.BeginPlayCallMs >= 0.0);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Tick export succeeds")), Runtime.Tick(1.0f / 60.0f, Result));
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Tick timing is captured")), Result.Metrics.TickCallMs >= 0.0);
		Result.ErrorCategory = TEXT("stale_success_error");
		TestTrue(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("failure-only Tick succeeds")),
			Runtime.Tick(
				1.0f / 60.0f,
				Result,
				EAvidScriptWasmResultDetail::FailureOnly));
		TestTrue(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("failure-only success clears stale errors")),
			Result.ErrorCategory.IsEmpty());
		TestTrue(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("failure-only success updates Tick state")),
			Result.bTickCalled && Result.TickCallCount == 2);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing optional EndPlay is a no-op")), Runtime.EndPlay(Result));
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing EndPlay is not marked called")), Result.bEndPlayCalled);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing EndPlay has zero call time")), Result.Metrics.EndPlayCallMs, 0.0);

		Runtime.Unload(Result);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("unload reports completed state")), Result.bUnloaded);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("unload timing is captured")), Result.Metrics.UnloadMs >= 0.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmSessionRestartSmokeTest,
	"AvidScript.Runtime.SessionRestartSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmSessionRestartSmokeTest::RunTest(const FString& Parameters)
{
	for (int32 RunIndex = 0; RunIndex < 2; ++RunIndex)
	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult Result;

		TestTrue(TEXT("Embedded module loads"), Runtime.LoadEmbeddedSmokeModule(Result));
		TestTrue(TEXT("Runtime reports loaded"), Runtime.IsLoaded());
		TestTrue(TEXT("BeginPlay export succeeds"), Runtime.BeginPlay(Result));
		TestTrue(TEXT("Tick export succeeds"), Runtime.Tick(1.0f / 60.0f, Result));
		TestEqual(TEXT("Tick call count"), Runtime.GetTickCallCount(), 1);

		Runtime.Unload();
		TestFalse(TEXT("Runtime unload clears loaded state"), Runtime.IsLoaded());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmErrorSmokeTest,
	"AvidScript.Runtime.ErrorSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmErrorSmokeTest::RunTest(const FString& Parameters)
{
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;

		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("module missing tick export still loads")),
			Runtime.LoadModule(GAvidScriptMissingTickWasmModule, UE_ARRAY_COUNT(GAvidScriptMissingTickWasmModule), TEXT("missing_tick"), Result));
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("BeginPlay still succeeds")), Runtime.BeginPlay(Result));
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing Tick export is reported")), Runtime.Tick(1.0f / 60.0f, Result));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing export category")), Result.ErrorCategory, FString(TEXT("missing_export")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing export name")), Result.ExportName, FString(TEXT("avid_on_tick")));

		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("trap module loads")),
			Runtime.LoadModule(GAvidScriptTrapTickWasmModule, UE_ARRAY_COUNT(GAvidScriptTrapTickWasmModule), TEXT("trap_tick"), Result));
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("BeginPlay succeeds before trap")), Runtime.BeginPlay(Result));
		TestFalse(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("failure-only Tick trap is reported")),
			Runtime.Tick(
				1.0f / 60.0f,
				Result,
				EAvidScriptWasmResultDetail::FailureOnly));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("trap category")), Result.ErrorCategory, FString(TEXT("trap")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("trap export name")), Result.ExportName, FString(TEXT("avid_on_tick")));
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("trap preserves stack frames")), !Result.DiagnosticFrames.IsEmpty());
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("failure-only trap materializes module identity")),
			Result.ModuleId,
			FString(TEXT("trap_tick")));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("failure-only trap materializes backend identity")),
			Result.BackendInfo.Kind,
			Lane.Selection.BackendKind);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWorldSubsystemLifecycleSmokeTest,
	"AvidScript.Runtime.WorldSubsystemLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWorldSubsystemLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateSmokeWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript smoke world."));
		DestroySmokeWorld(World);
		return true;
	}

	UAvidScriptWorldSubsystem* Subsystem = World->GetSubsystem<UAvidScriptWorldSubsystem>();
	TestNotNull(TEXT("AvidScript world subsystem exists for PIE worlds"), Subsystem);

	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	World->Tick(LEVELTICK_All, 1.0f / 60.0f);

	if (Subsystem != nullptr)
	{
		const FAvidScriptWorldRuntimeStats StatsAfterTick = Subsystem->GetRuntimeStats();
		TestTrue(TEXT("Subsystem loaded runtime on BeginPlay"), StatsAfterTick.bRuntimeLoaded);
		TestTrue(TEXT("Subsystem called avid_on_begin_play"), StatsAfterTick.bBeginPlayCalled);
		TestTrue(TEXT("Subsystem ticked avid_on_tick"), StatsAfterTick.TickCallCount > 0);
	}

	TestTrue(TEXT("Smoke world routes EndPlay to world subsystems"), World->EndPlay(EEndPlayReason::Quit));

	if (Subsystem != nullptr)
	{
		const FAvidScriptWorldRuntimeStats StatsAfterEndPlay = Subsystem->GetRuntimeStats();
		TestFalse(TEXT("Subsystem unloads runtime on EndPlay"), StatsAfterEndPlay.bRuntimeLoaded);
		TestTrue(TEXT("Subsystem records EndPlay cleanup"), StatsAfterEndPlay.bEndPlayCalled);
	}

	DestroySmokeWorld(World);
	return true;
}

#endif
