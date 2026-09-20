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
void Leb(TArray<uint8>& Bytes, uint32 Value)
{
    do { uint8 Byte = Value & 127; Value >>= 7; Bytes.Add(Byte | (Value ? 128 : 0)); } while (Value);
}

// Rebuild only custom metadata on a known compiler fixture; code stays intact.
TArray<uint8> ReplaceMetadata(TConstArrayView<uint8> Wasm, const FString* Json, bool bDuplicate = false)
{
    auto ReadLeb = [&](int32& Position)
    {
        uint32 Value = 0, Shift = 0; uint8 Byte;
        do { check(Position < Wasm.Num() && Shift <= 28); Byte = Wasm[Position++]; Value |= uint32(Byte & 127) << Shift; Shift += 7; } while (Byte & 128);
        return Value;
    };
    TArray<uint8> Result; Result.Append(Wasm.GetData(), 8);
    for (int32 Position = 8; Position < Wasm.Num();)
    {
        const int32 Start = Position;
        const uint8 Id = Wasm[Position++];
        const uint32 Size = ReadLeb(Position);
        const int32 End = Position + Size;
        check(End <= Wasm.Num());
        bool bReplace = false;
        if (Id == 0)
        {
            const uint32 NameSize = ReadLeb(Position);
            check(NameSize <= static_cast<uint32>(End - Position));
            const FUTF8ToTCHAR Name(reinterpret_cast<const ANSICHAR*>(Wasm.GetData() + Position), NameSize);
            bReplace = FString(Name.Length(), Name.Get()) == TEXT("avidscript.host_call_frames");
        }
        if (!bReplace) Result.Append(Wasm.GetData() + Start, End - Start);
        Position = End;
    }
    if (Json)
    {
        TArray<uint8> Payload;
        const FTCHARToUTF8 Name(TEXT("avidscript.host_call_frames")), Text(**Json);
        Leb(Payload, Name.Length()); Payload.Append(reinterpret_cast<const uint8*>(Name.Get()), Name.Length());
        Payload.Append(reinterpret_cast<const uint8*>(Text.Get()), Text.Length());
        for (int32 I = 0; I != (bDuplicate ? 2 : 1); ++I)
        { Result.Add(0); Leb(Result, Payload.Num()); Result.Append(Payload); }
    }
    return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedCSharpMethodRoutesTest,
    "AvidScript.Runtime.GeneratedTypes.CSharpMethodRoutes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpMethodRoutesTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
    FString MetadataText, Error;
    if (!TestTrue(TEXT("read real compiler fixture metadata"), FFileHelper::LoadFileToString(MetadataText,
        *FPaths::Combine(Directory, TEXT("csharp-ue-cross-objects.json"))))) return false;
    TSharedPtr<FJsonObject> Metadata;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MetadataText), Metadata) || !Metadata) return false;
    auto Function = MakeShared<FJsonObject>();
    Function->SetNumberField(TEXT("member_ordinal"), Metadata->GetNumberField(TEXT("member_ordinal")));
    Function->SetStringField(TEXT("stable_member_id"), Metadata->GetStringField(TEXT("method_id")));
    Function->SetStringField(TEXT("native_name"), TEXT("GetScriptValue"));
    Function->SetStringField(TEXT("export_name"), Metadata->GetStringField(TEXT("export_name")));
    Function->SetArrayField(TEXT("flags"), {});
    auto Type = MakeShared<FJsonObject>();
    Type->SetNumberField(TEXT("type_ordinal"), 0);
    Type->SetStringField(TEXT("stable_type_id"), Metadata->GetStringField(TEXT("type_id")));
    Type->SetStringField(TEXT("engine_name"), TEXT("AvidScriptGeneratedTypeSessionTestObject"));
    Type->SetStringField(TEXT("class_path"), UAvidScriptGeneratedTypeSessionTestObject::StaticClass()->GetPathName());
    Type->SetArrayField(TEXT("properties"), {});
    Type->SetArrayField(TEXT("functions"), {MakeShared<FJsonValueObject>(Function)});
    auto Registry = MakeShared<FJsonObject>();
    Registry->SetNumberField(TEXT("schema_version"), 6);
    Registry->SetStringField(TEXT("generator_version"), TEXT("1.8"));
    Registry->SetStringField(TEXT("module_name"), TEXT("AvidScriptRuntime"));
    Registry->SetStringField(TEXT("generation_key_sha256"), FString::ChrN(64, 'a'));
    Registry->SetArrayField(TEXT("types"), {MakeShared<FJsonValueObject>(Type)});
    FString RegistryText;
    if (!FJsonSerializer::Serialize(Registry, TJsonWriterFactory<>::Create(&RegistryText))) return false;
    TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types, ReloadTypes;
    if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, Types, Error)
        || !FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryText, ReloadTypes, Error)) { AddError(Error); return false; }
    FAvidScriptWasmReloadManifest Manifest;
    Manifest.ModuleId = TEXT("csharp_method_routes"); Manifest.Language = TEXT("csharp");
    Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
    Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
    for (const auto& Import : Metadata->GetArrayField(TEXT("imports")))
        Manifest.RequiredImports.Add({Import->AsObject()->GetStringField(TEXT("module")), Import->AsObject()->GetStringField(TEXT("name"))});
    TArray<uint8> Canonical, Payload; bool bFound = false;
    if (!FFileHelper::LoadFileToArray(Canonical, *FPaths::Combine(Directory, TEXT("csharp-ue-cross-objects.wasm")))
        || !ReadAvidScriptWasmCustomSection(Canonical, TEXT("avidscript.host_call_frames"), 4 * 1024 * 1024, Payload, bFound, Error)
        || !bFound) { AddError(Error); return false; }
    const FUTF8ToTCHAR RouteText(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
    const FString RouteJson(RouteText.Length(), RouteText.Get());
    for (int32 Scenario = 0; Scenario != 13; ++Scenario)
    {
        TSharedPtr<FJsonObject> Routes;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RouteJson), Routes)) return false;
        auto First = Routes->GetArrayField(TEXT("exports"))[0]->AsObject();
        switch (Scenario)
        {
        case 2: Routes->SetNumberField(TEXT("schema_version"), 2); break;
        case 3: Routes->SetArrayField(TEXT("exports"), {}); break;
        case 4: First->SetStringField(TEXT("import_name"), TEXT("unlisted")); break;
        case 5: First->SetStringField(TEXT("name"), TEXT("missing_export")); break;
        case 6: First->SetNumberField(TEXT("frame_bytes"), 65552); break;
        case 7: First->SetNumberField(TEXT("frame_bytes"), 80.5); break;
        case 8: First->SetStringField(TEXT("signature_sha256"), FString::ChrN(64, 'z')); break;
        case 9: First->SetArrayField(TEXT("parameter_offsets"), {MakeShared<FJsonValueNumber>(72)}); break;
        case 10: First->SetArrayField(TEXT("root_token_offsets"), {MakeShared<FJsonValueNumber>(65536)}); break;
        case 11: First->SetArrayField(TEXT("root_token_offsets"), {MakeShared<FJsonValueNumber>(65)}); break;
        case 12: First->SetStringField(TEXT("extra"), TEXT("unsupported")); break;
        }
        FString Changed;
        if (!FJsonSerializer::Serialize(Routes.ToSharedRef(), TJsonWriterFactory<>::Create(&Changed))) return false;
        const auto Bad = AvidScriptMethodRouteTests::ReplaceMetadata(Canonical, Scenario == 0 ? nullptr : &Changed, Scenario == 1);
        FAvidScriptWasmRuntimeInstance Runtime;
        TArray<FAvidScriptVmTypedHostImport> Imports;
        TestFalse(*FString::Printf(TEXT("malformed method metadata is rejected before load, scenario %d"), Scenario),
            Runtime.ConfigureGeneratedTypeHostBindings(Types, Imports, Error, Bad));
        TestTrue(TEXT("failed method configuration exposes no partial imports"), Imports.IsEmpty() && !Error.IsEmpty());
        TestTrue(TEXT("same Runtime can configure canonical routes after rejection"), Runtime.ConfigureGeneratedTypeHostBindings(Types, Imports, Error, Canonical));
        TestFalse(TEXT("unloaded Runtime cannot invoke a method frame"), Runtime.InvokeGeneratedMethodFrame(0, 0, 80));
    }
    TArray<uint8> Readback; bool bReadFound = true;
    TestFalse(TEXT("custom metadata payload limit is enforced"), ReadAvidScriptWasmCustomSection(Canonical,
        TEXT("avidscript.host_call_frames"), 1, Readback, bReadFound, Error));
    TestTrue(TEXT("failed custom section read has no partial output"), Readback.IsEmpty() && !bReadFound);
    auto Truncated = Canonical; Truncated.Add(0); Truncated.Add(0x80);
    TestFalse(TEXT("truncated trailing section rejects previously found metadata"), ReadAvidScriptWasmCustomSection(Truncated,
        TEXT("avidscript.host_call_frames"), 4 * 1024 * 1024, Readback, bReadFound, Error));
    TestTrue(TEXT("late read failure is atomic"), Readback.IsEmpty() && !bReadFound);
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (const bool bStress : {false, true})
    {
        AddInfo(FString::Printf(TEXT("production CSharp A to B to A backend=%d stress=%d"), static_cast<int32>(Backend), bStress));
        TArray<uint8> Wasm;
        if (!TestTrue(TEXT("read real compiler WASM"), FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory,
            bStress ? TEXT("csharp-ue-cross-objects-stress.wasm") : TEXT("csharp-ue-cross-objects.wasm"))))) return false;
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
        auto Host = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
        TStrongObjectPtr<UWorld> OtherWorld(NewObject<UWorld>());
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Other(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(OtherWorld.Get()));
        ON_SCOPE_EXIT { Host->Shutdown(); };
        if (!Host->InstallPackage(Types, Artifact, Error) || !Host->BeginInstance(*A, 0, Error)
            || !Host->BeginInstance(*B, 0, Error) || !Host->BeginInstance(*Other, 0, Error)) { AddError(Error); return false; }
        auto* SA = Host->GetInstanceSessionForTesting(*A);
        auto* SB = Host->GetInstanceSessionForTesting(*B);
        auto* SO = Host->GetInstanceSessionForTesting(*Other);
        TestTrue(TEXT("production owners share one Runtime"), SA->GetLiveRuntimeForTesting() == SB->GetLiveRuntimeForTesting());
        TestTrue(TEXT("other World has independent static state"), SA->GetLiveRuntimeForTesting() != SO->GetLiveRuntimeForTesting());
        auto Call = [&](UObject* Owner, int32 Expected)
        {
            int32 Value = -1;
            if (!TestTrue(TEXT("production CSharp entry executes"), FAvidScriptGeneratedTypeDispatcher::Invoke(Owner, 0, 0, {}, &Value))) return false;
            return TestEqual(TEXT("CSharp result matches the same-source .NET oracle"), Value, Expected);
        };
        auto Exercise = [&]()
        {
            if (!Call(A.Get(), 11) || !Call(B.Get(), 22) || !Call(A.Get(), 511) || !Call(A.Get(), 511)
                || !Call(Other.Get(), 11) || !Call(Other.Get(), 33)) return false;
            auto* Heap = SA->GetLiveRuntimeForTesting()->GetManagedHeapForTesting();
            if (!TestNotNull(TEXT("CSharp heap exists"), Heap)) return false;
            TestTrue(TEXT("cross-object calls allocated shared reference objects"), Heap->GetStats().Allocations >= 6);
            if (bStress) TestTrue(TEXT("GC ran on every allocation"), Heap->GetStats().Collections >= Heap->GetStats().Allocations);
            TestEqual(TEXT("all reentrant frames unwind"), Heap->GetStats().ActiveFrames, 0u);
            TestEqual(TEXT("return and borrowed-reference roots unwind"), Heap->GetStats().LiveRoots, 0u);
            TestTrue(TEXT("unreachable shared objects collect"), Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
            return TestEqual(TEXT("no retained call objects"), Heap->GetStats().LiveObjects, 0u);
        };
        if (!Exercise()) return false;
        auto OldLease = SA->GetRuntimeLeaseForTesting();
        FAvidScriptGeneratedTypePackageReloadResult Reloaded;
        if (!TestTrue(TEXT("real compiler method routes prepare again on body reload"), Host->ReloadPackage(ReloadTypes, Artifact, Reloaded, Error)))
        { AddError(Error); return false; }
        TestFalse(TEXT("old code lease retires"), OldLease.IsValid());
        if (!Exercise()) return false;
        TestTrue(TEXT("retire target instance"), Host->EndInstance(*B, Error));
        int32 Value = -1;
        TestFalse(TEXT("stored weak receiver cannot reenter retired instance"), FAvidScriptGeneratedTypeDispatcher::Invoke(A.Get(), 0, 0, {}, &Value));
        TestTrue(TEXT("failed cross-object call quarantines shared state"), SA->GetSnapshot().bFaultQuarantined);
        TestTrue(TEXT("fault leaves other World running"), SO->IsLiveLoaded());
        if (!Call(Other.Get(), 33)) return false;
    }
    return true;
}
#endif
