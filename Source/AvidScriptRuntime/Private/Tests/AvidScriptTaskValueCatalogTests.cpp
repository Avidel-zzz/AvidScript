#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptWasmRuntime.h"
#include "AvidScriptHash.h"
#include "AvidScriptWasmModuleLayout.h"
#include "Continuation/AvidScriptWasmTaskValueCatalog.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace AvidScriptTaskValueCatalogTestPrivate
{
void U32(TArray<uint8>& Bytes, uint32 Value)
{
    do { const uint8 Part = Value & 0x7f; Value >>= 7; Bytes.Add(Part | (Value ? 0x80 : 0)); } while (Value);
}
void DuplicateSection(TArray<uint8>& Wasm, TConstArrayView<uint8> Payload)
{
    const ANSICHAR Name[] = "avidscript.task_values"; TArray<uint8> Section;
    U32(Section, UE_ARRAY_COUNT(Name) - 1); Section.Append(reinterpret_cast<const uint8*>(Name), UE_ARRAY_COUNT(Name) - 1);
    Section.Append(Payload.GetData(), Payload.Num()); Wasm.Add(0); U32(Wasm, Section.Num()); Wasm.Append(Section);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskValueCatalogModuleTest,
    "AvidScript.Runtime.Continuation.TaskValueCatalog.CanonicalModuleLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskValueCatalogModuleTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_TASK_VALUE_CATALOG_DIR"));
    TArray<uint8> Wasm, Absent;
    if (!TestFalse(TEXT("Original compiler catalog fixture directory configured"), Directory.IsEmpty())
        || !TestTrue(TEXT("Original catalog WASM reads"), FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory, TEXT("scalar.wasm"))))
        || !TestTrue(TEXT("Original legacy WASM reads"), FFileHelper::LoadFileToArray(Absent, *FPaths::Combine(Directory, TEXT("absent.wasm"))))) return false;
    TArray<uint8> Payload; bool bFound = false; FString Error;
    if (!TestTrue(TEXT("Canonical task section reads"), ReadAvidScriptWasmCustomSection(Wasm, TEXT("avidscript.task_values"),
        AvidScript::TaskResult::CatalogAbi::MaxCatalogBytes, Payload, bFound, Error) && bFound)) return false;
    TArray<uint8> Duplicate = Wasm; AvidScriptTaskValueCatalogTestPrivate::DuplicateSection(Duplicate, Payload);
    TArray<uint8> InvalidVm = Wasm; InvalidVm.Last() = 0xff;
    TArray<uint8> Future = Wasm; int32 Header = INDEX_NONE;
    for (int32 I = 0; I <= Future.Num() - Payload.Num(); ++I)
        if (FMemory::Memcmp(Future.GetData() + I, Payload.GetData(), Payload.Num()) == 0) { Header = I; break; }
    if (!TestTrue(TEXT("Catalog payload belongs to original module bytes"), Header != INDEX_NONE)) return false;
    Future[Header + 4] = 2;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    {
        FAvidScriptVmBackendSelection Selection; Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
            ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
        if (!TestTrue(TEXT("Compiler catalog module loads in both VMs"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("catalog-fixture"), Result)))
        { AddError(Result.ErrorMessage); return false; }
        const auto* Catalog = Runtime.GetTaskValueCatalog();
        if (!TestNotNull(TEXT("Catalog publishes with successfully loaded VM"), Catalog)) return false;
        TestTrue(TEXT("Loaded ordinal resolves original result"), Catalog->GetValues().GetResultCount() == 1
            && Catalog->GetValues().Find(1)->GetTypeId() == "type:int32");
        TestEqual(TEXT("Authority records actual canonical bytes"), Catalog->GetCanonicalSha256(), FAvidScriptHash::Sha256Hex(Wasm));
        Runtime.Unload(); TestNull(TEXT("Unload clears task type directory"), Runtime.GetTaskValueCatalog());
        TestTrue(TEXT("Legacy module still loads"), Runtime.LoadModule(Absent.GetData(), Absent.Num(), TEXT("catalog-fixture"), Result));
        TestNull(TEXT("Legacy module has no synthesized task capability"), Runtime.GetTaskValueCatalog()); Runtime.Unload();
        for (const TArray<uint8>* Invalid : {&Duplicate, &Future})
        {
            TestFalse(TEXT("Invalid catalog rejects loading before publication"), Runtime.LoadModule(Invalid->GetData(), Invalid->Num(), TEXT("catalog-fixture"), Result));
            TestNull(TEXT("Rejected catalog cannot survive in Runtime"), Runtime.GetTaskValueCatalog()); Runtime.Unload();
        }
        TestFalse(TEXT("Wrong loaded module identity rejects catalog"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("foreign-module"), Result));
        TestNull(TEXT("Foreign catalog grants no authority"), Runtime.GetTaskValueCatalog()); Runtime.Unload();
        TestFalse(TEXT("VM compilation failure rejects otherwise valid catalog"), Runtime.LoadModule(InvalidVm.GetData(), InvalidVm.Num(), TEXT("catalog-fixture"), Result));
        TestNull(TEXT("Failed VM never publishes candidate type directory"), Runtime.GetTaskValueCatalog()); Runtime.Unload();
    }
    return true;
}
#endif
