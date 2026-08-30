#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
constexpr TCHAR GeneratedExportName[] =
	TEXT("avid_ue_0123456789abcdef0123456789abcdef");

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

TArray<uint8> BuildGeneratedTypeSessionModule()
{
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
	const TArray<uint8> CodeSection = {
		0x02,
		0x02, 0x00, 0x0b,
		0x05, 0x00, 0x20, 0x00, 0xa7, 0x0b
	};
	AppendWasmSection(Module, 0x0a, CodeSection);
	return Module;
}

FString BuildGeneratedTypeSessionManifest()
{
	return FString::Printf(
		TEXT(R"JSON({
  "schema_version": 4,
  "generator_version": "1.3",
  "module_name": "AvidScriptRuntime",
  "types": [
    {
      "type_ordinal": 0,
      "stable_type_id": "type:generated-session-fixture",
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
		*UAvidScriptGeneratedTypeSessionTestObject::StaticClass()->GetPathName(),
		GeneratedExportName);
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
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FString Error;
	if (!TestTrue(
		TEXT("Generated type registry builds for Runtime host"),
		FAvidScriptGeneratedTypeRegistry::BuildFromJson(
			BuildGeneratedTypeSessionManifest(),
			Registry,
			Error)))
	{
		AddError(Error);
		return true;
	}

	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	FAvidScriptWasmReloadManifest Manifest;
	Manifest.ModuleId = TEXT("generated_type_runtime_host");
	Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
	Manifest.Language = TEXT("wasm");
	Manifest.RequiredExports = { TEXT("avid_on_begin_play") };
	const TArray<uint8> Module = BuildGeneratedTypeSessionModule();
	const FAvidScriptRuntimeArtifact Artifact =
		FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Module, Selection);
	FAvidScriptGeneratedTypeRuntimeHost& Host =
		FAvidScriptGeneratedTypeRuntimeHost::Get();
	if (!TestTrue(
		TEXT("Runtime host installs one immutable generated package"),
		Host.InstallPackage(Registry, Artifact, Error)))
	{
		AddError(Error);
		return true;
	}

	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Receiver(
		NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	if (!TestTrue(
		TEXT("Runtime host creates ObjectHandle, Session and router registration"),
		Host.BeginInstance(*Receiver, 0, Error)))
	{
		AddError(Error);
		Host.ClearPackage(Error);
		return true;
	}
	TestTrue(TEXT("Runtime host records the active receiver"), Host.IsInstanceActive(*Receiver));
	TestEqual(TEXT("Runtime host owns one Session"), Host.GetActiveInstanceCount(), 1);
	TestEqual(TEXT("Runtime host owns one anchored ObjectHandle"), Host.GetRegisteredHandleCount(), 1);

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

	TestTrue(
		TEXT("Runtime host tears down Session, route and ObjectHandle"),
		Host.EndInstance(*Receiver, Error));
	TestFalse(TEXT("Ended receiver is no longer active"), Host.IsInstanceActive(*Receiver));
	TestEqual(TEXT("Runtime host releases the Session"), Host.GetActiveInstanceCount(), 0);
	TestEqual(TEXT("Runtime host releases the ObjectHandle"), Host.GetRegisteredHandleCount(), 0);
	TestTrue(TEXT("Inactive package can be cleared"), Host.ClearPackage(Error));
	return true;
}

#endif
