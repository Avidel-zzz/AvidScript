#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "AvidScriptLanguageErrorCatalog.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeBackendTestLanes.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "Validation/AvidScriptWasmImportPolicy.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBoundedGeneratedTypeBuildTest,
	"AvidScript.Runtime.BoundedGeneratedTypeBuild.FormalPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBoundedGeneratedTypeBuildTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptBoundedGeneratedTypeBuild"));
	FString TypeText;
	if (!TestTrue(TEXT("read formal generated type manifest"),
		FFileHelper::LoadFileToString(TypeText, *FPaths::Combine(Root,
			TEXT("AvidScriptGeneratedManifest.json")))))
		return false;
	TSharedPtr<FJsonObject> TypeManifest;
	if (!TestTrue(TEXT("parse formal generated type manifest"),
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TypeText), TypeManifest))
		|| !TestEqual(TEXT("exactly one generated type"),
			TypeManifest->GetArrayField(TEXT("types")).Num(), 1))
		return false;
	const TSharedPtr<FJsonObject> SourceType =
		TypeManifest->GetArrayField(TEXT("types"))[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>& Functions = SourceType->GetArrayField(TEXT("functions"));
	if (!TestEqual(TEXT("two source UFunctions"), Functions.Num(), 2))
		return false;
	FString HandledExport, UncaughtExport;
	auto Registry = MakeShared<FJsonObject>();
	Registry->SetNumberField(TEXT("schema_version"), 6);
	Registry->SetStringField(TEXT("generator_version"), TEXT("1.8"));
	Registry->SetStringField(TEXT("module_name"), TEXT("AvidScriptRuntime"));
	Registry->SetStringField(TEXT("generation_key_sha256"),
		TypeManifest->GetStringField(TEXT("generation_key_sha256")));
	auto RegistryType = MakeShared<FJsonObject>();
	RegistryType->SetNumberField(TEXT("type_ordinal"), 0);
	RegistryType->SetStringField(TEXT("stable_type_id"),
		SourceType->GetStringField(TEXT("stable_type_id")));
	RegistryType->SetStringField(TEXT("engine_name"),
		TEXT("AvidScriptGeneratedTypeSessionTestObject"));
	RegistryType->SetStringField(TEXT("class_path"),
		UAvidScriptGeneratedTypeSessionTestObject::StaticClass()->GetPathName());
	RegistryType->SetArrayField(TEXT("properties"), {});
	TArray<TSharedPtr<FJsonValue>> RegistryFunctions;
	for (const TSharedPtr<FJsonValue>& FunctionValue : Functions)
	{
		const TSharedPtr<FJsonObject> Function = FunctionValue->AsObject();
		const FString Name = Function->GetStringField(TEXT("native_name"));
		const FString Export = Function->GetStringField(TEXT("export_name"));
		if (Name == TEXT("ReadOrFallback")) HandledExport = Export;
		else if (Name == TEXT("RaiseUncaught")) UncaughtExport = Export;
		else return false;
		auto Route = MakeShared<FJsonObject>();
		Route->SetNumberField(TEXT("member_ordinal"), Function->GetNumberField(TEXT("member_ordinal")));
		Route->SetStringField(TEXT("stable_member_id"), Function->GetStringField(TEXT("stable_member_id")));
		Route->SetStringField(TEXT("native_name"), Name);
		Route->SetStringField(TEXT("export_name"), Export);
		Route->SetArrayField(TEXT("flags"), {});
		RegistryFunctions.Add(MakeShared<FJsonValueObject>(Route));
	}
	if (!TestTrue(TEXT("both generated exports exist"),
		!HandledExport.IsEmpty() && !UncaughtExport.IsEmpty()))
		return false;
	RegistryType->SetArrayField(TEXT("functions"), RegistryFunctions);
	Registry->SetArrayField(TEXT("types"), {MakeShared<FJsonValueObject>(RegistryType)});
	FString RegistryText, Error;
	if (!FJsonSerializer::Serialize(Registry, TJsonWriterFactory<>::Create(&RegistryText)))
		return false;
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types;
	if (!TestTrue(TEXT("register generated UFunction routes"),
		FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, Types, Error)))
	{
		AddError(Error);
		return false;
	}
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Wasm;
	TArray<uint8> CandidateWasm;
	if (!TestTrue(TEXT("read generated WASM for exact import preflight"),
		FFileHelper::LoadFileToArray(CandidateWasm,
			*FPaths::Combine(Root, TEXT("generated_types.wasm")))))
		return false;
	FAvidScriptWasmRuntimeInstance Preflight;
	TArray<FAvidScriptVmTypedHostImport> HostImports;
	if (!TestTrue(TEXT("preflight generated receiver capability"),
		Preflight.ConfigureGeneratedTypeHostBindings(Types, HostImports, Error, CandidateWasm)))
	{
		AddError(Error);
		return false;
	}
	TArray<FAvidScriptVmExpectedImport> AuthorizedImports;
	for (const FAvidScriptVmTypedHostImport& Import : HostImports)
	{
		if (!TestTrue(TEXT("generated import is an exact supplemental capability"),
			Import.bSupplementalRuntimeAuthority && Import.BindingOrdinal == MAX_uint32
			&& Import.ModuleName == TEXT("avidscript")))
			return false;
		AuthorizedImports.Add({Import.ModuleName, Import.ImportName});
	}
	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	const FScopedAvidScriptRuntimeImportAuthority ImportAuthority(AuthorizedImports);
	if (!TestTrue(TEXT("load formal Runtime manifest and canonical WASM"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			FPaths::Combine(Root, TEXT("generated_types.avidscript.json")),
			Manifest, Wasm, LoadResult)))
	{
		AddError(LoadResult.ErrorMessage);
		return false;
	}
	if (!TestTrue(TEXT("preflight and loaded WASM bytes match"),
		CandidateWasm == Wasm))
		return false;
	TestEqual(TEXT("type and Runtime module identity"), Manifest.ModuleId,
		FString(TEXT("bounded_generated_type_build")));
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptObjectRegistry Objects;
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Receiver(
			NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		FAvidScriptObjectHandleResult HandleResult;
		FAvidScriptWasmHostContext Context;
		Context.ObjectRegistry = &Objects;
		Context.OwnerHandle = Objects.RegisterObject(Receiver.Get(), HandleResult, false);
		FAvidScriptRuntimeSession Session;
		Session.SetHostContext(Context);
		Session.SetBackendSelectionForTesting(Lane.Selection);
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("configure generated receiver")),
			Session.ConfigureGeneratedTypeInstance(*Receiver, Context.OwnerHandle, 0, Types, Error)))
		{
			AddError(Error);
			return false;
		}
		FAvidScriptWasmReloadResult Loaded;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("load bounded generated package")),
			Session.LoadInitialModule(Wasm.GetData(), Wasm.Num(), Manifest, Loaded)))
		{
			AddError(Loaded.ErrorMessage);
			return false;
		}
		auto* Runtime = Session.GetLiveRuntimeForTesting();
		FAvidScriptContextualExportCall Handled, Uncaught;
		if (!Runtime->PrepareContextualExportCall(HandledExport, Handled, Error)
			|| !Runtime->PrepareContextualExportCall(UncaughtExport, Uncaught, Error))
		{
			AddError(Error);
			return false;
		}
		FAvidScriptVmCallFrame Frame;
		Frame.CellCount = 2;
		Frame.Cells[0] = Context.OwnerHandle.Slot;
		Frame.Cells[1] = Context.OwnerHandle.Generation;
		FAvidScriptVmError Failure;
		FAvidScriptVmCallResult Value;
		const auto ActiveContext = Session.GetTestSnapshot().HostContext;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("generated catch returns")),
			Runtime->InvokeInContext(Handled, ActiveContext, Frame, Failure, &Value)))
		{
			AddError(Failure.Details);
			return false;
		}
		TestEqual(TEXT("generated catch result"), static_cast<int32>(Value.Cells[0]), 7);
		const AvidScript::Managed::FHeap* Heap = Runtime->GetManagedHeapForTesting();
		TestTrue(TEXT("handled call releases error roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("generated uncaught error reports")),
			Runtime->InvokeInContext(Uncaught, ActiveContext, Frame, Failure, &Value));
		TestEqual(TEXT("uncaught error category"), Failure.Category,
			FString(TEXT("language_error_uncaught")));
		TestTrue(TEXT("uncaught error retains fixture location"),
			Failure.Details.Contains(TEXT("Fixtures/Phase66/BoundedGeneratedType.cs")));
		TestTrue(TEXT("failed call releases error roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		FAvidScriptWasmSmokeResult Stopped;
		TestTrue(TEXT("generated Session stops"), Session.StopAndUnload(Stopped));
		TestTrue(TEXT("generated registration clears"), Session.ClearGeneratedTypeInstance(Error));
	}
	return true;
}

#endif
