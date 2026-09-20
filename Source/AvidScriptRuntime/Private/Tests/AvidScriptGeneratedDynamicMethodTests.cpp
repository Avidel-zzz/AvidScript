#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "AvidScriptWasmModuleLayout.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

namespace AvidScriptMethodRouteTests
{
TArray<uint8> ReplaceMetadata(TConstArrayView<uint8> Wasm, const FString* Json, bool bDuplicate);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedCSharpDynamicMethodsTest,
    "AvidScript.Runtime.GeneratedTypes.CSharpDynamicMethods",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpDynamicMethodsTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
    FString MetadataText, Error;
    if (!FFileHelper::LoadFileToString(MetadataText, *FPaths::Combine(Directory, TEXT("csharp-ue-dispatch.json")))) return false;
    TSharedPtr<FJsonObject> Metadata;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MetadataText), Metadata)) return false;
    TArray<TSharedPtr<FJsonValue>> TypeRows;
    uint32 BaseOrdinal = MAX_uint32, EntryOrdinal = MAX_uint32;
    for (const auto& Value : Metadata->GetArrayField(TEXT("types")))
    {
        const auto& Input = *Value->AsObject();
        const bool bDerived = Input.GetBoolField(TEXT("derived"));
        UClass* Class = bDerived ? UAvidScriptGeneratedTypeDerivedTestObject::StaticClass() : UAvidScriptGeneratedTypeSessionTestObject::StaticClass();
        auto Row = MakeShared<FJsonObject>();
        const uint32 Ordinal = static_cast<uint32>(Input.GetIntegerField(TEXT("type_ordinal")));
        if (!bDerived) BaseOrdinal = Ordinal;
        Row->SetNumberField(TEXT("type_ordinal"), Ordinal);
        Row->SetStringField(TEXT("stable_type_id"), Input.GetStringField(TEXT("type_id")));
        Row->SetStringField(TEXT("engine_name"), Class->GetName());
        Row->SetStringField(TEXT("class_path"), Class->GetPathName());
        Row->SetArrayField(TEXT("properties"), {});
        TArray<TSharedPtr<FJsonValue>> Functions;
        for (const auto& FunctionValue : Input.GetArrayField(TEXT("functions")))
        {
            const auto& Function = *FunctionValue->AsObject();
            auto Entry = MakeShared<FJsonObject>();
            EntryOrdinal = static_cast<uint32>(Function.GetIntegerField(TEXT("member_ordinal")));
            Entry->SetNumberField(TEXT("member_ordinal"), EntryOrdinal);
            Entry->SetStringField(TEXT("stable_member_id"), Function.GetStringField(TEXT("method_id")));
            Entry->SetStringField(TEXT("native_name"), TEXT("GetScriptValue"));
            Entry->SetStringField(TEXT("export_name"), Function.GetStringField(TEXT("export_name")));
            Entry->SetArrayField(TEXT("flags"), {});
            Functions.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Row->SetArrayField(TEXT("functions"), Functions); TypeRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    auto Registry = MakeShared<FJsonObject>();
    Registry->SetNumberField(TEXT("schema_version"), 6);
    Registry->SetStringField(TEXT("generator_version"), TEXT("1.8"));
    Registry->SetStringField(TEXT("module_name"), TEXT("AvidScriptRuntime"));
    Registry->SetStringField(TEXT("generation_key_sha256"), FString::ChrN(64, 'b'));
    Registry->SetArrayField(TEXT("types"), TypeRows);
    FString RegistryText;
    if (!FJsonSerializer::Serialize(Registry, TJsonWriterFactory<>::Create(&RegistryText))) return false;
    TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types, ReloadTypes;
    if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, Types, Error)
        || !FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, ReloadTypes, Error)) { AddError(Error); return false; }
    FAvidScriptWasmReloadManifest Manifest;
    Manifest.ModuleId = TEXT("csharp_dynamic_methods"); Manifest.Language = TEXT("csharp");
    Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
    Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
    for (const auto& Import : Metadata->GetArrayField(TEXT("imports")))
        Manifest.RequiredImports.Add({Import->AsObject()->GetStringField(TEXT("module")), Import->AsObject()->GetStringField(TEXT("name"))});
    TArray<uint8> Canonical, Payload; bool bFound = false;
    if (!FFileHelper::LoadFileToArray(Canonical, *FPaths::Combine(Directory, TEXT("csharp-ue-dispatch.wasm")))
        || !ReadAvidScriptWasmCustomSection(Canonical, TEXT("avidscript.host_call_frames"), 4 * 1024 * 1024, Payload, bFound, Error)
        || !bFound) return false;
    const FUTF8ToTCHAR RouteText(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
    const FString RouteJson(RouteText.Length(), RouteText.Get());
    for (int32 Scenario = 0; Scenario < 12; ++Scenario)
    {
        TSharedPtr<FJsonObject> Routes;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RouteJson), Routes)) return false;
        TSharedPtr<FJsonObject> Dynamic;
        for (const auto& Row : Routes->GetArrayField(TEXT("exports")))
            if (Row->AsObject()->HasField(TEXT("dispatch_targets"))) { Dynamic = Row->AsObject(); break; }
        if (!Dynamic) return false;
        auto Targets = Dynamic->GetArrayField(TEXT("dispatch_targets"));
        const auto First = Targets[0]->AsObject();
        switch (Scenario)
        {
        case 0: Dynamic->RemoveField(TEXT("dispatch_targets")); break;
        case 1: Dynamic->SetArrayField(TEXT("dispatch_targets"), {}); break;
        case 2: First->SetNumberField(TEXT("selector"), 9999); break;
        case 3: Targets[1]->AsObject()->SetNumberField(TEXT("selector"), First->GetNumberField(TEXT("selector"))); break;
        case 4: Swap(Targets[0], Targets[1]); Dynamic->SetArrayField(TEXT("dispatch_targets"), Targets); break;
        case 5: First->SetStringField(TEXT("export_name"), TEXT("absent")); break;
        case 6: First->SetStringField(TEXT("export_name"), Dynamic->GetStringField(TEXT("name"))); break;
        case 7:
            for (const auto& Row : Routes->GetArrayField(TEXT("exports")))
                if (!Row->AsObject()->HasField(TEXT("dispatch_targets"))
                    && Row->AsObject()->GetStringField(TEXT("signature_sha256")) != Dynamic->GetStringField(TEXT("signature_sha256")))
                { First->SetStringField(TEXT("export_name"), Row->AsObject()->GetStringField(TEXT("name"))); break; }
            break;
        case 8: First->SetNumberField(TEXT("selector"), 0.5); break;
        case 9: Routes->SetNumberField(TEXT("schema_version"), 1); break;
        case 10: First->SetStringField(TEXT("extra"), TEXT("unknown")); break;
        case 11: First->RemoveField(TEXT("selector")); break;
        }
        FString Changed;
        if (!FJsonSerializer::Serialize(Routes.ToSharedRef(), TJsonWriterFactory<>::Create(&Changed))) return false;
        const auto Bad = AvidScriptMethodRouteTests::ReplaceMetadata(Canonical, &Changed, false);
        FAvidScriptWasmRuntimeInstance Runtime; TArray<FAvidScriptVmTypedHostImport> Imports;
        TestFalse(*FString::Printf(TEXT("malformed dynamic route rejected, scenario %d"), Scenario),
            Runtime.ConfigureGeneratedTypeHostBindings(Types, Imports, Error, Bad));
        TestTrue(TEXT("dynamic rejection exposes no partial capability"), Imports.IsEmpty() && !Error.IsEmpty());
        TestTrue(TEXT("canonical dynamic route configures after rejection"), Runtime.ConfigureGeneratedTypeHostBindings(Types, Imports, Error, Canonical));
    }
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (const bool bStress : {false, true})
    for (const int32 TargetKind : {0, 1, 2})
    {
        AddInfo(FString::Printf(TEXT("CSharp dynamic methods backend=%d stress=%d target-kind=%d"), static_cast<int32>(Backend), bStress, TargetKind));
        TArray<uint8> Wasm;
        if (!FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory,
            bStress ? TEXT("csharp-ue-dispatch-stress.wasm") : TEXT("csharp-ue-dispatch.wasm")))) return false;
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
        auto Host = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
        ON_SCOPE_EXIT { Host->Shutdown(); };
        TStrongObjectPtr<UWorld> OtherWorld(NewObject<UWorld>());
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
        UClass* TargetClass = TargetKind == 0 ? UAvidScriptGeneratedTypeSessionTestObject::StaticClass()
            : TargetKind == 1 ? UAvidScriptGeneratedTypeDerivedTestObject::StaticClass() : UAvidScriptGeneratedTypeNativeChildTestObject::StaticClass();
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(GetTransientPackage(), TargetClass));
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Other(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(OtherWorld.Get()));
        if (!Host->InstallPackage(Types, Artifact, Error) || !Host->BeginInstance(*A, BaseOrdinal, Error)
            // BeginInstance receives the declared base ordinal, then registers the
            // nearest known script class, including for an unregistered native child.
            || !Host->BeginInstance(*B, BaseOrdinal, Error)
            || !Host->BeginInstance(*Other, BaseOrdinal, Error)) { AddError(Error); return false; }
        auto* SA = Host->GetInstanceSessionForTesting(*A);
        auto* SO = Host->GetInstanceSessionForTesting(*Other);
        auto Call = [&](UObject* Object, int32 Expected)
        {
            int32 Result = -1;
            return TestTrue(TEXT("real CSharp dynamic entry executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Object, BaseOrdinal, EntryOrdinal, {}, &Result))
                && TestEqual(TEXT("virtual/interface/base result matches registration and .NET"), Result, Expected);
        };
        auto Exercise = [&]()
        {
            if (!Call(A.Get(), 11) || !Call(B.Get(), 22) || !Call(A.Get(), TargetKind != 0 ? 511 : 255)
                || !Call(A.Get(), TargetKind != 0 ? 511 : 255) || !Call(Other.Get(), 11) || !Call(Other.Get(), 33)) return false;
            auto* Heap = SA->GetLiveRuntimeForTesting()->GetManagedHeapForTesting();
            if (!TestNotNull(TEXT("dynamic calls use managed heap"), Heap)) return false;
            TestEqual(TEXT("dynamic frame scopes unwind"), Heap->GetStats().ActiveFrames, 0u);
            TestEqual(TEXT("dynamic roots unwind"), Heap->GetStats().LiveRoots, 0u);
            if (bStress) TestTrue(TEXT("dynamic references survived forced GC"), Heap->GetStats().Collections >= Heap->GetStats().Allocations);
            TestTrue(TEXT("dynamic temporary objects collect"), Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
            return TestEqual(TEXT("no dynamic method heap leak"), Heap->GetStats().LiveObjects, 0u);
        };
        if (!Exercise()) return false;
        FAvidScriptGeneratedTypePackageReloadResult Reloaded;
        if (!Host->ReloadPackage(ReloadTypes, Artifact, Reloaded, Error)) { AddError(Error); return false; }
        if (!Exercise()) return false;
        if (!Host->EndInstance(*B, Error)) { AddError(Error); return false; }
        int32 Result = -1;
        TestFalse(TEXT("dynamic weak interface target cannot reenter retired session"), FAvidScriptGeneratedTypeDispatcher::Invoke(A.Get(), BaseOrdinal, EntryOrdinal, {}, &Result));
        TestTrue(TEXT("dynamic failure quarantines shared domain"), SA->GetSnapshot().bFaultQuarantined);
        TestTrue(TEXT("dynamic failure leaves another World running"), SO->IsLiveLoaded());
        if (!Call(Other.Get(), 33)) return false;
    }
    return true;
}
#endif
