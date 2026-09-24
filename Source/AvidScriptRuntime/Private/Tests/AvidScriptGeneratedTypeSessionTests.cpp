#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "AvidScriptHash.h"
#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "AvidScriptVmArtifact.h"
#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr TCHAR GeneratedExportName[] =
	TEXT("avid_ue_0123456789abcdef0123456789abcdef");
constexpr TCHAR GeneratedTypeGenerationKey[] =
	TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

class FGeneratedHostMutationProbe final : public IAvidScriptGeneratedTypeInstance
{
public:
	uint64 GetGeneratedExecutionGeneration() const override { return 1; }
	bool InvokeGeneratedTypeMember(UObject&, const FAvidScriptObjectHandle&, uint32, uint32,
		TConstArrayView<FAvidScriptGeneratedCallArgument>, void*) override
	{
		if (Callback) Callback();
		return true;
	}
	TFunction<void()> Callback;
};

void AppendWasmSection(
	TArray<uint8>& Module,
	const uint8 SectionId,
	const TConstArrayView<uint8> Payload)
{
	Module.Add(SectionId);
	uint32 Size = static_cast<uint32>(Payload.Num());
	do
	{
		const uint8 Byte = static_cast<uint8>(Size & 0x7f);
		Size >>= 7;
		Module.Add(Byte | (Size ? 0x80 : 0));
	} while (Size);
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
	const bool bTrapGeneratedExport = false, const bool bIncrementGlobal = false, const bool bTrapSecondBegin = false)
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
	if (bTrapSecondBegin) AppendWasmSection(Module, 6, {2, 0x7f, 1, 0x41, 0, 0x0b, 0x7f, 1, 0x41, 0, 0x0b});
	else if (bIncrementGlobal) AppendWasmSection(Module, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});

	TArray<uint8> ExportSection = { 0x02 };
	AppendWasmExport(ExportSection, "avid_on_begin_play", 0);
	AppendWasmExport(
		ExportSection,
		"avid_ue_0123456789abcdef0123456789abcdef",
		1);
	AppendWasmSection(Module, 0x07, ExportSection);
	const TArray<uint8> CodeSection = bTrapSecondBegin
		? TArray<uint8>{2, 18, 0, 0x23, 1, 0x41, 1, 0x6a, 0x24, 1, 0x23, 1, 0x41, 2, 0x46, 0x04, 0x40, 0, 0x0b, 0x0b,
			11, 0, 0x23, 0, 0x41, 1, 0x6a, 0x24, 0, 0x23, 0, 0x0b}
		: bIncrementGlobal
		? TArray<uint8>{2, 2, 0, 0x0b, 11, 0, 0x23, 0, 0x41, 1, 0x6a, 0x24, 0, 0x23, 0, 0x0b}
		: bTrapGeneratedExport
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
	FAvidScriptGeneratedCSharpReceiverTest,
	"AvidScript.Runtime.GeneratedTypes.CSharpReceiver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpReceiverTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		for (const FString File : {TEXT("csharp-ue-receiver"), TEXT("csharp-ue-receiver-stress"), TEXT("csharp-ue-receiver-null"), TEXT("csharp-ue-receiver-plain-ref")})
		{
			const bool bNull = File.EndsWith(TEXT("-null"));
			AddInfo(FString::Printf(TEXT("CSharp UE receiver backend=%d fixture=%s"), static_cast<int32>(Backend), *File));
			TArray<uint8> Wasm;
			FString MetadataText;
			FString MetadataName = File;
			MetadataName.RemoveFromEnd(TEXT("-stress"));
			if (!TestTrue(TEXT("Read current CSharp WASM"), FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory, File + TEXT(".wasm"))))) return false;
			if (!TestTrue(TEXT("Read compiler receiver metadata"), FFileHelper::LoadFileToString(MetadataText,
				*FPaths::Combine(Directory, MetadataName + TEXT(".json"))))) return false;
			TSharedPtr<FJsonObject> Metadata;
			if (!TestTrue(TEXT("Parse compiler metadata"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MetadataText), Metadata)) || !Metadata) return false;
			TSharedPtr<FJsonObject> RegistryJson;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BuildGeneratedTypeSessionManifest()), RegistryJson) || !RegistryJson) return false;
			// Bind the compiled C# entry to the existing reflected native test class.
			const TSharedPtr<FJsonObject> TypeJson = RegistryJson->GetArrayField(TEXT("types"))[0]->AsObject();
			TypeJson->SetStringField(TEXT("stable_type_id"), Metadata->GetStringField(TEXT("type_id")));
			const TSharedPtr<FJsonObject> FunctionJson = TypeJson->GetArrayField(TEXT("functions"))[0]->AsObject();
			FunctionJson->SetStringField(TEXT("stable_member_id"), Metadata->GetStringField(TEXT("method_id")));
			FunctionJson->SetStringField(TEXT("export_name"), Metadata->GetStringField(TEXT("export_name")));
			FString RegistryText;
			if (!FJsonSerializer::Serialize(RegistryJson.ToSharedRef(), TJsonWriterFactory<>::Create(&RegistryText))) return false;
			TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types;
			FString Error;
			if (!TestTrue(TEXT("Registry accepts the compiled entry ABI"), FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, Types, Error))) { AddError(Error); return false; }
			FAvidScriptObjectRegistry Objects;
			TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Receiver(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
			FAvidScriptObjectHandleResult HandleResult;
			const auto Handle = Objects.RegisterObject(Receiver.Get(), HandleResult, false);
			if (!TestTrue(TEXT("Owner handle registers"), Handle.IsValid())) return false;
			FAvidScriptRuntimeSession Session;
			FAvidScriptWasmHostContext Context;
			Context.ObjectRegistry = &Objects;
			Context.OwnerHandle = Handle;
			Session.SetHostContext(Context);
			if (!TestTrue(TEXT("Configure compiled CSharp owner"), Session.ConfigureGeneratedTypeInstance(*Receiver, Handle, 0, Types, Error))) { AddError(Error); return false; }
			FAvidScriptVmBackendSelection Selection;
			Selection.BackendKind = Backend;
			Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
			Session.SetBackendSelectionForTesting(Selection);
			FAvidScriptWasmReloadManifest Manifest;
			Manifest.ModuleId = TEXT("csharp_ue_receiver");
			Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
			Manifest.Language = TEXT("csharp");
			Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
			for (const auto& Import : Metadata->GetArrayField(TEXT("imports")))
				Manifest.RequiredImports.Add({Import->AsObject()->GetStringField(TEXT("module")), Import->AsObject()->GetStringField(TEXT("name"))});
			FAvidScriptWasmReloadResult Load;
			if (!TestTrue(TEXT("Load compiler-produced receiver module"), Session.LoadInitialModule(Wasm.GetData(), Wasm.Num(), Manifest, Load))) { AddError(Load.ErrorMessage); return false; }
			int32 Result = -1;
			const bool bCalled = FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver.Get(), 0, 0, {}, &Result);
			if (!TestEqual(TEXT("Receiver execution or null binding rejection"), bCalled, !bNull)) return false;
			if (bNull)
			{
				TestTrue(TEXT("Null receiver binding quarantines the Session"), Session.GetSnapshot().bFaultQuarantined);
				TestFalse(TEXT("Faulted module is unloaded"), Session.GetSnapshot().bHasActiveRuntime);
			}
			else
			{
				TestEqual(TEXT("UE delegates and captures match .NET result"), Result, 511);
				auto* Runtime = Session.GetLiveRuntimeForTesting();
				if (!TestNotNull(TEXT("Receiver runtime remains live"), Runtime)) return false;
				auto* Heap = Runtime->GetManagedHeapForTesting();
				if (!TestNotNull(TEXT("Receiver contexts use managed heap"), Heap)) return false;
				if (File.EndsWith(TEXT("-plain-ref")))
					TestEqual(TEXT("Primitive references need no placeholder heap objects"), Heap->GetStats().Allocations, uint64(0));
				else TestTrue(TEXT("Repeated receiver bindings allocated contexts"), Heap->GetStats().Allocations >= 64);
				if (File.EndsWith(TEXT("-stress"))) TestTrue(TEXT("Receiver contexts survive collection at every allocation"), Heap->GetStats().Collections >= Heap->GetStats().Allocations);
				TestEqual(TEXT("Receiver invocation frames unwind"), Heap->GetStats().ActiveFrames, 0u);
				TestEqual(TEXT("Receiver invocation roots unwind"), Heap->GetStats().LiveRoots, 0u);
				TestTrue(TEXT("Detached receiver contexts collect"), Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
				TestEqual(TEXT("Receiver boxes and captured environments released"), Heap->GetStats().LiveObjects, 0u);
				FAvidScriptWasmReloadResult Reload;
				if (!TestTrue(TEXT("CSharp receiver module body reloads"), Session.ReloadModule(Wasm.GetData(), Wasm.Num(), Manifest, Reload))) { AddError(Reload.ErrorMessage); return false; }
				TestTrue(TEXT("Reloaded receiver executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver.Get(), 0, 0, {}, &Result));
				TestEqual(TEXT("Reloaded result preserves delegate semantics"), Result, 511);
				TestTrue(TEXT("Retire owner handle"), Objects.ReleaseHandle(Handle, HandleResult, false));
				TestFalse(TEXT("Compiled receiver cannot execute after owner retirement"), FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver.Get(), 0, 0, {}, &Result));
			}
			FAvidScriptWasmSmokeResult Stop;
			Session.StopAndUnload(Stop);
			TestTrue(TEXT("Compiled receiver route releases"), Session.ClearGeneratedTypeInstance(Error));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedReceiverAuthorityTest,
	"AvidScript.Runtime.GeneratedTypes.ReceiverAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedCSharpSharedBindingsTest,
	"AvidScript.Runtime.GeneratedTypes.CSharpSharedBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpSharedBindingsTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const bool bStress : {false, true})
	{
		AddInfo(FString::Printf(TEXT("CSharp shared bindings backend=%d stress=%d"), static_cast<int32>(Backend), bStress));
		TArray<uint8> Wasm;
		FString MetadataText;
		if (!FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory,
			bStress ? TEXT("csharp-ue-shared-bindings-stress.wasm") : TEXT("csharp-ue-shared-bindings.wasm")))
			|| !FFileHelper::LoadFileToString(MetadataText, *FPaths::Combine(Directory, TEXT("csharp-ue-shared-bindings.json")))) return false;
		TSharedPtr<FJsonObject> Metadata, RegistryJson;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MetadataText), Metadata)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BuildGeneratedTypeSessionManifest()), RegistryJson)) return false;
		const auto TypeJson = RegistryJson->GetArrayField(TEXT("types"))[0]->AsObject();
		TypeJson->SetStringField(TEXT("stable_type_id"), Metadata->GetStringField(TEXT("type_id")));
		TypeJson->SetArrayField(TEXT("properties"), Metadata->GetArrayField(TEXT("properties")));
		const auto FunctionJson = TypeJson->GetArrayField(TEXT("functions"))[0]->AsObject();
		FunctionJson->SetNumberField(TEXT("member_ordinal"), Metadata->GetNumberField(TEXT("member_ordinal")));
		FunctionJson->SetStringField(TEXT("stable_member_id"), Metadata->GetStringField(TEXT("method_id")));
		FunctionJson->SetStringField(TEXT("export_name"), Metadata->GetStringField(TEXT("export_name")));
		FString RegistryText, Error;
		if (!FJsonSerializer::Serialize(RegistryJson.ToSharedRef(), TJsonWriterFactory<>::Create(&RegistryText))) return false;
		TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types, OtherTypes;
		if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, Types, Error)
			|| !FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, OtherTypes, Error)) { AddError(Error); return false; }
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmReloadManifest Manifest;
		Manifest.ModuleId = TEXT("csharp_shared_bindings"); Manifest.Language = TEXT("csharp");
		Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
		Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
		for (const auto& Import : Metadata->GetArrayField(TEXT("imports")))
			Manifest.RequiredImports.Add({Import->AsObject()->GetStringField(TEXT("module")), Import->AsObject()->GetStringField(TEXT("name"))});
		FAvidScriptObjectRegistry Objects;
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>()),
			B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>()), C(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		A->Value = 10; B->Value = 20; C->Value = 1000;
		FAvidScriptRuntimeSession SessionA, SessionB, SessionC;
		auto Load = [&](FAvidScriptRuntimeSession& Session, UObject& Receiver, const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry)
		{
			FAvidScriptObjectHandleResult HandleResult;
			FAvidScriptWasmHostContext Context;
			Context.ObjectRegistry = &Objects;
			Context.OwnerHandle = Objects.RegisterObject(&Receiver, HandleResult, false);
			Session.SetHostContext(Context);
			Session.SetBackendSelectionForTesting(Selection);
			if (!Session.ConfigureGeneratedTypeInstance(Receiver, Context.OwnerHandle, 0, Registry, Error)) { AddError(Error); return false; }
			FAvidScriptWasmReloadResult Result;
			if (!Session.LoadInitialModule(Wasm.GetData(), Wasm.Num(), Manifest, Result)) { AddError(Result.ErrorMessage); return false; }
			return true;
		};
		if (!Load(SessionA, *A, Types) || !Load(SessionB, *B, Types) || !Load(SessionC, *C, OtherTypes)) return false;
		const auto ContextA = SessionA.GetTestSnapshot().HostContext;
		auto ContextB = SessionB.GetTestSnapshot().HostContext;
		auto ContextC = SessionC.GetTestSnapshot().HostContext;
		auto* Runtime = SessionA.GetLiveRuntimeForTesting();
		// Independent-Session probe: a state from another VM must never be reused
		// even when the receiver ABI matches. Production Host coverage follows below.
		for (auto* Context : {&ContextB, &ContextC})
		{
			Context->InstanceExecutionState.Reset();
			if (!TestTrue(TEXT("prepare peer state in carrier Runtime"), Runtime->CreateInstanceExecutionState(
				*Context, Context->InstanceExecutionState, Error))) { AddError(Error); return false; }
			FAvidScriptWasmSmokeResult Begin;
			if (!TestTrue(TEXT("begin peer state in carrier Runtime"), Runtime->BeginPlayInContext(*Context, Begin)))
			{ AddError(Begin.ErrorMessage); return false; }
		}
		FAvidScriptContextualExportCall Entry;
		if (!TestTrue(TEXT("prepare compiled shared entry"), Runtime->PrepareContextualExportCall(Metadata->GetStringField(TEXT("export_name")), Entry, Error))) return false;
		auto Invoke = [&](const FAvidScriptWasmHostContext& Context, const FAvidScriptObjectHandle& Self, bool bExpected, int32 ExpectedValue)
		{
			FAvidScriptVmCallFrame Frame;
			Frame.CellCount = 2; Frame.Cells[0] = Self.Slot; Frame.Cells[1] = Self.Generation;
			FAvidScriptVmError Failure;
			FAvidScriptVmCallResult Result;
			const bool bCalled = Runtime->InvokeInContext(Entry, Context, Frame, Failure, &Result);
			TestEqual(*FString::Printf(TEXT("compiled receiver authority: %s"), *Failure.Details), bCalled, bExpected);
			if (bExpected) TestEqual(TEXT("shared static state and selected UObject property match .NET"), static_cast<int32>(Result.Cells[0]), ExpectedValue);
			else TestFalse(TEXT("rejection has a diagnostic"), Failure.Category.IsEmpty());
		};
		Invoke(ContextA, ContextA.OwnerHandle, true, 1111);
		Invoke(ContextB, ContextB.OwnerHandle, true, 2222);
		Invoke(ContextA, ContextA.OwnerHandle, true, 1414);
		TestEqual(TEXT("A property is independent"), A->Value, 14);
		TestEqual(TEXT("B property is independent"), B->Value, 22);
		Invoke(ContextB, ContextA.OwnerHandle, false, 0);
		Invoke(ContextC, ContextC.OwnerHandle, false, 0);
		TestEqual(TEXT("same-class foreign registry snapshot cannot authorize this package"), C->Value, 1000);
		TestTrue(TEXT("retire the original generated registration while Runtime remains loaded"), SessionA.ClearGeneratedTypeInstance(Error));
		TestFalse(TEXT("retained original context does not retain its authority"), ContextA.GeneratedTypeAuthority.IsValid());
		Invoke(ContextA, ContextA.OwnerHandle, false, 0);
		Invoke(ContextB, ContextB.OwnerHandle, true, 2626);
		TestEqual(TEXT("Runtime-owned property bindings survive original registration teardown"), B->Value, 26);
		FAvidScriptWasmSmokeResult Stop;
		TestTrue(TEXT("peer Session stops"), SessionB.StopAndUnload(Stop));
		Invoke(ContextB, ContextB.OwnerHandle, false, 0);
		TestTrue(TEXT("peer registration clears"), SessionB.ClearGeneratedTypeInstance(Error));
		TestFalse(TEXT("retained peer context authority expires"), ContextB.GeneratedTypeAuthority.IsValid());
		Invoke(ContextB, ContextB.OwnerHandle, false, 0);
		TestTrue(TEXT("peer stop does not destroy the independently retained Runtime"), Runtime->IsLoaded());
		auto* Heap = Runtime->GetManagedHeapForTesting();
		TestEqual(TEXT("compiled captures leave no active frame"), Heap->GetStats().ActiveFrames, 0u);
		TestEqual(TEXT("compiled captures leave no roots"), Heap->GetStats().LiveRoots, 0u);
		if (bStress) TestTrue(TEXT("compiled receiver captures survived forced collection"), Heap->GetStats().Collections >= Heap->GetStats().Allocations);
		TestTrue(TEXT("shared Runtime detached captures collect"), Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
		TestEqual(TEXT("no detached captured receivers remain"), Heap->GetStats().LiveObjects, 0u);
		TestTrue(TEXT("carrier Session stops"), SessionA.StopAndUnload(Stop));
		TestTrue(TEXT("foreign Session stops"), SessionC.StopAndUnload(Stop));
		TestTrue(TEXT("foreign registration clears"), SessionC.ClearGeneratedTypeInstance(Error));

		auto Host = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
		ON_SCOPE_EXIT { Host->Shutdown(); };
		const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
		A->Value = 10; B->Value = 20; C->Value = 1000;
		if (!Host->InstallPackage(Types, Artifact, Error) || !Host->BeginInstance(*A, 0, Error)
			|| !Host->BeginInstance(*B, 0, Error)) { AddError(Error); return false; }
		auto* ProductionA = Host->GetInstanceSessionForTesting(*A);
		auto* ProductionB = Host->GetInstanceSessionForTesting(*B);
		TestTrue(TEXT("CSharp production owners share the same VM"), ProductionA->GetLiveRuntimeForTesting() == ProductionB->GetLiveRuntimeForTesting());
		auto CallProduction = [&](UObject* Owner, int32 Expected)
		{
			int32 Value = 0;
			TestTrue(TEXT("CSharp production dispatcher executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Owner, 0,
				static_cast<uint32>(Metadata->GetNumberField(TEXT("member_ordinal"))), {}, &Value));
			TestEqual(TEXT("CSharp production static and owner state match .NET"), Value, Expected);
		};
		CallProduction(A.Get(), 1111); CallProduction(B.Get(), 2222); CallProduction(A.Get(), 1414);
		TestTrue(TEXT("CSharp first production owner exits"), Host->EndInstance(*A, Error));
		CallProduction(B.Get(), 2626);
		FAvidScriptGeneratedTypePackageReloadResult ProductionReload;
		if (!Host->ReloadPackage(OtherTypes, Artifact, ProductionReload, Error)
			|| !Host->BeginInstance(*C, 0, Error)) { AddError(Error); return false; }
		TestTrue(TEXT("CSharp post-reload owner shares the canonical bindings"), ProductionB->GetLiveRuntimeForTesting()
			== Host->GetInstanceSessionForTesting(*C)->GetLiveRuntimeForTesting());
		CallProduction(B.Get(), 2727); CallProduction(C.Get(), 101202);
		auto* ProductionHeap = ProductionB->GetLiveRuntimeForTesting()->GetManagedHeapForTesting();
		TestEqual(TEXT("production invocation frames unwind"), ProductionHeap->GetStats().ActiveFrames, 0u);
		TestEqual(TEXT("production temporary roots unwind"), ProductionHeap->GetStats().LiveRoots, 0u);
		if (bStress) TestTrue(TEXT("production captures survive forced GC"), ProductionHeap->GetStats().Collections >= ProductionHeap->GetStats().Allocations);
	}
	return true;
}

bool FAvidScriptGeneratedReceiverAuthorityTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	// The fixture enters the actual supplemental import through each VM backend.
	// Its generated export has the same packed receiver signature as C# UClass methods.
	auto BuildModule = [](const bool bForeignHandle, const bool bUnknownOrdinal, const bool bWrongSignature)
	{
		TArray<uint8> Module = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
		const TArray<uint8> Types = {3, 0x60, 0, 0, 0x60, 1, 0x7e, 1, static_cast<uint8>(bWrongSignature ? 0x7e : 0x7f), 0x60, 1, 0x7e, 1, 0x7f};
		AppendWasmSection(Module, 1, Types);
		TArray<uint8> Imports = {1};
		auto Name = [&Imports](const ANSICHAR* Text)
		{
			const int32 Length = FCStringAnsi::Strlen(Text);
			Imports.Add(static_cast<uint8>(Length));
			Imports.Append(reinterpret_cast<const uint8*>(Text), Length);
		};
		Name("avidscript");
		Name(bUnknownOrdinal ? "avid_ue_receiver_1_require_v1" : "avid_ue_receiver_0_require_v1");
		Imports.Append({0, 1});
		AppendWasmSection(Module, 2, Imports);
		const TArray<uint8> Functions = {2, 0, 2};
		AppendWasmSection(Module, 3, Functions);
		TArray<uint8> Exports = {2};
		AppendWasmExport(Exports, "avid_on_begin_play", 1);
		AppendWasmExport(Exports, "avid_ue_0123456789abcdef0123456789abcdef", 2);
		AppendWasmSection(Module, 7, Exports);
		TArray<uint8> Body = {0, 0x20, 0};
		if (bForeignHandle) Body.Append({0x42, 1, 0x7c}); // i64.add: next live registry slot, same generation
		Body.Append({0x10, 0});
		if (bWrongSignature) Body.Add(0xa7); // i32.wrap_i64 keeps the export signature valid
		Body.Add(0x0b);
		TArray<uint8> Code = {2, 2, 0, 0x0b, static_cast<uint8>(Body.Num())};
		Code.Append(Body);
		AppendWasmSection(Module, 10, Code);
		return Module;
	};
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types;
	FString Error;
	if (!TestTrue(TEXT("Receiver authority type plan"), FAvidScriptGeneratedTypeRegistry::BuildFromJson(
		BuildGeneratedTypeSessionManifest(), Types, Error))) { AddError(Error); return false; }
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		for (const FString Case : {TEXT("live"), TEXT("foreign"), TEXT("stale"), TEXT("registry-missing"),
			TEXT("registry-rebound"), TEXT("destroyed"), TEXT("unknown-type"), TEXT("wrong-signature"), TEXT("world-normalized"), TEXT("world-teardown")})
		{
			AddInfo(FString::Printf(TEXT("Receiver authority backend=%d case=%s"), static_cast<int32>(Backend), *Case));
			FAvidScriptObjectRegistry Objects;
			FAvidScriptObjectRegistry ReboundObjects;
			UWorld* World = Case.StartsWith(TEXT("world-")) ? UWorld::CreateWorld(EWorldType::Game, false) : nullptr;
			UWorld* OtherWorld = Case == TEXT("world-normalized") ? UWorld::CreateWorld(EWorldType::Game, false) : nullptr;
			ON_SCOPE_EXIT { if (OtherWorld) OtherWorld->DestroyWorld(false); if (World) World->DestroyWorld(false); };
			if (Case.StartsWith(TEXT("world-")) && !TestNotNull(TEXT("Receiver World fixture"), World)) return false;
			if (Case == TEXT("world-normalized") && !TestNotNull(TEXT("Other World fixture"), OtherWorld)) return false;
			UObject* ReceiverOuter = World ? static_cast<UObject*>(World) : GetTransientPackage();
			TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Receiver(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(ReceiverOuter));
			TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Other(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(ReceiverOuter));
			FAvidScriptObjectHandleResult HandleResult;
			const auto Handle = Objects.RegisterObject(Receiver.Get(), HandleResult, false);
			const auto Foreign = Objects.RegisterObject(Other.Get(), HandleResult, false);
			if (!TestTrue(TEXT("Two live handles"), Handle.IsValid() && Foreign.IsValid())) return false;
			TestEqual(TEXT("Foreign fixture preserves generation"), Foreign.Generation, Handle.Generation);
			TestEqual(TEXT("Foreign fixture uses next slot"), Foreign.Slot, Handle.Slot + 1);
			FAvidScriptRuntimeSession Session;
			FAvidScriptWasmHostContext Context;
			Context.ObjectRegistry = Case == TEXT("registry-missing") ? nullptr : &Objects;
			Context.OwnerHandle = Handle;
			Context.World = OtherWorld ? OtherWorld : World;
			Session.SetHostContext(Context);
			if (!TestTrue(TEXT("Configure receiver authority"), Session.ConfigureGeneratedTypeInstance(*Receiver, Handle, 0, Types, Error)))
			{ AddError(Error); return false; }
			TestFalse(TEXT("Another receiver is not this Session"), Session.ValidateGeneratedTypeReceiver(static_cast<int64>(Foreign.ToUInt64()), 0));
			TestFalse(TEXT("Unknown type ordinal rejected"), Session.ValidateGeneratedTypeReceiver(static_cast<int64>(Handle.ToUInt64()), 1));
			TestFalse(TEXT("Forged generation rejected"), Session.ValidateGeneratedTypeReceiver(static_cast<int64>(Handle.ToUInt64() ^ (1ULL << 32)), 0));
			FAvidScriptVmBackendSelection Selection;
			Selection.BackendKind = Backend;
			Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
			Session.SetBackendSelectionForTesting(Selection);
			FAvidScriptWasmReloadManifest Manifest;
			Manifest.ModuleId = TEXT("generated_receiver_authority");
			Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
			Manifest.Language = TEXT("wasm");
			Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
			Manifest.RequiredImports = {{TEXT("avidscript"), Case == TEXT("unknown-type")
				? TEXT("avid_ue_receiver_1_require_v1") : TEXT("avid_ue_receiver_0_require_v1")}};
			const TArray<uint8> Module = BuildModule(Case == TEXT("foreign"), Case == TEXT("unknown-type"), Case == TEXT("wrong-signature"));
			FAvidScriptWasmReloadResult Load;
			const bool bLoaded = Session.LoadInitialModule(Module.GetData(), Module.Num(), Manifest, Load);
			if (Case == TEXT("unknown-type") || Case == TEXT("wrong-signature") || Case == TEXT("registry-missing"))
			{
				TestFalse(TEXT("Unknown or ABI-mismatched receiver import cannot load"), bLoaded);
				TestFalse(TEXT("Invalid import has a diagnostic"), Load.ErrorMessage.IsEmpty());
				TestTrue(TEXT("Rejected receiver route released"), Session.ClearGeneratedTypeInstance(Error));
				continue;
			}
			if (!TestTrue(TEXT("Load receiver guard WASM"), bLoaded)) { AddError(Load.ErrorMessage); return false; }
			if (Case == TEXT("stale")) TestTrue(TEXT("Retire receiver generation"), Objects.ReleaseHandle(Handle, HandleResult, false));
			if (Case == TEXT("destroyed")) Receiver->MarkAsGarbage();
			if (Case == TEXT("world-teardown")) World->bIsTearingDown = true;
			if (Case == TEXT("registry-rebound"))
			{
				const auto ReboundHandle = ReboundObjects.RegisterObject(Other.Get(), HandleResult, false);
				TestTrue(TEXT("Rebound registry reproduces numeric handle"), ReboundHandle == Handle);
				Context.ObjectRegistry = &ReboundObjects;
				Session.SetHostContext(Context);
				TestTrue(TEXT("live execution state rejects registry rebinding"), Session.GetTestSnapshot().HostContext.ObjectRegistry == &Objects);
				Context.ObjectRegistry = &Objects;
			}
			int32 Result = -1;
			const bool bCalled = FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver.Get(), 0, 0, {}, &Result);
			const bool bExpectedLive = Case == TEXT("live") || Case == TEXT("world-normalized") || Case == TEXT("registry-rebound");
			TestEqual(TEXT("Only live current authority executes"), bCalled, bExpectedLive);
			if (bExpectedLive)
			{
				TestEqual(TEXT("Host guard succeeds"), Result, 1);
				FAvidScriptRuntimeSession Peer;
				FAvidScriptWasmHostContext PeerContext = Context;
				PeerContext.OwnerHandle = Foreign;
				Peer.SetHostContext(PeerContext);
				TestTrue(TEXT("Peer owns a distinct receiver"), Peer.ConfigureGeneratedTypeInstance(*Other, Foreign, 0, Types, Error));
				Peer.SetBackendSelectionForTesting(Selection);
				FAvidScriptWasmReloadResult PeerLoad;
				TestTrue(TEXT("Two Sessions share only the native ABI stub"), Peer.LoadInitialModule(Module.GetData(), Module.Num(), Manifest, PeerLoad));
				int32 PeerResult = -1;
				TestTrue(TEXT("Peer dispatch uses its own context"), FAvidScriptGeneratedTypeDispatcher::Invoke(Other.Get(), 0, 0, {}, &PeerResult));
				TestEqual(TEXT("Peer guard result"), PeerResult, 1);
				FAvidScriptWasmReloadResult Reload;
				TestTrue(TEXT("Guard authority survives body reload"), Session.ReloadModule(Module.GetData(), Module.Num(), Manifest, Reload));
				Result = -1;
				TestTrue(TEXT("Reloaded guard executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver.Get(), 0, 0, {}, &Result));
				TestEqual(TEXT("Reloaded guard retains identity"), Result, 1);
				FAvidScriptWasmSmokeResult PeerStop;
				Peer.StopAndUnload(PeerStop);
				TestTrue(TEXT("Peer route released independently"), Peer.ClearGeneratedTypeInstance(Error));
			}
			else if (Case != TEXT("destroyed"))
			{
				TestTrue(TEXT("Rejected guard quarantines Session"), Session.GetSnapshot().bFaultQuarantined);
				TestFalse(TEXT("Rejected guard unloads VM"), Session.GetSnapshot().bHasActiveRuntime);
			}
			FAvidScriptWasmSmokeResult Stop;
			Session.StopAndUnload(Stop);
			TestFalse(TEXT("Stopped route cannot reenter"), FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver.Get(), 0, 0, {}, &Result));
			TestTrue(TEXT("Receiver route released"), Session.ClearGeneratedTypeInstance(Error));
			TestFalse(TEXT("Released authority cannot validate"), Session.ValidateGeneratedTypeReceiver(static_cast<int64>(Handle.ToUInt64()), 0));
		}
	}
	return true;
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
	FAvidScriptObjectRegistry Objects;
	FAvidScriptObjectHandleResult HandleResult;
	const auto ReceiverHandle = Objects.RegisterObject(Receiver.Get(), HandleResult, false);
	FAvidScriptWasmHostContext Context;
	Context.ObjectRegistry = &Objects;
	Context.OwnerHandle = ReceiverHandle;
	FAvidScriptRuntimeSession Session;
	Session.SetHostContext(Context);
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
	TestEqual(TEXT("Packed ObjectHandle low cell reaches C# this"), ScriptResult, static_cast<int32>(ReceiverHandle.Slot));
	const uint64 InitialGeneration = Session.GetGeneratedExecutionGeneration();
	TestTrue(TEXT("Loaded Session has a nonzero execution generation"), InitialGeneration != 0);

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
	TestEqual(TEXT("Reloaded export preserves receiver identity"), ScriptResult, static_cast<int32>(ReceiverHandle.Slot));
	TestEqual(TEXT("Committed reload advances execution generation"), Session.GetGeneratedExecutionGeneration(), InitialGeneration + 1);

	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(TEXT("Session unload succeeds"), Session.StopAndUnload(StopResult));
	TestEqual(TEXT("Unloaded code retains its last identity for rejection diagnostics"), Session.GetGeneratedExecutionGeneration(), InitialGeneration + 1);
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
	TrapSession.SetHostContext(Context);
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
	TestTrue(TEXT("validated replacement can recover a quarantined generated Session"),
		TrapSession.LoadInitialModule(Module.GetData(), Module.Num(), Manifest, TrapLoadResult));
	TestFalse(TEXT("replacement clears old generation quarantine"), TrapSession.GetSnapshot().bFaultQuarantined);
	TestTrue(TEXT("recovered generated route executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver.Get(), 0, 0, {}, &ScriptResult));
	TestEqual(TEXT("recovered route retains its owner"), ScriptResult, static_cast<int32>(ReceiverHandle.Slot));
	TestTrue(TEXT("recovered Session stops"), TrapSession.StopAndUnload(StopResult));
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
		TEXT("Fresh Runtime host reports no installed generated package"),
		Host->HasInstalledPackage());
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
	TestTrue(
		TEXT("Runtime host reports the verified generated package"),
		Host->HasInstalledPackage());

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

	FAvidScriptRuntimeSession* const OwnedSession = Host->GetInstanceSessionForTesting(*Receiver);
	if (!TestNotNull(TEXT("Host exposes its test Session"), OwnedSession)) return false;
	const uint64 OriginalGeneration = OwnedSession->GetGeneratedExecutionGeneration();
	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> ReentrantReceiver(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	bool bObservedLiveExecution = false;
	OwnedSession->SetLiveExecutionObserverForTesting([&]()
	{
		bObservedLiveExecution = true;
		FString RejectedError;
		TestFalse(TEXT("active Guest cannot destroy its owning Session"), Host->EndInstance(*Receiver, RejectedError));
		TestFalse(TEXT("active Guest cannot mutate the instance map"), Host->BeginInstance(*ReentrantReceiver, 0, RejectedError));
		TestFalse(TEXT("active Guest cannot clear the package"), Host->ClearPackage(RejectedError));
		FAvidScriptGeneratedTypePackageReloadResult RejectedReload;
		TestFalse(TEXT("active Guest cannot start package replacement"),
			Host->ReloadPackageFromDescriptorFile(DescriptorPath, RejectedReload, RejectedError));
		Host->Shutdown();
		TestTrue(TEXT("rejected shutdown preserves package and current Session"), Host->HasInstalledPackage()
			&& Host->GetInstanceSessionForTesting(*Receiver) == OwnedSession && OwnedSession->IsLiveLoaded());
		TestEqual(TEXT("rejected mutations preserve receiver handle"), Host->GetRegisteredHandleCount(), 1);
	});
	FAvidScriptWasmSmokeResult ReentrantTick;
	TestTrue(TEXT("outer Guest entry survives rejected Host mutations"), OwnedSession->Tick(0.016f, ReentrantTick));
	TestTrue(TEXT("live execution observer ran"), bObservedLiveExecution);
	TestEqual(TEXT("rejected mutations do not advance code generation"), OwnedSession->GetGeneratedExecutionGeneration(), OriginalGeneration);
	TestEqual(TEXT("rejected mutations leave one owned instance"), Host->GetActiveInstanceCount(), 1);

	// Router activity can belong to another target while this Host's Session is idle.
	FGeneratedHostMutationProbe Probe;
	FAvidScriptGeneratedTypeInstanceRegistration ProbeRegistration;
	FAvidScriptGeneratedTypeRouter& Router = FAvidScriptGeneratedTypeRouter::Get();
	TestTrue(TEXT("native callback route registers"), Router.RegisterInstance(*ReentrantReceiver, {71, 9}, Probe, ProbeRegistration));
	Probe.Callback = [&]()
	{
		FString RejectedError;
		TestFalse(TEXT("foreign active route cannot tear down an idle peer Session"), Host->EndInstance(*Receiver, RejectedError));
		Host->Shutdown();
		TestTrue(TEXT("foreign route rejection retains ownership"), Host->GetInstanceSessionForTesting(*Receiver) == OwnedSession);
	};
	TestTrue(TEXT("native callback completes without destroying the peer"),
		FAvidScriptGeneratedTypeDispatcher::Invoke(ReentrantReceiver.Get(), 0, 0, {}, nullptr));
	ReentrantReceiver->MarkAsGarbage();
	TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> PruneTrigger(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
	FAvidScriptGeneratedTypeInstanceRegistration PruneRegistration;
	TestTrue(TEXT("new registration prunes a dead receiver route"), Router.RegisterInstance(*PruneTrigger, {72, 9}, Probe, PruneRegistration));
	TestTrue(TEXT("already-pruned registration teardown is idempotent"), ProbeRegistration.Reset());
	TestTrue(TEXT("prune trigger route tears down"), PruneRegistration.Reset());
	bool bObservedReload = false;
	OwnedSession->SetCandidateBeginPlayObserverForTesting([&]()
	{
		bObservedReload = true;
		FString RejectedError;
		TestFalse(TEXT("candidate callback cannot remove a map entry during reload"), Host->EndInstance(*Receiver, RejectedError));
		TestTrue(TEXT("Host transaction owns the mutation boundary"), RejectedError.Contains(TEXT("host transaction")));
		Host->Shutdown();
		TestTrue(TEXT("candidate callback preserves the original Session identity"), Host->GetInstanceSessionForTesting(*Receiver) == OwnedSession);
	});

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
	TestTrue(TEXT("reload mutation callback ran"), bObservedReload);
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
	TestEqual(TEXT("Rollback transaction prepares one candidate before failure"), RollbackResult.PreparedInstanceCount, 1);
	TestEqual(TEXT("Rejected transaction never publishes a candidate"), RollbackResult.ReloadedInstanceCount, 0);
	TestEqual(TEXT("Rollback transaction discards one prepared candidate"), RollbackResult.RolledBackInstanceCount, 1);
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
	TestFalse(TEXT("Cleared Runtime host reports no generated package"), Host->HasInstalledPackage());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedContextContinuationTest,
	"AvidScript.Runtime.GeneratedTypes.SessionContextContinuation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedContextContinuationTest::RunTest(const FString& Parameters)
{
	if (GEngine == nullptr) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptGeneratedCallbackSession"));
	if (World == nullptr) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	TArray<uint8> Wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	AppendWasmSection(Wasm, 1, {6, 0x60, 0, 0, 0x60, 0, 1, 0x7f, 0x60, 2, 0x7d, 0x7f, 1, 0x7e,
		0x60, 1, 0x7e, 1, 0x7f, 0x60, 5, 0x7f, 0x7e, 0x7f, 0x7f, 0x7f, 0, 0x60, 1, 0x7d, 0});
	TArray<uint8> Imports{2};
	auto Name = [&](const char* Text)
	{
		const int32 Length = FCStringAnsi::Strlen(Text); Imports.Add(static_cast<uint8>(Length));
		Imports.Append(reinterpret_cast<const uint8*>(Text), Length);
	};
	Name("avidscript"); Name("owner_get_slot"); Imports.Append({0, 1});
	Name("avidscript"); Name("continuation_delay"); Imports.Append({0, 2});
	AppendWasmSection(Wasm, 2, Imports);
	AppendWasmSection(Wasm, 3, {4, 0, 3, 4, 5});
	AppendWasmSection(Wasm, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
	TArray<uint8> Exports{4};
	AppendWasmExport(Exports, "avid_on_begin_play", 2);
	AppendWasmExport(Exports, "avid_ue_0123456789abcdef0123456789abcdef", 3);
	AppendWasmExport(Exports, "avid_on_continuation_v2", 4);
	AppendWasmExport(Exports, "avid_on_tick", 5);
	AppendWasmSection(Wasm, 7, Exports);
	const TArray<uint8> Begin{0, 0x43, 0x0a, 0xd7, 0x23, 0x3c, 0x41, 51, 0x10, 1, 0x1a, 0x0b},
		Get{0, 0x23, 0, 0x10, 0, 0x6a, 0x0b}, Callback{0, 0x23, 0, 0x41, 0xe8, 7, 0x6a, 0x24, 0, 0x0b}, Tick{0, 0x0b};
	TArray<uint8> Code{4};
	for (const auto* Body : {&Begin, &Get, &Callback, &Tick}) { Code.Add(static_cast<uint8>(Body->Num())); Code.Append(*Body); }
	AppendWasmSection(Wasm, 10, Code);
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FString Error;
	if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(BuildGeneratedTypeSessionManifest(), Registry, Error)) { AddError(Error); return false; }
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptObjectRegistry Objects;
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Owner(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(World));
		FAvidScriptObjectHandleResult HandleResult;
		FAvidScriptWasmHostContext Context;
		Context.World = World; Context.ObjectRegistry = &Objects;
		Context.OwnerHandle = Objects.RegisterObject(Owner.Get(), HandleResult, false);
		FAvidScriptRuntimeSession Session;
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		Session.SetBackendSelectionForTesting(Selection); Session.SetHostContext(Context);
		if (!Session.ConfigureGeneratedTypeInstance(*Owner, Context.OwnerHandle, 0, Registry, Error)) { AddError(Error); return false; }
		FAvidScriptWasmReloadManifest Manifest;
		Manifest.ModuleId = TEXT("generated_context_continuation"); Manifest.Language = TEXT("csharp");
		Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
		Manifest.RequiredExports = {TEXT("avid_on_begin_play"), TEXT("avid_on_tick"), TEXT("avid_on_continuation_v2")};
		Manifest.RequiredImports = {{TEXT("avidscript"), TEXT("owner_get_slot")}, {TEXT("avidscript"), TEXT("continuation_delay")}};
		FAvidScriptWasmReloadResult Loaded;
		if (!Session.LoadInitialModule(Wasm.GetData(), Wasm.Num(), Manifest, Loaded)) { AddError(Loaded.ErrorMessage); return false; }
		for (int32 Generation = 0; Generation < 2; ++Generation)
		{
			const auto State = Session.GetTestSnapshot().HostContext.InstanceExecutionState;
			if (!TestTrue(TEXT("production Session owns an explicit execution state"), State.IsValid())) return false;
			TestTrue(TEXT("instance lifecycle is Running"), Session.GetSnapshot().LifecycleState == EAvidScriptLifecycleState::Running);
			TestTrue(TEXT("Runtime default lifecycle remains Loaded"), Session.GetLiveRuntimeForTesting()->GetLifecycleState() == EAvidScriptLifecycleState::Loaded);
			World->Tick(LEVELTICK_All, 0); ++GFrameCounter; World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
			FAvidScriptWasmSmokeResult TickResult;
			if (!TestTrue(TEXT("generated Session pumps contextual continuation"), Session.TickLive(0.001f, TickResult))) { AddError(TickResult.ErrorMessage); return false; }
			int32 Value = 0;
			TestTrue(TEXT("generated route observes callback state"), FAvidScriptGeneratedTypeDispatcher::Invoke(Owner.Get(), 0, 0, {}, &Value));
			TestEqual(TEXT("callback executed exactly once in the owner context"), Value, 1000 + static_cast<int32>(Context.OwnerHandle.Slot));
			TestEqual(TEXT("continuation entry finalized"), Session.GetLivePendingContinuationCount(), 0);
			TestEqual(TEXT("Session reports the selected state's tick count"), Session.GetLiveTickCallCount(), 1);
			TestEqual(TEXT("default Runtime tick count remains zero"), Session.GetLiveRuntimeForTesting()->GetTickCallCount(), 0);
			FAvidScriptWasmSmokeResult Snapshot;
			TestTrue(TEXT("full snapshot selects the instance"), Session.CaptureLiveSnapshot(Snapshot));
			TestEqual(TEXT("full snapshot has instance tick count"), Snapshot.TickCallCount, 1);
			TestEqual(TEXT("hot snapshot has instance tick count"), Session.GetLiveHotSnapshot().TickCallCount, 1);
			if (Generation == 0 && !Session.ReloadModule(Wasm.GetData(), Wasm.Num(), Manifest, Loaded)) { AddError(Loaded.ErrorMessage); return false; }
			if (Generation == 0)
			{
				TestTrue(TEXT("reload retires old instance state"), State->IsRetired());
				TestTrue(TEXT("reload publishes a distinct state"), State != Session.GetTestSnapshot().HostContext.InstanceExecutionState);
			}
		}
		const auto FinalState = Session.GetTestSnapshot().HostContext.InstanceExecutionState;
		FAvidScriptWasmSmokeResult Stop;
		TestTrue(TEXT("generated callback Session stops"), Session.StopAndUnload(Stop));
		TestTrue(TEXT("stop retires retained execution state"), FinalState->IsRetired());
		TestTrue(TEXT("generated callback registration clears"), Session.ClearGeneratedTypeInstance(Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedSessionExecutionEntriesTest,
	"AvidScript.Runtime.GeneratedTypes.SessionExecutionEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedSessionExecutionEntriesTest::RunTest(const FString& Parameters)
{
	// Real VM exports exercise Session scheduling, debug suspension and event entry.
	TArray<uint8> Wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	AppendWasmSection(Wasm, 1, {10,
		0x60, 0, 0, // 0: lifecycle
		0x60, 1, 0x7e, 1, 0x7f, // 1: receiver / debug probe
		0x60, 1, 0x7d, 0, // 2: Tick
		0x60, 2, 0x7f, 0x7d, 0, // 3: event
		0x60, 4, 0x7e, 0x7f, 0x7f, 0x7f, 1, 0x7e, // 4: debug suspend
		0x60, 3, 0x7e, 0x7f, 0x7f, 1, 0x7f, // 5: frame read
		0x60, 2, 0x7e, 0x7f, 0, // 6: debug resume
		0x60, 2, 0x7f, 0x7f, 0, // 7: timer
		0x60, 2, 0x7d, 0x7f, 1, 0x7f, // 8: timer set once
		0x60, 8, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7d, 0x7d, 0x7d, 0}); // 9: gameplay event
	TArray<uint8> Imports{4};
	auto Import = [&](const char* Name, uint8 Type)
	{
		const char* Namespace = "avidscript";
		for (const char* Text : {Namespace, Name})
		{
			const int32 Length = FCStringAnsi::Strlen(Text);
			Imports.Add(static_cast<uint8>(Length)); Imports.Append(reinterpret_cast<const uint8*>(Text), Length);
		}
		Imports.Append({0, Type});
	};
	Import("avid_debug_probe", 1); Import("avid_debug_suspend", 4);
	Import("avid_debug_frame_read", 5); Import("timer_set_once", 8);
	AppendWasmSection(Wasm, 2, Imports);
	AppendWasmSection(Wasm, 3, {8, 0, 1, 2, 3, 6, 7, 9, 0});
	AppendWasmSection(Wasm, 5, {1, 0, 1});
	AppendWasmSection(Wasm, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
	TArray<uint8> Exports{9};
	AppendWasmExport(Exports, "avid_on_begin_play", 4);
	AppendWasmExport(Exports, "avid_ue_0123456789abcdef0123456789abcdef", 5);
	AppendWasmExport(Exports, "avid_on_tick", 6);
	AppendWasmExport(Exports, "avid_on_event", 7);
	AppendWasmExport(Exports, "avid_on_debug_resume", 8);
	AppendWasmExport(Exports, "avid_on_timer", 9);
	AppendWasmExport(Exports, "avid_on_gameplay_event", 10);
	AppendWasmExport(Exports, "avid_on_end_play", 11);
	Exports.Append({6, 'm', 'e', 'm', 'o', 'r', 'y', 2, 0});
	AppendWasmSection(Wasm, 7, Exports);
	const TArray<uint8> Begin{0, 0x43, 0, 0, 0, 0, 0x41, 7, 0x10, 3, 0x1a, 0x0b};
	const TArray<uint8> Get{0, 0x23, 0, 0x0b};
	// On a requested pause, commit one byte at non-null memory[16], route 1, then return.
	const TArray<uint8> Tick{0, 0x42, 1, 0x10, 0, 0x04, 0x40,
		0x42, 1, 0x41, 1, 0x41, 16, 0x41, 1, 0x10, 1, 0x1a, 0x0b, 0x0b};
	const TArray<uint8> Increment{0, 0x23, 0, 0x41, 1, 0x6a, 0x24, 0, 0x0b};
	const TArray<uint8> Resume{0, 0x20, 0, 0x41, 16, 0x41, 1, 0x10, 2, 0x1a,
		0x23, 0, 0x41, 10, 0x6a, 0x24, 0, 0x0b};
	TArray<uint8> Code{8};
	for (const auto* Body : {&Begin, &Get, &Tick, &Increment, &Resume, &Increment, &Increment, &Increment})
	{
		Code.Add(static_cast<uint8>(Body->Num())); Code.Append(*Body);
	}
	AppendWasmSection(Wasm, 10, Code);
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FString Error;
	if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(BuildGeneratedTypeSessionManifest(), Registry, Error)) return false;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptObjectRegistry Objects;
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Owner(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		FAvidScriptObjectHandleResult HandleResult;
		FAvidScriptWasmHostContext Context;
		Context.ObjectRegistry = &Objects;
		Context.OwnerHandle = Objects.RegisterObject(Owner.Get(), HandleResult, false);
		FAvidScriptRuntimeSession Session;
		Session.SetHostContext(Context);
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		Session.SetBackendSelectionForTesting(Selection);
		if (!Session.ConfigureGeneratedTypeInstance(*Owner, Context.OwnerHandle, 0, Registry, Error)) return false;
		FAvidScriptWasmReloadManifest Manifest;
		Manifest.ModuleId = TEXT("generated_session_entries"); Manifest.Language = TEXT("wasm");
		Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
		Manifest.RequiredExports = {TEXT("avid_on_begin_play"), TEXT("avid_on_tick"), TEXT("avid_on_debug_resume")};
		for (const TCHAR* Name : {TEXT("avid_debug_probe"), TEXT("avid_debug_suspend"), TEXT("avid_debug_frame_read"), TEXT("timer_set_once")})
			Manifest.RequiredImports.Add({TEXT("avidscript"), Name});
		FAvidScriptWasmReloadResult Load;
		if (!Session.LoadInitialModule(Wasm.GetData(), Wasm.Num(), Manifest, Load)) { AddError(Load.ErrorMessage); return false; }
		const auto State = Session.GetTestSnapshot().HostContext.InstanceExecutionState;
		TestEqual(TEXT("BeginPlay schedules an instance Timer"), Session.GetLivePendingTimerCount(), 1);
		FAvidScriptWasmSmokeResult Result;
		TestTrue(TEXT("Tick dispatches the instance Timer"), Session.Tick(0.01f, Result));
		TestEqual(TEXT("Timer belongs to instance state"), Session.GetLiveTimerCallbackCount(), 1);
		TestEqual(TEXT("Timer consumes its queue"), Session.GetLivePendingTimerCount(), 0);
		TestTrue(TEXT("ordinary event uses instance state"), Session.DispatchEvent(7, 2.0f, Result));
		FAvidScriptGameplayEvent Event; Event.Type = EAvidScriptGameplayEventType::Input;
		TestTrue(TEXT("typed gameplay event uses instance state"), Session.DispatchGameplayEventLive(Event, Result));
		Result.ErrorMessage = TEXT("hot-success-sentinel");
		TestTrue(TEXT("hot ordinary event succeeds"), Session.DispatchEventHot(8, 3.0f, Result));
		TestTrue(TEXT("hot typed event succeeds"), Session.DispatchGameplayEventHot(Event, Result));
		TestTrue(TEXT("hot Tick succeeds"), Session.TickHot(0.01f, Result));
		TestEqual(TEXT("hot success does not overwrite failure output"), Result.ErrorMessage, FString(TEXT("hot-success-sentinel")));
		TestEqual(TEXT("event count is per instance"), Session.GetLiveEventCallbackCount(), 4);
		TestTrue(TEXT("attach generated Session debugger"), Session.AttachDebugger({}));
		TestTrue(TEXT("request pause"), Session.RequestDebugPause());
		if (!TestTrue(TEXT("contextual Tick returns a valid suspension"), Session.Tick(0.01f, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		TestTrue(TEXT("generated Session is paused"), Session.GetDebugSnapshot().State == EAvidScriptDebugSessionState::Paused);
		TestFalse(TEXT("pause is not a Runtime fault"), Session.GetSnapshot().bFaultQuarantined);
		TestFalse(TEXT("paused Session rejects ordinary entry"), Session.DispatchEvent(9, 0.0f, Result));
		TestEqual(TEXT("paused rejection is explicit"), Result.ErrorCategory, FString(TEXT("debug_execution_suspended")));
		TestTrue(TEXT("contextual resume consumes suspended frame"), Session.ContinueDebugExecution(Result));
		TestTrue(TEXT("resume returns to Running"), Session.GetDebugSnapshot().State == EAvidScriptDebugSessionState::Running);
		int32 Value = 0;
		TestTrue(TEXT("generated member sees event and resume writes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Owner.Get(), 0, 0, {}, &Value));
		TestEqual(TEXT("one timer plus four events plus resume"), Value, 15);
		TestTrue(TEXT("instance EndPlay succeeds"), Session.EndPlayLive(Result));
		TestTrue(TEXT("EndPlay result reports selected lifecycle"), Result.bEndPlayCalled);
		TestTrue(TEXT("repeated EndPlay succeeds"), Session.EndPlayLive(Result));
		TestTrue(TEXT("EndPlay runs once"), State->GetLifecycleState() == EAvidScriptLifecycleState::Stopped);
		TestTrue(TEXT("stop after EndPlay succeeds"), Session.StopAndUnload(Result));
		TestTrue(TEXT("stop retires explicit state"), State->IsRetired());
		TestTrue(TEXT("execution fixture registration clears"), Session.ClearGeneratedTypeInstance(Error));
		auto Host = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Peer(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		ON_SCOPE_EXIT { Host->Shutdown(); };
		const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
		if (!Host->InstallPackage(Registry, Artifact, Error) || !Host->BeginInstance(*Owner, 0, Error)
			|| !Host->BeginInstance(*Peer, 0, Error)) { AddError(Error); return false; }
		auto* OwnerSession = Host->GetInstanceSessionForTesting(*Owner);
		auto* PeerSession = Host->GetInstanceSessionForTesting(*Peer);
		const auto OwnerState = OwnerSession->GetTestSnapshot().HostContext.InstanceExecutionState;
		TestEqual(TEXT("production owner schedules its Timer"), OwnerSession->GetLivePendingTimerCount(), 1);
		TestEqual(TEXT("production peer schedules a separate Timer"), PeerSession->GetLivePendingTimerCount(), 1);
		TestTrue(TEXT("production owner exits before its Timer fires"), Host->EndInstance(*Owner, Error));
		TestTrue(TEXT("exited owner state is retired"), OwnerState->IsRetired());
		TestEqual(TEXT("exited owner Timer is cancelled"), OwnerState->GetPendingTimerCount(), 0);
		TestEqual(TEXT("peer Timer survives owner exit"), PeerSession->GetLivePendingTimerCount(), 1);
		TestTrue(TEXT("production peer pumps its own Timer"), PeerSession->Tick(0.01f, Result));
		TestEqual(TEXT("only peer Timer executes"), PeerSession->GetLiveTimerCallbackCount(), 1);
		TestTrue(TEXT("production peer reads shared state after owner exit"), FAvidScriptGeneratedTypeDispatcher::Invoke(Peer.Get(), 0, 0, {}, &Value));
		TestEqual(TEXT("shared state includes one EndPlay and one surviving Timer"), Value, 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedPackagePreparedReloadTest,
	"AvidScript.Runtime.GeneratedTypes.PackagePreparedReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedPackagePreparedReloadTest::RunTest(const FString& Parameters)
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types;
	FString Error;
	if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(BuildGeneratedTypeSessionManifest(), Types, Error)) return false;
	const auto Wasm = BuildGeneratedTypeSessionModule(INDEX_NONE, false, true);
	FAvidScriptWasmReloadManifest Manifest;
	Manifest.ModuleId = TEXT("prepared_package_reload"); Manifest.Language = TEXT("wasm");
	Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
	Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
		auto Host = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
		ON_SCOPE_EXIT { Host->Shutdown(); };
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		if (!Host->InstallPackage(Types, Artifact, Error) || !Host->BeginInstance(*A, 0, Error) || !Host->BeginInstance(*B, 0, Error))
		{ AddError(Error); return false; }
		auto* SessionA = Host->GetInstanceSessionForTesting(*A);
		auto* SessionB = Host->GetInstanceSessionForTesting(*B);
		const auto BeforeA = SessionA->GetTestSnapshot(), BeforeB = SessionB->GetTestSnapshot();
		const auto GenerationA = SessionA->GetGeneratedExecutionGeneration(), GenerationB = SessionB->GetGeneratedExecutionGeneration();
		auto Call = [&](UObject* Owner, int32 Expected)
		{
			int32 Value = 0;
			TestTrue(TEXT("published generated route executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Owner, 0, 0, {}, &Value));
			TestEqual(TEXT("module global retains the original VM state"), Value, Expected);
		};
		TestTrue(TEXT("production owners share one VM"), BeforeA.LiveRuntimeIdentity == BeforeB.LiveRuntimeIdentity);
		Call(A.Get(), 1); Call(A.Get(), 2); Call(B.Get(), 3);
		A->Value = 100;
		int32 CandidateVisits = 0;
		FProperty* Property = FindFProperty<FProperty>(A->GetClass(), TEXT("Value"));
		if (!TestNotNull(TEXT("native effect property exists"), Property)) return false;
		auto ObserveCandidate = [&](IAvidScriptBindingHostEffectJournal* Journal)
		{
			++CandidateVisits;
			TestTrue(TEXT("candidate preparation retains both old VMs"), SessionA->GetLiveRuntimeForTesting() == BeforeA.LiveRuntimeIdentity
				&& SessionB->GetLiveRuntimeForTesting() == BeforeB.LiveRuntimeIdentity);
			int32 Rejected = 0;
			TestFalse(TEXT("package barrier fences old A"), FAvidScriptGeneratedTypeDispatcher::Invoke(A.Get(), 0, 0, {}, &Rejected));
			TestFalse(TEXT("package barrier fences old B"), FAvidScriptGeneratedTypeDispatcher::Invoke(B.Get(), 0, 0, {}, &Rejected));
			FAvidScriptWasmSmokeResult Stop;
			TestFalse(TEXT("candidate cannot unload an idle package peer"), SessionA->StopAndUnload(Stop));
			TestTrue(TEXT("old instance state is retained until publication"), !BeforeA.HostContext.InstanceExecutionState->IsRetired()
				&& !BeforeB.HostContext.InstanceExecutionState->IsRetired());
			FAvidScriptBindingHostEffectPrepareResult Effect;
			if (TestNotNull(TEXT("candidate owns a stable effect journal"), Journal))
			{
				TestTrue(TEXT("both candidate journals capture the same native property in order"), Journal->PrepareReflectedProperty(
					*BeforeA.HostContext.ObjectRegistry, BeforeA.HostContext.OwnerHandle, *A, *Property, Effect));
				A->Value += 7;
			}
		};
		for (int32 FailAfter : {1, 2})
		{
			SessionA->SetCandidateBeginPlayObserverForTesting(ObserveCandidate);
			SessionB->SetCandidateBeginPlayObserverForTesting(ObserveCandidate);
			Host->SetReloadFailureAfterInstanceCountForTesting(FailAfter);
			FAvidScriptGeneratedTypePackageReloadResult Result;
			TestFalse(TEXT("prepared package rejects injected failure"), Host->ReloadPackage(Types, Artifact, Result, Error));
			TestEqual(TEXT("prepared count is separate from published count"), Result.PreparedInstanceCount, FailAfter);
			TestEqual(TEXT("failure publishes no instances"), Result.ReloadedInstanceCount, 0);
			TestEqual(TEXT("all prepared candidates are discarded"), Result.RolledBackInstanceCount, FailAfter);
			TestTrue(TEXT("original package remains live"), Result.bRollbackPreservedLivePackage);
			TestEqual(TEXT("reverse journal rollback restores pre-package native property"), A->Value, 100);
			TestTrue(TEXT("A Runtime identity is unchanged"), SessionA->GetLiveRuntimeForTesting() == BeforeA.LiveRuntimeIdentity);
			TestTrue(TEXT("B Runtime identity is unchanged"), SessionB->GetLiveRuntimeForTesting() == BeforeB.LiveRuntimeIdentity);
			TestTrue(TEXT("A instance state is unchanged"), SessionA->GetTestSnapshot().HostContext.InstanceExecutionState == BeforeA.HostContext.InstanceExecutionState);
			TestTrue(TEXT("B instance state is unchanged"), SessionB->GetTestSnapshot().HostContext.InstanceExecutionState == BeforeB.HostContext.InstanceExecutionState);
			TestEqual(TEXT("A generation is unchanged"), SessionA->GetGeneratedExecutionGeneration(), GenerationA);
			TestEqual(TEXT("B generation is unchanged"), SessionB->GetGeneratedExecutionGeneration(), GenerationB);
		}
		TestEqual(TEXT("both failure points actually prepared candidates"), CandidateVisits, 3);
		Call(A.Get(), 4); Call(B.Get(), 5);
		SessionA->SetCandidateBeginPlayObserverForTesting(ObserveCandidate);
		SessionB->SetCandidateBeginPlayObserverForTesting(ObserveCandidate);
		FAvidScriptGeneratedTypePackageReloadResult Committed;
		if (!TestTrue(TEXT("fully prepared package publishes"), Host->ReloadPackage(Types, Artifact, Committed, Error)))
		{ AddError(Error); return false; }
		TestEqual(TEXT("whole package prepared before publication"), Committed.PreparedInstanceCount, 2);
		TestEqual(TEXT("whole package publishes"), Committed.ReloadedInstanceCount, 2);
		TestEqual(TEXT("committed journals preserve candidate native effects"), A->Value, 114);
		TestTrue(TEXT("publication retires both old states"), BeforeA.HostContext.InstanceExecutionState->IsRetired()
			&& BeforeB.HostContext.InstanceExecutionState->IsRetired());
		TestEqual(TEXT("A generation advances exactly once"), SessionA->GetGeneratedExecutionGeneration(), GenerationA + 1);
		TestEqual(TEXT("B generation advances exactly once"), SessionB->GetGeneratedExecutionGeneration(), GenerationB + 1);
		TestTrue(TEXT("publication installs one shared candidate VM"), SessionA->GetLiveRuntimeForTesting() == SessionB->GetLiveRuntimeForTesting());
		Call(A.Get(), 1); Call(B.Get(), 2);
		const auto CurrentA = SessionA->GetTestSnapshot().HostContext;
		const auto CurrentB = SessionB->GetTestSnapshot().HostContext;
		int32 InvalidatingVisits = 0;
		auto InvalidateEffectTarget = [&](IAvidScriptBindingHostEffectJournal* Journal)
		{
			FAvidScriptBindingHostEffectPrepareResult Effect;
			if (Journal && Journal->PrepareReflectedProperty(*CurrentA.ObjectRegistry, CurrentA.OwnerHandle, *A, *Property, Effect))
				A->Value += 3;
			if (++InvalidatingVisits == 2) A->MarkAsGarbage();
		};
		SessionA->SetCandidateBeginPlayObserverForTesting(InvalidateEffectTarget);
		SessionB->SetCandidateBeginPlayObserverForTesting(InvalidateEffectTarget);
		FAvidScriptGeneratedTypePackageReloadResult Unrestorable;
		TestFalse(TEXT("owner invalidation rejects package preparation"), Host->ReloadPackage(Types, Artifact, Unrestorable, Error));
		TestEqual(TEXT("both candidate effect journals existed before invalidation"), InvalidatingVisits, 2);
		TestFalse(TEXT("failed native restoration cannot claim preserved package"), Unrestorable.bRollbackPreservedLivePackage);
		TestEqual(TEXT("unrestorable package publishes nothing"), Unrestorable.ReloadedInstanceCount, 0);
		TestEqual(TEXT("unrestorable package tears down every Session"), Host->GetActiveInstanceCount(), 0);
		TestEqual(TEXT("unrestorable package releases registry handles"), Host->GetRegisteredHandleCount(), 0);
		TestTrue(TEXT("fail-closed teardown retires both original states"), CurrentA.InstanceExecutionState->IsRetired()
			&& CurrentB.InstanceExecutionState->IsRetired());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedProductionDomainTest,
	"AvidScript.Runtime.GeneratedTypes.ProductionExecutionDomain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedProductionDomainTest::RunTest(const FString& Parameters)
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types, ReloadTypes;
	FString Error;
	if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(BuildGeneratedTypeSessionManifest(), Types, Error)
		|| !FAvidScriptGeneratedTypeRegistry::BuildFromJson(BuildGeneratedTypeSessionManifest(), ReloadTypes, Error))
	{ AddError(Error); return false; }
	FAvidScriptWasmReloadManifest Manifest;
	Manifest.ModuleId = TEXT("production_execution_domain"); Manifest.Language = TEXT("wasm");
	Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
	Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (int32 Scenario = 0; Scenario != 3; ++Scenario)
	{
		AddInfo(FString::Printf(TEXT("production domain backend=%d scenario=%d"), static_cast<int32>(Backend), Scenario));
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		const auto Wasm = BuildGeneratedTypeSessionModule(INDEX_NONE, Scenario == 1, Scenario != 1, Scenario == 2);
		const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
		auto Host = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
		TStrongObjectPtr<UWorld> OtherWorld(NewObject<UWorld>());
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> C(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Other(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(OtherWorld.Get()));
		ON_SCOPE_EXIT { Host->Shutdown(); };
		if (!Host->InstallPackage(Types, Artifact, Error) || !Host->BeginInstance(*A, 0, Error)
			|| !Host->BeginInstance(*Other, 0, Error)) { AddError(Error); return false; }
		auto* SA = Host->GetInstanceSessionForTesting(*A);
		auto* SO = Host->GetInstanceSessionForTesting(*Other);
		auto OldLease = SA->GetRuntimeLeaseForTesting();
		const auto StateA = SA->GetTestSnapshot().HostContext.InstanceExecutionState;
		TestTrue(TEXT("different Worlds use different VMs"), SA->GetLiveRuntimeForTesting() != SO->GetLiveRuntimeForTesting());
		if (Scenario == 2)
		{
			TestFalse(TEXT("second owner initialization trap rejects the join"), Host->BeginInstance(*B, 0, Error));
			TestTrue(TEXT("failed join quarantines the existing owner"), SA->GetSnapshot().bFaultQuarantined);
			TestFalse(TEXT("failed join releases existing shared VM"), OldLease.IsValid());
			TestTrue(TEXT("failed join retires the existing state"), StateA->IsRetired());
			TestTrue(TEXT("failed join leaves a different World running"), SO->IsLiveLoaded());
			TestEqual(TEXT("failed new owner releases its handle"), Host->GetRegisteredHandleCount(), 2);
			continue;
		}
		if (!Host->BeginInstance(*B, 0, Error)) { AddError(Error); return false; }
		auto* SB = Host->GetInstanceSessionForTesting(*B);
		const auto StateB = SB->GetTestSnapshot().HostContext.InstanceExecutionState;
		TestTrue(TEXT("same World owners share Runtime identity"), SA->GetLiveRuntimeForTesting() == SB->GetLiveRuntimeForTesting());
		TestTrue(TEXT("shared Runtime preserves distinct instance state"), StateA != StateB);
		int32 Value = 0;
		if (Scenario == 1)
		{
			TestFalse(TEXT("member trap fails the source call"), FAvidScriptGeneratedTypeDispatcher::Invoke(A.Get(), 0, 0, {}, &Value));
			TestTrue(TEXT("trap quarantines both owners"), SA->GetSnapshot().bFaultQuarantined && SB->GetSnapshot().bFaultQuarantined);
			TestFalse(TEXT("trap releases the complete domain VM"), OldLease.IsValid());
			TestTrue(TEXT("trap retires all owner states"), StateA->IsRetired() && StateB->IsRetired());
			TestFalse(TEXT("peer cannot enter failed shared state"), FAvidScriptGeneratedTypeDispatcher::Invoke(B.Get(), 0, 0, {}, &Value));
			TestTrue(TEXT("another World remains loaded after domain trap"), SO->IsLiveLoaded());
			continue;
		}
		auto Call = [&](UObject* Owner, int32 Expected)
		{
			TestTrue(TEXT("production member executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Owner, 0, 0, {}, &Value));
			TestEqual(TEXT("production global is shared only inside its World"), Value, Expected);
		};
		Call(A.Get(), 1); Call(B.Get(), 2); Call(Other.Get(), 1);
		FAvidScriptWasmReloadResult Rejected;
		TestFalse(TEXT("one owner cannot privately reload shared code"), SA->ReloadArtifact(Artifact, Rejected));
		TestTrue(TEXT("first owner releases its lease"), Host->EndInstance(*A, Error));
		TestTrue(TEXT("peer retains the VM after first owner exits"), OldLease.IsValid() && SB->IsLiveLoaded());
		TestTrue(TEXT("only exiting state retires"), StateA->IsRetired() && !StateB->IsRetired());
		Call(B.Get(), 3);
		FAvidScriptGeneratedTypePackageReloadResult Reloaded;
		TestTrue(TEXT("shared package reload accepts a fresh equivalent registry"), Host->ReloadPackage(ReloadTypes, Artifact, Reloaded, Error));
		TestFalse(TEXT("publication releases previous VM"), OldLease.IsValid());
		TestTrue(TEXT("old remaining owner state retires"), StateB->IsRetired());
		if (!Host->BeginInstance(*C, 0, Error)) { AddError(Error); return false; }
		auto* SC = Host->GetInstanceSessionForTesting(*C);
		TestTrue(TEXT("owner joining after reload reuses canonical type identity and VM"), SB->GetLiveRuntimeForTesting() == SC->GetLiveRuntimeForTesting());
		Call(B.Get(), 1); Call(C.Get(), 2); Call(Other.Get(), 1);
		auto ReloadedLease = SB->GetRuntimeLeaseForTesting();
		TestTrue(TEXT("reloaded owner B stops"), Host->EndInstance(*B, Error));
		TestTrue(TEXT("reloaded peer C remains"), ReloadedLease.IsValid());
		Call(C.Get(), 3);
		TestTrue(TEXT("last reloaded owner stops"), Host->EndInstance(*C, Error));
		TestFalse(TEXT("last owner destroys the Runtime"), ReloadedLease.IsValid());
		TestTrue(TEXT("unrelated World still owns its Runtime"), SO->IsLiveLoaded());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedCSharpAsyncInvocationTest,
	"AvidScript.Runtime.GeneratedTypes.CSharpAsyncInvocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpAsyncInvocationTest::RunTest(const FString& Parameters)
{
	if (!GEngine) return false;
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const bool bCapture : {false, true})
	for (int32 Scenario = 0; Scenario < 9; ++Scenario)
	{
		AddInfo(FString::Printf(TEXT("UE async invocation backend=%d capture=%d scenario=%d"), static_cast<int32>(Backend), bCapture, Scenario));
		const FString Stem = bCapture ? TEXT("csharp-ue-async-capture") : TEXT("csharp-ue-async-scalar");
		TArray<uint8> Wasm; FString MetadataText, Error;
		if (!FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory, Stem + TEXT(".wasm")))
			|| !FFileHelper::LoadFileToString(MetadataText, *FPaths::Combine(Directory, Stem + TEXT(".json")))) return false;
		TSharedPtr<FJsonObject> Metadata, RegistryJson;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MetadataText), Metadata)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BuildGeneratedTypeSessionManifest()), RegistryJson)) return false;
		const auto Type = RegistryJson->GetArrayField(TEXT("types"))[0]->AsObject();
		Type->SetStringField(TEXT("stable_type_id"), Metadata->GetStringField(TEXT("type_id")));
		Type->SetArrayField(TEXT("properties"), Metadata->GetArrayField(TEXT("properties")));
		const auto Function = Type->GetArrayField(TEXT("functions"))[0]->AsObject();
		Function->SetNumberField(TEXT("member_ordinal"), Metadata->GetNumberField(TEXT("member_ordinal")));
		Function->SetStringField(TEXT("stable_member_id"), Metadata->GetStringField(TEXT("method_id")));
		Function->SetStringField(TEXT("export_name"), Metadata->GetStringField(TEXT("export_name")));
		FString RegistryText;
		if (!FJsonSerializer::Serialize(RegistryJson.ToSharedRef(), TJsonWriterFactory<>::Create(&RegistryText))) return false;
		TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types, ReloadTypes;
		if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, Types, Error)
			|| !FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, ReloadTypes, Error)) { AddError(Error); return false; }
		FAvidScriptWasmReloadManifest Manifest;
		Manifest.ModuleId = TEXT("csharp_ue_async"); Manifest.Language = TEXT("csharp");
		Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
		Manifest.RequiredExports = {TEXT("avid_on_begin_play"), TEXT("avid_on_continuation_v2")};
		for (const auto& Import : Metadata->GetArrayField(TEXT("imports")))
			Manifest.RequiredImports.Add({Import->AsObject()->GetStringField(TEXT("module")), Import->AsObject()->GetStringField(TEXT("name"))});
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptUeAsyncWorld"));
		if (!World) return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		bool bWorldDestroyed = false;
		ON_SCOPE_EXIT { if (!bWorldDestroyed) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); } };
		auto Host = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
		ON_SCOPE_EXIT { Host->Shutdown(); };
		TStrongObjectPtr<UWorld> OtherWorld(NewObject<UWorld>());
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(World));
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(World));
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Other(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(OtherWorld.Get()));
		A->Value = 10; B->Value = 100; Other->Value = 1000;
		if (!Host->InstallPackage(Types, Artifact, Error) || !Host->BeginInstance(*A, 0, Error)
			|| !Host->BeginInstance(*B, 0, Error) || !Host->BeginInstance(*Other, 0, Error)) { AddError(Error); return false; }
		auto* SA = Host->GetInstanceSessionForTesting(*A);
		auto* SB = Host->GetInstanceSessionForTesting(*B);
		auto* SO = Host->GetInstanceSessionForTesting(*Other);
		TestTrue(TEXT("async peers share the production Runtime"), SA->GetLiveRuntimeForTesting() == SB->GetLiveRuntimeForTesting());
		TestTrue(TEXT("other World has an independent Runtime"), SB->GetLiveRuntimeForTesting() != SO->GetLiveRuntimeForTesting());
		const uint32 EntryOrdinal = static_cast<uint32>(Metadata->GetIntegerField(TEXT("member_ordinal")));
		auto Call = [&](UObject* Object, int32 Expected)
		{
			int32 Result = -1;
			return TestTrue(TEXT("compiled UE async entry executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Object, 0, EntryOrdinal, {}, &Result))
				&& TestEqual(TEXT("compiled UE async entry result"), Result, Expected);
		};
		if (!Call(A.Get(), 11) || !Call(B.Get(), 22) || !Call(A.Get(), 10) || !Call(Other.Get(), 11)) return false;
		TestEqual(TEXT("initial async side effect targets B"), B->Value, 103);
		TestEqual(TEXT("caller A owns no target continuation"), SA->GetLivePendingContinuationCount(), 0);
		TestEqual(TEXT("callee B owns the suspended invocation"), SB->GetLivePendingContinuationCount(), 1);
		if (Scenario == 7)
		{
			if (!Call(A.Get(), 10)) return false;
			TestEqual(TEXT("same UE object owns separate async invocations"), SB->GetLivePendingContinuationCount(), 2);
		}
		auto* Heap = SB->GetLiveRuntimeForTesting()->GetManagedHeapForTesting();
		bool bReloadedBeforeAllocation = false;
		auto Collect = [&](uint32 ExpectedRoots)
		{
			if (!Heap) return TestEqual(TEXT("scalar continuation needs no managed heap"), ExpectedRoots, 0u);
			// Scalar frames never initialize the managed heap. Reload likewise
			// publishes a fresh heap before any async closure has allocated.
			const auto Expected = !bCapture || bReloadedBeforeAllocation
				? AvidScript::Managed::EHeapError::NotConfigured : AvidScript::Managed::EHeapError::Ok;
			if (!TestTrue(TEXT("collect between UE async segments respects lazy heap configuration"), Heap->Collect() == Expected)) return false;
			TestEqual(TEXT("UE async suspends without Guest frames"), Heap->GetStats().ActiveFrames, 0u);
			return TestEqual(TEXT("UE async retains only the suspended state"), Heap->GetStats().LiveRoots, ExpectedRoots);
		};
		if (!Collect(bCapture ? (Scenario == 7 ? 2u : 1u) : 0u)) return false;
		auto AdvanceWorld = [&]() { World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter; };
		auto ResumeB = [&]()
		{
			FAvidScriptWasmSmokeResult Result;
			if (!SB->TickLive(0.001f, Result)) { AddError(Result.ErrorMessage); return false; }
			return true;
		};
		if (Scenario == 1) { if (!Host->EndInstance(*A, Error)) { AddError(Error); return false; } SA = nullptr; }
		if (Scenario == 3)
		{
			for (int32 I = 0; I < 3 && B->Value == 103; ++I) { AdvanceWorld(); if (!ResumeB()) return false; }
			TestEqual(TEXT("first UE resume restores this and amount"), B->Value, 107);
			if (!Collect(bCapture ? 1u : 0u)) return false;
		}
		if (Scenario == 5) { AdvanceWorld(); AdvanceWorld(); }
		if (Scenario == 2 || Scenario == 3 || Scenario == 5)
		{
			if (!Host->EndInstance(*B, Error)) { AddError(Error); return false; }
			SB = nullptr;
			if (!Collect(0)) return false;
		}
		if (Scenario == 4)
		{
			GEngine->DestroyWorldContext(World); World->DestroyWorld(false); bWorldDestroyed = true;
			TestEqual(TEXT("World teardown cancels target await"), SB->GetLivePendingContinuationCount(), 0);
			TestFalse(TEXT("World teardown invalidates target Session"), SB->IsLiveLoaded());
		}
		if (Scenario == 6)
		{
			auto Lease = SB->GetRuntimeLeaseForTesting();
			FAvidScriptGeneratedTypePackageReloadResult Reloaded;
			if (!Host->ReloadPackage(ReloadTypes, Artifact, Reloaded, Error)) { AddError(Error); return false; }
			TestFalse(TEXT("reload retires the suspended code lease"), Lease.IsValid());
			TestEqual(TEXT("reload cancels the old invocation"), SB->GetLivePendingContinuationCount(), 0);
			Heap = SB->GetLiveRuntimeForTesting()->GetManagedHeapForTesting();
			bReloadedBeforeAllocation = true;
		}
		if (Scenario == 8)
		{
			TWeakObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> WeakB(B.Get());
			B.Reset();
			CollectGarbage(RF_NoFlags);
			TestFalse(TEXT("suspended receiver and closure do not retain the UObject"), WeakB.IsValid());
			AdvanceWorld(); AdvanceWorld();
			TestEqual(TEXT("collected UObject cancels its pending invocation"), SB->GetLivePendingContinuationCount(), 0);
			// The object no longer exists; do not enter its retired receiver.
			SB = nullptr;
		}
		if (!bWorldDestroyed)
		{
			for (int32 I = 0; I < 6; ++I) { AdvanceWorld(); if (SB && !ResumeB()) return false; }
			if (!Collect(0)) return false;
			if (Heap) TestEqual(TEXT("UE async environments are reclaimed"), Heap->GetStats().LiveObjects, 0u);
		}
		if (B) TestEqual(TEXT("async target result respects owner cancellation"), B->Value,
			Scenario <= 1 ? 112 : Scenario == 3 ? 107 : Scenario == 7 ? 124 : 103);
		TestEqual(TEXT("caller property stays unchanged"), A->Value, 10);
		TestTrue(TEXT("another World remains live"), SO->IsLiveLoaded());
		TestEqual(TEXT("another World property stays unchanged"), Other->Value, 1000);
		TestEqual(TEXT("another World owns no stray await"), SO->GetLivePendingContinuationCount(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedLanguageErrorEntryTest,
	"AvidScript.Runtime.LanguageErrorCatalog.GeneratedUFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedLanguageErrorEntryTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptLanguageErrorCatalogTests/GuestFixtures"));
	TArray<uint8> Wasm;
	FString MetadataText;
	if (!TestTrue(TEXT("read generated language-error WASM"), FFileHelper::LoadFileToArray(
			Wasm, *FPaths::Combine(Directory, TEXT("generated-ufunction-entry.wasm"))))
		|| !TestTrue(TEXT("read generated language-error metadata"), FFileHelper::LoadFileToString(
			MetadataText, *FPaths::Combine(Directory, TEXT("generated-ufunction-entry.json")))))
		return false;
	TSharedPtr<FJsonObject> Metadata, RegistryJson;
	if (!TestTrue(TEXT("parse generated language-error metadata"), FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(MetadataText), Metadata))
		|| !TestTrue(TEXT("parse generated type registry fixture"), FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(BuildGeneratedTypeSessionManifest()), RegistryJson)))
		return false;
	const auto TypeJson = RegistryJson->GetArrayField(TEXT("types"))[0]->AsObject();
	TypeJson->SetStringField(TEXT("stable_type_id"), Metadata->GetStringField(TEXT("type_id")));
	const auto FunctionJson = TypeJson->GetArrayField(TEXT("functions"))[0]->AsObject();
	FunctionJson->SetNumberField(TEXT("member_ordinal"), Metadata->GetNumberField(TEXT("member_ordinal")));
	FunctionJson->SetStringField(TEXT("stable_member_id"), Metadata->GetStringField(TEXT("method_id")));
	FunctionJson->SetStringField(TEXT("native_name"), TEXT("GetLanguageErrorValue"));
	FunctionJson->SetStringField(TEXT("export_name"), Metadata->GetStringField(TEXT("export_name")));
	FString RegistryText, Error;
	if (!FJsonSerializer::Serialize(RegistryJson.ToSharedRef(), TJsonWriterFactory<>::Create(&RegistryText)))
		return false;
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types;
	if (!TestTrue(TEXT("build generated language-error registry"),
			FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, Types, Error)))
	{
		AddError(Error);
		return false;
	}
	FAvidScriptWasmReloadManifest Manifest;
	Manifest.ModuleId = Metadata->GetStringField(TEXT("module_id"));
	Manifest.Language = TEXT("csharp");
	Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
	Manifest.RequiredExports = {TEXT("avid_on_begin_play"), Metadata->GetStringField(TEXT("export_name"))};
	for (const auto& Import : Metadata->GetArrayField(TEXT("imports")))
		Manifest.RequiredImports.Add({Import->AsObject()->GetStringField(TEXT("module")),
			Import->AsObject()->GetStringField(TEXT("name"))});
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		AddInfo(FString::Printf(TEXT("generated language-error backend=%d"), static_cast<int32>(Backend)));
		FAvidScriptObjectRegistry Objects;
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Receiver(
			NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		FAvidScriptObjectHandleResult HandleResult;
		FAvidScriptWasmHostContext HostContext;
		HostContext.ObjectRegistry = &Objects;
		HostContext.OwnerHandle = Objects.RegisterObject(Receiver.Get(), HandleResult, false);
		FAvidScriptRuntimeSession Session;
		Session.SetHostContext(HostContext);
		Session.SetBackendSelectionForTesting(Selection);
		if (!Session.ConfigureGeneratedTypeInstance(*Receiver, HostContext.OwnerHandle, 0, Types, Error))
		{
			AddError(Error);
			return false;
		}
		FAvidScriptWasmReloadResult Loaded;
		if (!TestTrue(TEXT("load generated language-error instance"),
				Session.LoadInitialModule(Wasm.GetData(), Wasm.Num(), Manifest, Loaded)))
		{
			AddError(Loaded.ErrorMessage);
			return false;
		}
		auto* Runtime = Session.GetLiveRuntimeForTesting();
		FAvidScriptContextualExportCall Entry;
		if (!TestTrue(TEXT("prepare generated language-error entry"),
				Runtime->PrepareContextualExportCall(Metadata->GetStringField(TEXT("export_name")),
					Entry, Error)))
		{
			AddError(Error);
			return false;
		}
		const auto Context = Session.GetTestSnapshot().HostContext;
		FAvidScriptVmCallFrame Frame;
		Frame.CellCount = 3;
		Frame.Cells[0] = HostContext.OwnerHandle.Slot;
		Frame.Cells[1] = HostContext.OwnerHandle.Generation;
		Frame.Cells[2] = 5;
		FAvidScriptVmError Failure;
		FAvidScriptVmCallResult Value;
		if (!TestTrue(TEXT("generated value entry returns normally"),
				Runtime->InvokeInContext(Entry, Context, Frame, Failure, &Value)))
		{
			AddError(Failure.Details);
			return false;
		}
		TestEqual(TEXT("generated value matches C#"), static_cast<int32>(Value.Cells[0]), 7);
		const AvidScript::Managed::FHeap* Heap = Runtime->GetManagedHeapForTesting();
		TestTrue(TEXT("normal generated call leaves no invocation roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		Frame.Cells[2] = static_cast<uint32>(-1);
		TestFalse(TEXT("uncaught generated error fails the call"),
			Runtime->InvokeInContext(Entry, Context, Frame, Failure, &Value));
		TestEqual(TEXT("generated error has language category"), Failure.Category,
			FString(TEXT("language_error_uncaught")));
		TestTrue(TEXT("generated error retains source position"),
			Failure.Details.Contains(TEXT("Scripts/GeneratedLanguageError.cs")));
		TestTrue(TEXT("failed generated call leaves no invocation roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		FAvidScriptWasmSmokeResult Stopped;
		TestTrue(TEXT("generated test instance stops"), Session.StopAndUnload(Stopped));
		TestTrue(TEXT("generated test registration clears"), Session.ClearGeneratedTypeInstance(Error));

		auto Production = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
		ON_SCOPE_EXIT { Production->Shutdown(); };
		const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(
			NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> B(
			NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		if (!Production->InstallPackage(Types, Artifact, Error)
			|| !Production->BeginInstance(*A, 0, Error)
			|| !Production->BeginInstance(*B, 0, Error))
		{
			AddError(Error);
			return false;
		}
		auto* OwnerSession = Production->GetInstanceSessionForTesting(*A);
		auto* PeerSession = Production->GetInstanceSessionForTesting(*B);
		const auto Lease = OwnerSession->GetRuntimeLeaseForTesting();
		int32 Input = 5, Result = 0;
		FAvidScriptGeneratedCallArgument Argument{&Input};
		if (!TestTrue(TEXT("production generated UFunction returns normally"),
				FAvidScriptGeneratedTypeDispatcher::Invoke(A.Get(), 0,
					static_cast<uint32>(Metadata->GetNumberField(TEXT("member_ordinal"))),
					MakeArrayView(&Argument, 1), &Result)))
			return false;
		TestEqual(TEXT("production generated value matches C#"), Result, 7);
		Input = -1;
		TestFalse(TEXT("production generated UFunction reports uncaught error"),
			FAvidScriptGeneratedTypeDispatcher::Invoke(A.Get(), 0,
				static_cast<uint32>(Metadata->GetNumberField(TEXT("member_ordinal"))),
				MakeArrayView(&Argument, 1), &Result));
		const auto OwnerFault = OwnerSession->GetSnapshot();
		TestEqual(TEXT("production owner preserves language error category"), OwnerFault.FaultCategory,
			FString(TEXT("language_error_uncaught")));
		TestTrue(TEXT("production owner preserves source position"),
			OwnerFault.FaultDiagnostic.Contains(TEXT("Scripts/GeneratedLanguageError.cs")));
		TestTrue(TEXT("language error quarantines the shared execution domain"),
			OwnerFault.bFaultQuarantined && PeerSession->GetSnapshot().bFaultQuarantined);
		TestFalse(TEXT("language error releases the shared VM lease"), Lease.IsValid());
		TestFalse(TEXT("peer cannot reenter the failed generated package"),
			FAvidScriptGeneratedTypeDispatcher::Invoke(B.Get(), 0,
				static_cast<uint32>(Metadata->GetNumberField(TEXT("member_ordinal"))),
				MakeArrayView(&Argument, 1), &Result));
	}
	return true;
}

#endif
