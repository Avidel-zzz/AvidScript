#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "AvidScriptHash.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
constexpr TCHAR GeneratedExportName[] =
	TEXT("avid_ue_0123456789abcdef0123456789abcdef");
constexpr TCHAR GeneratedTypeGenerationKey[] =
	TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

void AppendWasmSection(
	TArray<uint8>& Module,
	const uint8 SectionId,
	const TConstArrayView<uint8> Payload)
{
	check(Payload.Num() < 128);
	Module.Add(SectionId);
	Module.Add(static_cast<uint8>(Payload.Num()));
	Module.Append(Payload.GetData(), Payload.Num());
}

void AppendWasmExport(
	TArray<uint8>& Payload,
	const ANSICHAR* Name,
	const uint8 FunctionIndex)
{
	const int32 NameLength = FCStringAnsi::Strlen(Name);
	check(NameLength < 128);
	Payload.Add(static_cast<uint8>(NameLength));
	Payload.Append(reinterpret_cast<const uint8*>(Name), NameLength);
	Payload.Add(0x00);
	Payload.Add(FunctionIndex);
}

TArray<uint8> BuildGeneratedTypeSessionModule(const int32 ReturnConstant = INDEX_NONE)
{
	check(ReturnConstant == INDEX_NONE || ReturnConstant >= 0 && ReturnConstant < 64);
	TArray<uint8> Module = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00
	};
	const TArray<uint8> TypeSection = {
		0x02,
		0x60, 0x00, 0x00,
		0x60, 0x01, 0x7e, 0x01, 0x7f
	};
	AppendWasmSection(Module, 0x01, TypeSection);
	const TArray<uint8> FunctionSection = { 0x02, 0x00, 0x01 };
	AppendWasmSection(Module, 0x03, FunctionSection);

	TArray<uint8> ExportSection = { 0x02 };
	AppendWasmExport(ExportSection, "avid_on_begin_play", 0);
	AppendWasmExport(
		ExportSection,
		"avid_ue_0123456789abcdef0123456789abcdef",
		1);
	AppendWasmSection(Module, 0x07, ExportSection);
	const TArray<uint8> CodeSection = ReturnConstant == INDEX_NONE
		? TArray<uint8>{
			0x02,
			0x02, 0x00, 0x0b,
			0x05, 0x00, 0x20, 0x00, 0xa7, 0x0b
		}
		: TArray<uint8>{
			0x02,
			0x02, 0x00, 0x0b,
			0x04, 0x00, 0x41, static_cast<uint8>(ReturnConstant), 0x0b
		};
	AppendWasmSection(Module, 0x0a, CodeSection);
	return Module;
}

FString BuildGeneratedTypeSessionManifest(
	const TCHAR* const StableTypeId = TEXT("type:generated-session-fixture"))
{
	return FString::Printf(
		TEXT(R"JSON({
  "schema_version": 5,
  "generator_version": "1.7",
  "module_name": "AvidScriptRuntime",
  "generation_key_sha256": "%s",
  "types": [
    {
      "type_ordinal": 0,
      "stable_type_id": "%s",
      "engine_name": "AvidScriptGeneratedTypeSessionTestObject",
      "class_path": "%s",
      "properties": [],
      "functions": [
        {
          "member_ordinal": 0,
          "stable_member_id": "function:get-script-value",
          "native_name": "GetScriptValue",
          "export_name": "%s",
          "flags": []
        }
      ]
    }
  ]
})JSON"),
		GeneratedTypeGenerationKey,
		StableTypeId,
		*UAvidScriptGeneratedTypeSessionTestObject::StaticClass()->GetPathName(),
		GeneratedExportName);
}

bool SaveUtf8Fixture(
	const FString& Path,
	const FString& Text,
	TArray<uint8>& OutBytes)
{
	FTCHARToUTF8 Utf8(*Text);
	OutBytes.Reset(Utf8.Length());
	OutBytes.Append(
		reinterpret_cast<const uint8*>(Utf8.Get()),
		Utf8.Length());
	return FFileHelper::SaveArrayToFile(OutBytes, *Path);
}

FString BuildRuntimeManifestFixture(const FString& WasmSha256)
{
	return FString::Printf(
		TEXT(R"JSON({
  "schema_version": 1,
  "module_id": "generated_type_runtime_host",
  "abi_version": 1,
  "language": "csharp",
  "source": { "file": "generated-type-fixture.cs" },
  "wasm": { "file": "generated-types.wasm", "sha256": "%s" },
  "required_exports": ["avid_on_begin_play"],
  "required_imports": [],
  "toolchain": { "compiler": "avidscript-csharp-guest-wasm" }
})JSON"),
		*WasmSha256);
}

FString BuildPackageDescriptorFixture(
	const FString& TypeManifestSha256,
	const FString& RuntimeManifestSha256)
{
	const FString PackageId = FAvidScriptHash::Sha256HexUtf8(FString::Printf(
		TEXT("%s\n%s\n%s"),
		GeneratedTypeGenerationKey,
		*TypeManifestSha256,
		*RuntimeManifestSha256));
	return FString::Printf(
		TEXT(R"JSON({
  "schema_version": 1,
  "package_id": "%s",
  "module_name": "AvidScriptRuntime",
  "runtime_module_id": "generated_type_runtime_host",
  "execution_backend": "wasmtime_jit",
  "generation_key_sha256": "%s",
  "type_manifest": {
    "file": "generated-types.json",
    "sha256": "%s"
  },
  "runtime_manifest": {
    "file": "generated-types.avidscript.json",
    "sha256": "%s"
  }
})JSON"),
		*PackageId,
		GeneratedTypeGenerationKey,
		*TypeManifestSha256,
		*RuntimeManifestSha256);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedTypeSessionTest,
	"AvidScript.Runtime.GeneratedTypes.SessionPreparedDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedTypeSessionTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FString Error;
	if (!TestTrue(
		TEXT("Generated type registry builds"),
		FAvidScriptGeneratedTypeRegistry::BuildFromJson(
			BuildGeneratedTypeSessionManifest(),
			Registry,
			Error)))
	{
		AddError(Error);
		return true;
	}

	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Receiver(
		NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	const FAvidScriptObjectHandle ReceiverHandle{ 17, 5 };
	FAvidScriptRuntimeSession Session;
	TestTrue(
		TEXT("Session owns the generated receiver route"),
		Session.ConfigureGeneratedTypeInstance(
			*Receiver,
			ReceiverHandle,
			0,
			Registry,
			Error));

	int32 ScriptResult = 0;
	TestFalse(
		TEXT("Prepared dispatch fails closed before a live runtime exists"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));

	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Session.SetBackendSelectionForTesting(Selection);
	FAvidScriptWasmReloadManifest Manifest;
	Manifest.ModuleId = TEXT("generated_type_session");
	Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
	Manifest.Language = TEXT("wasm");
	Manifest.RequiredExports = { TEXT("avid_on_begin_play") };
	const TArray<uint8> Module = BuildGeneratedTypeSessionModule();
	FAvidScriptWasmReloadResult LoadResult;
	if (!TestTrue(
		TEXT("Session loads and prepares canonical generated exports"),
		Session.LoadInitialModule(Module.GetData(), Module.Num(), Manifest, LoadResult)))
	{
		AddError(LoadResult.ErrorMessage);
		return true;
	}

	TestTrue(
		TEXT("Generated dispatch enters the prepared WASM export"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));
	TestEqual(TEXT("Packed ObjectHandle low cell reaches C# this"), ScriptResult, 17);

	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(
		TEXT("Reload transaction replaces generated prepared calls"),
		Session.ReloadModule(Module.GetData(), Module.Num(), Manifest, ReloadResult));
	ScriptResult = 0;
	TestTrue(
		TEXT("Generated dispatch survives reload without a stale export"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));
	TestEqual(TEXT("Reloaded export preserves receiver identity"), ScriptResult, 17);

	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(TEXT("Session unload succeeds"), Session.StopAndUnload(StopResult));
	TestFalse(
		TEXT("Unloaded Session fences generated dispatch"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));
	TestTrue(
		TEXT("Generated receiver route tears down explicitly"),
		Session.ClearGeneratedTypeInstance(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedTypeRuntimeHostTest,
	"AvidScript.Runtime.GeneratedTypes.RuntimeHost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedTypeRuntimeHostTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	FString Error;
	const FString PackageRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests/GeneratedTypeRuntimePackage")));
	IFileManager::Get().DeleteDirectory(*PackageRoot, false, true);
	if (!TestTrue(
		TEXT("Generated type package fixture directory is created"),
		IFileManager::Get().MakeDirectory(*PackageRoot, true)))
	{
		return true;
	}
	const TArray<uint8> Module = BuildGeneratedTypeSessionModule();
	const FString WasmPath = FPaths::Combine(PackageRoot, TEXT("generated-types.wasm"));
	const FString TypeManifestPath = FPaths::Combine(PackageRoot, TEXT("generated-types.json"));
	const FString RuntimeManifestPath = FPaths::Combine(
		PackageRoot,
		TEXT("generated-types.avidscript.json"));
	const FString DescriptorPath = FPaths::Combine(PackageRoot, TEXT("package.json"));
	TestTrue(
		TEXT("Generated type package WASM writes"),
		FFileHelper::SaveArrayToFile(Module, *WasmPath));
	TArray<uint8> TypeManifestBytes;
	TArray<uint8> RuntimeManifestBytes;
	TArray<uint8> DescriptorBytes;
	const FString TypeManifestJson = BuildGeneratedTypeSessionManifest();
	TestTrue(
		TEXT("Generated type manifest writes"),
		SaveUtf8Fixture(TypeManifestPath, TypeManifestJson, TypeManifestBytes));
	const FString RuntimeManifestJson = BuildRuntimeManifestFixture(
		FAvidScriptHash::Sha256Hex(Module));
	TestTrue(
		TEXT("Generated type Runtime manifest writes"),
		SaveUtf8Fixture(
			RuntimeManifestPath,
			RuntimeManifestJson,
			RuntimeManifestBytes));
	const FString TypeManifestSha256 = FAvidScriptHash::Sha256Hex(TypeManifestBytes);
	const FString RuntimeManifestSha256 = FAvidScriptHash::Sha256Hex(RuntimeManifestBytes);
	const FString CorruptRuntimeSha256 = FString::ChrN(64, TEXT('f'));
	TestTrue(
		TEXT("Corrupt generated package descriptor writes"),
		SaveUtf8Fixture(
			DescriptorPath,
			BuildPackageDescriptorFixture(
				TypeManifestSha256,
				CorruptRuntimeSha256),
			DescriptorBytes));
	TUniquePtr<FAvidScriptGeneratedTypeRuntimeHost> Host =
		FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
	ON_SCOPE_EXIT
	{
		Host->Shutdown();
	};
	TestFalse(
		TEXT("Generated package descriptor rejects a Runtime manifest hash mismatch"),
		Host->InstallPackageFromDescriptorFile(DescriptorPath, Error));
	TestTrue(
		TEXT("Valid generated package descriptor writes"),
		SaveUtf8Fixture(
			DescriptorPath,
			BuildPackageDescriptorFixture(
				TypeManifestSha256,
				RuntimeManifestSha256),
			DescriptorBytes));
	if (!TestTrue(
		TEXT("Runtime host installs a verified generated package descriptor"),
		Host->InstallPackageFromDescriptorFile(DescriptorPath, Error)))
	{
		AddError(Error);
		return true;
	}

	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Receiver(
		NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	if (!TestTrue(
		TEXT("Runtime host creates ObjectHandle, Session and router registration"),
		Host->BeginInstance(*Receiver, 0, Error)))
	{
		AddError(Error);
		Host->ClearPackage(Error);
		return true;
	}
	TestTrue(TEXT("Runtime host records the active receiver"), Host->IsInstanceActive(*Receiver));
	TestEqual(TEXT("Runtime host owns one Session"), Host->GetActiveInstanceCount(), 1);
	TestEqual(TEXT("Runtime host owns one anchored ObjectHandle"), Host->GetRegisteredHandleCount(), 1);
	TestTrue(
		TEXT("Repeated native Super-chain activation is idempotent"),
		Host->BeginInstance(*Receiver, 0, Error));
	TestEqual(TEXT("Idempotent activation preserves one Session"), Host->GetActiveInstanceCount(), 1);

	int32 ScriptResult = 0;
	TestTrue(
		TEXT("Runtime-owned route dispatches into the prepared Wasmtime export"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));
	TestTrue(TEXT("Runtime-owned packed handle reaches the guest"), ScriptResult > 0);

	const TArray<uint8> BodyOnlyModule = BuildGeneratedTypeSessionModule(37);
	TestTrue(
		TEXT("Body-only candidate WASM writes"),
		FFileHelper::SaveArrayToFile(BodyOnlyModule, *WasmPath));
	const FString BodyOnlyRuntimeManifestJson = BuildRuntimeManifestFixture(
		FAvidScriptHash::Sha256Hex(BodyOnlyModule));
	TestTrue(
		TEXT("Body-only Runtime manifest writes"),
		SaveUtf8Fixture(
			RuntimeManifestPath,
			BodyOnlyRuntimeManifestJson,
			RuntimeManifestBytes));
	const FString BodyOnlyRuntimeManifestSha256 =
		FAvidScriptHash::Sha256Hex(RuntimeManifestBytes);
	TestTrue(
		TEXT("Body-only package descriptor writes"),
		SaveUtf8Fixture(
			DescriptorPath,
			BuildPackageDescriptorFixture(
				TypeManifestSha256,
				BodyOnlyRuntimeManifestSha256),
			DescriptorBytes));
	FAvidScriptGeneratedTypePackageReloadResult BodyOnlyResult;
	if (!TestTrue(
		TEXT("Active generated package accepts a body-only descriptor reload"),
		Host->ReloadPackageFromDescriptorFile(
			DescriptorPath,
			BodyOnlyResult,
			Error)))
	{
		AddError(Error);
		Host->EndInstance(*Receiver, Error);
		Host->ClearPackage(Error);
		return true;
	}
	TestTrue(
		TEXT("Body-only reload reports its applied disposition"),
		BodyOnlyResult.Disposition
			== EAvidScriptGeneratedTypePackageReloadDisposition::BodyOnlyApplied);
	TestEqual(TEXT("Body-only reload sees one active Session"), BodyOnlyResult.CandidateInstanceCount, 1);
	TestEqual(TEXT("Body-only reload commits one active Session"), BodyOnlyResult.ReloadedInstanceCount, 1);
	ScriptResult = 0;
	TestTrue(
		TEXT("Body-only package keeps the generated route live"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));
	TestEqual(TEXT("Body-only package executes the new WASM body"), ScriptResult, 37);

	const FString StructuralTypeManifestJson = BuildGeneratedTypeSessionManifest(
		TEXT("type:generated-session-fixture-structural-change"));
	TestTrue(
		TEXT("Structural candidate type manifest writes"),
		SaveUtf8Fixture(
			TypeManifestPath,
			StructuralTypeManifestJson,
			TypeManifestBytes));
	const FString StructuralTypeManifestSha256 =
		FAvidScriptHash::Sha256Hex(TypeManifestBytes);
	TestTrue(
		TEXT("Structural package descriptor writes"),
		SaveUtf8Fixture(
			DescriptorPath,
			BuildPackageDescriptorFixture(
				StructuralTypeManifestSha256,
				BodyOnlyRuntimeManifestSha256),
			DescriptorBytes));
	FAvidScriptGeneratedTypePackageReloadResult StructuralResult;
	TestFalse(
		TEXT("Active generated package rejects a structural descriptor reload"),
		Host->ReloadPackageFromDescriptorFile(
			DescriptorPath,
			StructuralResult,
			Error));
	TestTrue(
		TEXT("Structural reload requests a native rebuild"),
		StructuralResult.Disposition
			== EAvidScriptGeneratedTypePackageReloadDisposition::NativeRebuildRequired);
	TestTrue(
		TEXT("Structural rejection preserves the live package"),
		StructuralResult.bRollbackPreservedLivePackage);
	TestTrue(
		TEXT("Structural rejection names the changed type identity"),
		StructuralResult.StructuralChangeReason.Contains(TEXT("type identity")));
	ScriptResult = 0;
	TestTrue(
		TEXT("Structural rejection preserves generated dispatch"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));
	TestEqual(TEXT("Structural rejection preserves the body-only runtime"), ScriptResult, 37);

	TestTrue(
		TEXT("Compatible type manifest is restored for rollback testing"),
		SaveUtf8Fixture(TypeManifestPath, TypeManifestJson, TypeManifestBytes));
	const TArray<uint8> RollbackCandidateModule = BuildGeneratedTypeSessionModule(41);
	TestTrue(
		TEXT("Rollback candidate WASM writes"),
		FFileHelper::SaveArrayToFile(RollbackCandidateModule, *WasmPath));
	const FString RollbackRuntimeManifestJson = BuildRuntimeManifestFixture(
		FAvidScriptHash::Sha256Hex(RollbackCandidateModule));
	TestTrue(
		TEXT("Rollback candidate Runtime manifest writes"),
		SaveUtf8Fixture(
			RuntimeManifestPath,
			RollbackRuntimeManifestJson,
			RuntimeManifestBytes));
	const FString RollbackRuntimeManifestSha256 =
		FAvidScriptHash::Sha256Hex(RuntimeManifestBytes);
	TestTrue(
		TEXT("Rollback candidate package descriptor writes"),
		SaveUtf8Fixture(
			DescriptorPath,
			BuildPackageDescriptorFixture(
				TypeManifestSha256,
				RollbackRuntimeManifestSha256),
			DescriptorBytes));

	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> SecondReceiver(
		NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	if (!TestTrue(
		TEXT("Runtime host activates a second generated receiver"),
		Host->BeginInstance(*SecondReceiver, 0, Error)))
	{
		AddError(Error);
		Host->EndInstance(*Receiver, Error);
		Host->ClearPackage(Error);
		return true;
	}
	Host->SetReloadFailureAfterInstanceCountForTesting(1);
	FAvidScriptGeneratedTypePackageReloadResult RollbackResult;
	TestFalse(
		TEXT("Multi-instance package transaction surfaces an injected candidate failure"),
		Host->ReloadPackageFromDescriptorFile(
			DescriptorPath,
			RollbackResult,
			Error));
	TestEqual(TEXT("Rollback transaction sees two Sessions"), RollbackResult.CandidateInstanceCount, 2);
	TestEqual(TEXT("Rollback transaction commits one candidate before failure"), RollbackResult.ReloadedInstanceCount, 1);
	TestEqual(TEXT("Rollback transaction restores one committed candidate"), RollbackResult.RolledBackInstanceCount, 1);
	TestTrue(
		TEXT("Rollback transaction preserves the previous live package"),
		RollbackResult.bRollbackPreservedLivePackage);
	for (UObject* const ActiveReceiver : {
		static_cast<UObject*>(Receiver.Get()),
		static_cast<UObject*>(SecondReceiver.Get()) })
	{
		ScriptResult = 0;
		TestTrue(
			TEXT("Rolled-back generated receiver remains dispatchable"),
			FAvidScriptGeneratedTypeDispatcher::Invoke(
				ActiveReceiver,
				0,
				0,
				TConstArrayView<FAvidScriptGeneratedCallArgument>(),
				&ScriptResult));
		TestEqual(TEXT("Rolled-back generated receiver keeps body version 37"), ScriptResult, 37);
	}
	TestTrue(
		TEXT("Runtime host tears down the second reloaded receiver"),
		Host->EndInstance(*SecondReceiver, Error));

	TestTrue(
		TEXT("Runtime host tears down Session, route and ObjectHandle"),
		Host->EndInstance(*Receiver, Error));
	TestFalse(TEXT("Ended receiver is no longer active"), Host->IsInstanceActive(*Receiver));
	TestEqual(TEXT("Runtime host releases the Session"), Host->GetActiveInstanceCount(), 0);
	TestEqual(TEXT("Runtime host releases the ObjectHandle"), Host->GetRegisteredHandleCount(), 0);
	TestTrue(
		TEXT("Repeated native Super-chain teardown is idempotent"),
		Host->EndInstance(*Receiver, Error));
	TestTrue(TEXT("Inactive package can be cleared"), Host->ClearPackage(Error));
	return true;
}

#endif
