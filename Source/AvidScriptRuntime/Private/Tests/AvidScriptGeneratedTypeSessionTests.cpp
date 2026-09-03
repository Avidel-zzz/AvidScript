#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "AvidScriptHash.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptVmArtifact.h"
#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

TArray<uint8> BuildGeneratedTypeSessionModule(
	const int32 ReturnConstant = INDEX_NONE,
	const bool bTrapGeneratedExport = false)
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
	const TArray<uint8> CodeSection = bTrapGeneratedExport
		? TArray<uint8>{
			0x02,
			0x02, 0x00, 0x0b,
			0x03, 0x00, 0x00, 0x0b
		}
		: ReturnConstant == INDEX_NONE
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
  "schema_version": 6,
  "generator_version": "1.8",
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
	const FString& RuntimeManifestSha256,
	const TCHAR* ExecutionBackend = TEXT("wasmtime_jit"))
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
  "execution_backend": "%s",
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
		ExecutionBackend,
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

	FAvidScriptRuntimeSession TrapSession;
	TestTrue(
		TEXT("Trap Session owns the generated receiver route"),
		TrapSession.ConfigureGeneratedTypeInstance(
			*Receiver,
			ReceiverHandle,
			0,
			Registry,
			Error));
	TrapSession.SetBackendSelectionForTesting(Selection);
	const TArray<uint8> TrapModule =
		BuildGeneratedTypeSessionModule(INDEX_NONE, true);
	FAvidScriptWasmReloadResult TrapLoadResult;
	TestTrue(
		TEXT("Generated trap fixture loads"),
		TrapSession.LoadInitialModule(
			TrapModule.GetData(),
			TrapModule.Num(),
			Manifest,
			TrapLoadResult));
	TestFalse(
		TEXT("Generated prepared trap fails closed"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));
	const FAvidScriptRuntimeSessionSnapshot TrapSnapshot =
		TrapSession.GetSnapshot();
	TestTrue(TEXT("Generated trap quarantines its Session"), TrapSnapshot.bFaultQuarantined);
	TestFalse(TEXT("Generated trap unloads its Runtime"), TrapSnapshot.bHasActiveRuntime);
	TestEqual(
		TEXT("Generated trap keeps its root category"),
		TrapSnapshot.FaultCategory,
		FString(TEXT("guest_trap")));
	TestEqual(
		TEXT("Generated trap records its export"),
		TrapSnapshot.FaultExportName,
		FString(GeneratedExportName));
	TestEqual(TEXT("Generated trap records one root fault"), TrapSnapshot.FaultCount, 1);
	TestFalse(
		TEXT("Quarantined generated route rejects repeated entry"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(
			Receiver.Get(),
			0,
			0,
			TConstArrayView<FAvidScriptGeneratedCallArgument>(),
			&ScriptResult));
	TestEqual(
		TEXT("Repeated generated entry is counted"),
		TrapSession.GetSnapshot().FaultedEntryRejectCount,
		1);
	TestTrue(
		TEXT("Trap generated receiver route tears down explicitly"),
		TrapSession.ClearGeneratedTypeInstance(Error));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedTypeRuntimeHostPrecompiledTest,
	"AvidScript.Runtime.GeneratedTypes.RuntimeHostPrecompiled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedTypeRuntimeHostPrecompiledTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const TArray<uint8> Module = BuildGeneratedTypeSessionModule(37);
	FAvidScriptVmArtifactCompileRequest CompileRequest;
	CompileRequest.Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	CompileRequest.Selection.ExecutionMode = EAvidScriptVmExecutionMode::Aot;
	CompileRequest.Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	CompileRequest.CanonicalWasmBytes = Module;
	FAvidScriptVmArtifactCompileResult CompileResult;
	if (!TestTrue(TEXT("Generated type fixture precompiles"),
		CompileAvidScriptVmArtifact(CompileRequest, CompileResult)))
	{
		AddError(CompileResult.Error.Category + TEXT(": ") + CompileResult.Error.Details);
		return false;
	}
	const FAvidScriptVmOwnedArtifact& Compiled = CompileResult.Artifact;
	const FString PackageRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("AvidScriptTests/GeneratedTypePrecompiled"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestTrue(TEXT("Precompiled package fixture directory is created"),
		IFileManager::Get().MakeDirectory(*PackageRoot, true)))
	{
		return false;
	}
	TUniquePtr<FAvidScriptGeneratedTypeRuntimeHost> Host =
		FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
	ON_SCOPE_EXIT
	{
		Host->Shutdown();
		IFileManager::Get().DeleteDirectory(*PackageRoot, false, true);
	};
	const FString WasmPath = FPaths::Combine(PackageRoot, TEXT("generated-types.wasm"));
	const FString ExecutionFile = TEXT("generated-types.cwasm");
	const FString ExecutionPath = FPaths::Combine(PackageRoot, ExecutionFile);
	const FString TypeManifestPath = FPaths::Combine(PackageRoot, TEXT("generated-types.json"));
	const FString RuntimeManifestPath = FPaths::Combine(PackageRoot, TEXT("generated-types.avidscript.json"));
	const FString DescriptorPath = FPaths::Combine(PackageRoot, TEXT("package.json"));
	TArray<uint8> TypeManifestBytes;
	if (!TestTrue(TEXT("Precompiled fixture canonical WASM writes"),
		FFileHelper::SaveArrayToFile(Module, *WasmPath))
		|| !TestTrue(TEXT("Precompiled fixture execution bytes write"),
			FFileHelper::SaveArrayToFile(Compiled.ExecutionBytes, *ExecutionPath))
		|| !TestTrue(TEXT("Precompiled fixture type manifest writes"),
			SaveUtf8Fixture(TypeManifestPath, BuildGeneratedTypeSessionManifest(), TypeManifestBytes)))
	{
		return false;
	}
	const FString TypeManifestSha256 = FAvidScriptHash::Sha256Hex(TypeManifestBytes);
	const TSharedRef<FJsonObject> Execution = MakeShared<FJsonObject>();
	Execution->SetStringField(TEXT("format"), TEXT("wasmtime_serialized_v1"));
	Execution->SetStringField(TEXT("file"), ExecutionFile);
	Execution->SetStringField(TEXT("sha256"), Compiled.ExecutionIdentity);
	Execution->SetStringField(TEXT("canonical_sha256"), Compiled.CanonicalWasmIdentity);
	Execution->SetStringField(TEXT("compiler_build_identity"), Compiled.CompilerBuildIdentity);
	Execution->SetStringField(TEXT("target_triple"), Compiled.TargetTriple);
	Execution->SetStringField(TEXT("attestation_id"), Compiled.AttestationId);
	Execution->SetStringField(TEXT("policy"), TEXT("require_precompiled"));
	Execution->SetStringField(TEXT("fallback"), TEXT("wasmtime_jit"));
	auto WritePackage = [&](const TSharedPtr<FJsonObject>& ExecutionObject,
		const TCHAR* Backend = TEXT("wasmtime_precompiled"))
	{
		TSharedPtr<FJsonObject> RuntimeManifest;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(
			BuildRuntimeManifestFixture(FAvidScriptHash::Sha256Hex(Module))), RuntimeManifest))
		{
			AddError(TEXT("Runtime manifest fixture could not be parsed"));
			return false;
		}
		if (ExecutionObject.IsValid())
		{
			RuntimeManifest->SetObjectField(TEXT("execution"), ExecutionObject);
		}
		FString RuntimeJson;
		TArray<uint8> RuntimeBytes;
		TArray<uint8> DescriptorBytes;
		return TestTrue(TEXT("Runtime manifest fixture serializes"),
			FJsonSerializer::Serialize(RuntimeManifest.ToSharedRef(), TJsonWriterFactory<>::Create(&RuntimeJson)))
			&& TestTrue(TEXT("Runtime manifest fixture writes"),
				SaveUtf8Fixture(RuntimeManifestPath, RuntimeJson, RuntimeBytes))
			&& TestTrue(TEXT("Package descriptor fixture writes with matching hashes"),
				SaveUtf8Fixture(DescriptorPath,
					BuildPackageDescriptorFixture(TypeManifestSha256, FAvidScriptHash::Sha256Hex(RuntimeBytes), Backend),
					DescriptorBytes));
	};
	FString Error;
	if (!WritePackage(Execution)
		|| !TestTrue(TEXT("Schema1 installs a verified required precompiled artifact"),
			Host->InstallPackageFromDescriptorFile(DescriptorPath, Error)))
	{
		AddError(Error);
		return false;
	}
	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Receiver(
		NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	if (!TestTrue(TEXT("Precompiled generated Session activates"), Host->BeginInstance(*Receiver, 0, Error)))
	{
		AddError(Error);
		return false;
	}
	int32 ScriptResult = 0;
	TestTrue(TEXT("Precompiled generated export dispatches"), FAvidScriptGeneratedTypeDispatcher::Invoke(
		Receiver.Get(), 0, 0, TConstArrayView<FAvidScriptGeneratedCallArgument>(), &ScriptResult));
	TestEqual(TEXT("Precompiled generated body executes"), ScriptResult, 37);
	TestTrue(TEXT("Precompiled Session tears down"), Host->EndInstance(*Receiver, Error));
	TestTrue(TEXT("Precompiled package clears"), Host->ClearPackage(Error));

	auto RejectPackage = [&](const TCHAR* Label, const TCHAR* ExpectedError)
	{
		TestFalse(Label, Host->InstallPackageFromDescriptorFile(DescriptorPath, Error));
		TestTrue(TEXT("Rejected package keeps an explicit diagnostic"), Error.Contains(ExpectedError));
		TestEqual(TEXT("Rejected package has no active instance"), Host->GetActiveInstanceCount(), 0);
		TestFalse(TEXT("Rejected package cannot activate a receiver"), Host->BeginInstance(*Receiver, 0, Error));
	};
	if (!WritePackage(nullptr)) { return false; }
	RejectPackage(TEXT("Canonical JIT cannot masquerade as precompiled"), TEXT("requires a verified AOT artifact"));
	if (!WritePackage(Execution, TEXT("wasmtime_jit"))) { return false; }
	RejectPackage(TEXT("JIT descriptor cannot downgrade an AOT policy"), TEXT("cannot override an execution artifact policy"));

	Execution->SetStringField(TEXT("policy"), TEXT("prefer_precompiled"));
	if (!WritePackage(Execution)) { return false; }
	RejectPackage(TEXT("Precompiled descriptor requires a no-fallback policy"), TEXT("require_precompiled"));
	Execution->SetStringField(TEXT("file"), TEXT("missing.cwasm"));
	if (!WritePackage(Execution)) { return false; }
	RejectPackage(TEXT("Loader JIT fallback cannot satisfy a precompiled descriptor"), TEXT("no JIT fallback"));
	Execution->SetStringField(TEXT("policy"), TEXT("require_precompiled"));
	if (!WritePackage(Execution)) { return false; }
	RejectPackage(TEXT("Required missing artifact is rejected"), TEXT("execution_file_missing"));
	Execution->SetStringField(TEXT("file"), ExecutionFile);

	Execution->SetStringField(TEXT("target_triple"), TEXT("foreign-target"));
	if (!WritePackage(Execution)) { return false; }
	RejectPackage(TEXT("Precompiled target mismatch is rejected"), TEXT("execution_target_mismatch"));
	Execution->SetStringField(TEXT("target_triple"), Compiled.TargetTriple);
	Execution->SetStringField(TEXT("attestation_id"), FString::ChrN(32, TEXT('0')));
	if (!WritePackage(Execution)) { return false; }
	RejectPackage(TEXT("Precompiled expired attestation is rejected"), TEXT("execution_attestation_invalid"));
	Execution->SetStringField(TEXT("attestation_id"), Compiled.AttestationId);
	Execution->SetStringField(TEXT("sha256"), FString::ChrN(64, TEXT('0')));
	if (!WritePackage(Execution)) { return false; }
	RejectPackage(TEXT("Precompiled execution hash mismatch is rejected"), TEXT("execution_identity_mismatch"));

	TestTrue(TEXT("Disguised canonical bytes write with cwasm extension"),
		FFileHelper::SaveArrayToFile(Module, *ExecutionPath));
	Execution->SetStringField(TEXT("sha256"), FAvidScriptHash::Sha256Hex(Module));
	if (!WritePackage(Execution)) { return false; }
	RejectPackage(TEXT("Renamed JIT bytes cannot reuse an AOT attestation"), TEXT("execution_attestation_invalid"));
	return true;
}

#endif
