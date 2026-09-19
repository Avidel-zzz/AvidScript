#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "AvidScriptHash.h"
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
	FAvidScriptGeneratedCSharpReceiverTest,
	"AvidScript.Runtime.GeneratedTypes.CSharpReceiver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpReceiverTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		for (const FString File : {TEXT("csharp-ue-receiver"), TEXT("csharp-ue-receiver-stress"), TEXT("csharp-ue-receiver-null")})
		{
			const bool bNull = File.EndsWith(TEXT("-null"));
			AddInfo(FString::Printf(TEXT("CSharp UE receiver backend=%d fixture=%s"), static_cast<int32>(Backend), *File));
			TArray<uint8> Wasm;
			FString MetadataText;
			if (!TestTrue(TEXT("Read current CSharp WASM"), FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory, File + TEXT(".wasm"))))) return false;
			if (!TestTrue(TEXT("Read compiler receiver metadata"), FFileHelper::LoadFileToString(MetadataText,
				*FPaths::Combine(Directory, bNull ? TEXT("csharp-ue-receiver-null.json") : TEXT("csharp-ue-receiver.json"))))) return false;
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
				TestTrue(TEXT("Repeated receiver bindings allocated contexts"), Heap->GetStats().Allocations >= 64);
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
			TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Other(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
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
			if (Case == TEXT("unknown-type") || Case == TEXT("wrong-signature"))
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
			}
			int32 Result = -1;
			const bool bCalled = FAvidScriptGeneratedTypeDispatcher::Invoke(Receiver.Get(), 0, 0, {}, &Result);
			const bool bExpectedLive = Case == TEXT("live") || Case == TEXT("world-normalized");
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
	TestEqual(TEXT("Reloaded export preserves receiver identity"), ScriptResult, 17);
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

#endif
