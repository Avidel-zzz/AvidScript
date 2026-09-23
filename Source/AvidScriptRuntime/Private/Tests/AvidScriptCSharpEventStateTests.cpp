#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptBindingInvocation.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptCSharpEventStateTest,
    "AvidScript.Runtime.DelegateSubscription.CSharpManagedState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCSharpEventStateTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Managed;
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
    FString Descriptor;
    if (!TestTrue(TEXT("Production emitter generated event descriptor"), FFileHelper::LoadFileToString(Descriptor,
        *FPaths::Combine(Root, TEXT("event-facade.bindings.json"))))) return false;
    TSharedPtr<const FAvidScriptBindingPackage> Package;
    FAvidScriptBindingPackageLoadResult PackageResult;
    if (!FAvidScriptBindingPackage::LoadDescriptor(Descriptor, Package, PackageResult))
    { AddError(TEXT("Could not load production event descriptor")); return false; }
    TArray<FAvidScriptPreparedDelegateEvent> Catalog;
    FString Error;
    if (!Package->BuildPreparedDelegateEvents(Catalog, Error) || Catalog.Num() != 3)
    { AddError(Error); return false; }
    if (!GEngine) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptCSharpEventState"));
    if (!World) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

    for (const auto Backend : { EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr })
    for (const FString Kind : { TEXT("capture"), TEXT("bound"), TEXT("static"), TEXT("list"), TEXT("null"), TEXT("refout"), TEXT("singlecast") })
    {
        AddInfo(FString::Printf(TEXT("CSharp generated event backend=%d kind=%s"), static_cast<int32>(Backend), *Kind));
        FString Json;
        TSharedPtr<FJsonObject> Guest;
        const FString Stem = TEXT("csharp-event-generated-") + Kind;
        if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(Root, Stem + TEXT(".guest.json")))
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Guest))
        { AddError(TEXT("Run CSharpGuest.Tests --generated-event-state first")); return false; }
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Root, Stem + TEXT(".wasm")))) return false;
        auto Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("csharp_generated_event"));
        Manifest.BindingPackage = Package;
        Manifest.RequiredImports.Reset();
        for (const auto& Value : Guest->GetArrayField(TEXT("imports")))
        {
            auto Import = Value->AsObject();
            Manifest.RequiredImports.Add({ Import->GetStringField(TEXT("module")), Import->GetStringField(TEXT("name")) });
        }
        uint32 CountAddress = 0, ResultAddress = 0;
        for (const auto& Value : Guest->GetObjectField(TEXT("memory_layout"))->GetArrayField(TEXT("state_slots")))
        {
            auto Slot = Value->AsObject();
            FString Id = Slot->GetStringField(TEXT("global_id"));
            if (Id.Contains(TEXT("::Script.Count:"))) CountAddress = Slot->GetIntegerField(TEXT("offset"));
            if (Id.Contains(TEXT("::Script.Result:"))) ResultAddress = Slot->GetIntegerField(TEXT("offset"));
        }
        if (!TestTrue(TEXT("Compiler metadata contains observable globals"), CountAddress > 0 && ResultAddress > 0)) return false;
        AActor* Owner = World->SpawnActor<AActor>(Catalog[0].ExpectedSourceClass);
        if (!Owner) return false;
        FAvidScriptObjectRegistry Registry;
        FAvidScriptObjectHandleResult HandleResult;
        FAvidScriptRuntimeSession Session;
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        Session.SetBackendSelectionForTesting(Selection);
        FAvidScriptWasmHostContext Context;
        Context.World = World; Context.ObjectRegistry = &Registry;
        Context.OwnerHandle = Registry.RegisterObject(Owner, HandleResult, false);
        Session.SetHostContext(Context);
        FAvidScriptWasmReloadResult Loaded;
        if (!Session.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, Loaded)) { AddError(Loaded.ErrorMessage); return false; }
        auto* Runtime = Session.GetLiveRuntimeForTesting();
        // Production initial loading must NOT auto-bind a stateful callback.
        TestEqual(TEXT("Typed callbacks wait for explicit subscription"), Session.GetDelegateSubscriptionCountForTesting(), 0);
        TArray<FAvidScriptPreparedDelegateEvent> Events;
        if (!Runtime->BuildPreparedDelegateEvents(Events, Error) || Events.Num() != 1) { AddError(Error); return false; }
        const auto& Event = Events[0];
        TestTrue(TEXT("Resolved callback requires managed state"), Event.bRequiresManagedState);
        TestEqual(TEXT("Legacy API cannot bind a callback without state"), Runtime->HandleEventSubscribeImport(
            Context.OwnerHandle.Slot, Context.OwnerHandle.Generation, Event.EventOrdinal), int64(0));
        FAvidScriptWasmSmokeResult Result;
        if (!Runtime->Tick(0.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        auto& Heap = *Runtime->GetManagedHeapForTesting();
        const bool Empty = Kind == TEXT("null"), Outputs = Kind == TEXT("refout") || Kind == TEXT("singlecast");
        TestEqual(TEXT("Each real subscription owns one persistent root"), Heap.GetStats().LiveRoots, Empty ? 0u : 1u);
        TestTrue(TEXT("Collect after publisher returned"), Heap.Collect() == EHeapError::Ok);
        TestEqual(TEXT("Publisher temporary frames released"), Heap.GetStats().ActiveFrames, 0u);
        for (int32 Round = 0; Round < 3; ++Round)
        {
            FStructOnScope Frame(Event.Signature.SignatureFunction);
            auto SetInt = [&](const TCHAR* Name, int32 Value) { FindFProperty<FIntProperty>(Event.Signature.SignatureFunction, Name)->SetPropertyValue_InContainer(Frame.GetStructMemory(), Value); };
            auto GetInt = [&](const TCHAR* Name) { return FindFProperty<FIntProperty>(Event.Signature.SignatureFunction, Name)->GetPropertyValue_InContainer(Frame.GetStructMemory()); };
            if (Outputs) SetInt(TEXT("Value"), 2);
            else
            {
                FindFProperty<FObjectProperty>(Event.Signature.SignatureFunction, TEXT("SourceActor"))->SetObjectPropertyValue_InContainer(Frame.GetStructMemory(), Owner);
                SetInt(TEXT("Count"), 2);
                FindFProperty<FFloatProperty>(Event.Signature.SignatureFunction, TEXT("Scale"))->SetPropertyValue_InContainer(Frame.GetStructMemory(), Round == 1 ? -1.0f : 1.0f);
            }
            if (Event.Signature.Kind == EAvidScriptPreparedDelegateKind::Singlecast)
                Event.Signature.SinglecastProperty->GetPropertyValuePtr_InContainer(Owner)->ProcessDelegate<UObject>(Frame.GetStructMemory());
            else
                Event.Signature.MulticastProperty->GetMulticastDelegate(Event.Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))->ProcessDelegate<UObject>(Frame.GetStructMemory());
            if (!TestFalse(TEXT("Real CSharp event avoids quarantine"), Session.GetSnapshot().bFaultQuarantined)) return false;
            int32 Count = 0, Value = 0;
            TestTrue(TEXT("Read callback count"), Runtime->ReadStateBytes(CountAddress, MakeArrayView(reinterpret_cast<uint8*>(&Count), 4), Error));
            TestTrue(TEXT("Read shared callback state"), Runtime->ReadStateBytes(ResultAddress, MakeArrayView(reinterpret_cast<uint8*>(&Value), 4), Error));
            const int32 Calls = Outputs ? Round + 1 : FMath::Min(Round + 1, 2);
            TestEqual(TEXT("Event count, ordered list and self cancellation"), Count, Empty ? 0 : Calls * (Kind == TEXT("list") ? 2 : 1));
            TestEqual(TEXT("Repeated events mutate the same capture or receiver"), Value, Empty ? 0 : Kind == TEXT("list") ? (Calls == 1 ? 84 : 172) : 40 + 2 * Calls);
            if (Outputs)
            {
                TestEqual(TEXT("Actual UE ref parameter receives CSharp write"), GetInt(TEXT("Value")), Value);
                TestEqual(TEXT("Actual UE out parameter receives CSharp write"), GetInt(TEXT("Doubled")), Value * 2);
                if (Kind == TEXT("singlecast")) TestEqual(TEXT("UE receives delegate return value"), GetInt(TEXT("ReturnValue")), Value * 2 + 1);
            }
            TestTrue(TEXT("Collect after callback"), Heap.Collect() == EHeapError::Ok);
            if (Empty || (!Outputs && Round >= 1)) TestEqual(TEXT("Cancelled event graph reclaimed"), Heap.GetStats().LiveObjects, 0u);
        }
        Session.UnbindDelegateSubscriptionsForTesting();
        TestTrue(TEXT("Collect after owner releases subscriptions"), Heap.Collect() == EHeapError::Ok);
        TestEqual(TEXT("No callback objects remain"), Heap.GetStats().LiveObjects, 0u);
        TestEqual(TEXT("No callback roots remain"), Heap.GetStats().LiveRoots, 0u);
        TestEqual(TEXT("No callback frames remain"), Heap.GetStats().ActiveFrames, 0u);
        Session.StopAndUnload(Result);
        Owner->Destroy();
    }

    // Two calls reuse the same linear-memory frame. The second call must see
    // its zeroed local, while a separate call result must be visible by address.
    TArray<uint8> AddressFixture;
    if (!FFileHelper::LoadFileToArray(AddressFixture,
        *FPaths::Combine(Root, TEXT("address-taken-regression.wasm"))))
    { AddError(TEXT("Run WasmBackend.Tests to generate address-taken-regression.wasm")); return false; }
    for (const auto Backend : { EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr })
    {
        AddInfo(FString::Printf(TEXT("Address-taken scalar regression backend=%d"), static_cast<int32>(Backend)));
        AActor* ProbeOwner = World->SpawnActor<AActor>();
        if (!ProbeOwner) return false;
        FAvidScriptObjectRegistry Registry;
        FAvidScriptObjectHandleResult HandleResult;
        FAvidScriptWasmHostContext Context;
        Context.World = World; Context.ObjectRegistry = &Registry;
        Context.OwnerHandle = Registry.RegisterObject(ProbeOwner, HandleResult, false);
        FAvidScriptRuntimeSession Session;
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
            ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        Session.SetBackendSelectionForTesting(Selection);
        Session.SetHostContext(Context);
        auto Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("address_taken_regression"));
        FAvidScriptWasmReloadResult Loaded;
        if (!Session.LoadInitialModule(AddressFixture.GetData(), AddressFixture.Num(), Manifest, Loaded))
        { AddError(Loaded.ErrorMessage); return false; }
        auto* Runtime = Session.GetLiveRuntimeForTesting();
        FAvidScriptWasmSmokeResult Result;
        if (!Runtime->Tick(0.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        int32 Zero = -1, CallResult = -1;
        TestTrue(TEXT("Read reused scalar slot"), Runtime->ReadStateBytes(20, MakeArrayView(reinterpret_cast<uint8*>(&Zero), 4), Error));
        TestTrue(TEXT("Read address-taken call result"), Runtime->ReadStateBytes(16, MakeArrayView(reinterpret_cast<uint8*>(&CallResult), 4), Error));
        TestEqual(TEXT("Frame reuse starts with zeroed WASM local"), Zero, 0);
        TestEqual(TEXT("Call result is synchronized to address storage"), CallResult, 85);
        Session.StopAndUnload(Result);
        ProbeOwner->Destroy();
    }
    return true;
}
#endif
