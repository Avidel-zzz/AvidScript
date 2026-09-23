#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptBindingInvocation.h"
#include "Lifecycle/AvidScriptRuntimeLifecycleCoordinator.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
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
#include "UObject/UObjectGlobals.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptCSharpEventLanguageTest,
    "AvidScript.Runtime.DelegateSubscription.CSharpEventLanguage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCSharpEventLanguageTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Managed;
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
    FString Descriptor, Json;
    if (!TestTrue(TEXT("Production event descriptor exists"), FFileHelper::LoadFileToString(Descriptor,
        *FPaths::Combine(Root, TEXT("event-facade.bindings.json"))))) return false;
    if (!TestTrue(TEXT("Compiled CSharp language fixture exists"), FFileHelper::LoadFileToString(Json,
        *FPaths::Combine(Root, TEXT("csharp-event-language.guest.json"))))) return false;
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("Compiled CSharp language WASM exists"), FFileHelper::LoadFileToArray(Bytes,
        *FPaths::Combine(Root, TEXT("csharp-event-language.wasm"))))) return false;
    TSharedPtr<const FAvidScriptBindingPackage> Package;
    FAvidScriptBindingPackageLoadResult PackageResult;
    if (!FAvidScriptBindingPackage::LoadDescriptor(Descriptor, Package, PackageResult))
    { AddError(TEXT("Could not load production event descriptor")); return false; }
    TSharedPtr<FJsonObject> Guest;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Guest) || !Guest.IsValid())
    { AddError(TEXT("Could not parse compiled language Guest IR")); return false; }
    TArray<FAvidScriptPreparedDelegateEvent> Catalog;
    FString Error;
    if (!Package->BuildPreparedDelegateEvents(Catalog, Error)) { AddError(Error); return false; }
    const FAvidScriptPreparedDelegateEvent* Signal = Catalog.FindByPredicate([](const auto& Event)
    {
        return Event.Signature.MulticastProperty != nullptr
            && Event.Signature.MulticastProperty->GetFName() == TEXT("OnScriptSignal");
    });
    if (!TestNotNull(TEXT("Selected generated event is in the native catalog"), Signal)) return false;
    uint32 CountAddress = 0, ResultAddress = 0, SourceEvaluationsAddress = 0, HandlerEvaluationsAddress = 0;
    uint32 TargetSlotAddress = 0, TargetGenerationAddress = 0;
    for (const auto& Value : Guest->GetObjectField(TEXT("memory_layout"))->GetArrayField(TEXT("state_slots")))
    {
        auto Slot = Value->AsObject();
        if (Slot->GetStringField(TEXT("global_id")).Contains(TEXT("::Script.Count:")))
            CountAddress = Slot->GetIntegerField(TEXT("offset"));
        if (Slot->GetStringField(TEXT("global_id")).Contains(TEXT("::Script.Result:")))
            ResultAddress = Slot->GetIntegerField(TEXT("offset"));
        if (Slot->GetStringField(TEXT("global_id")).Contains(TEXT("::Script.SourceEvaluations:")))
            SourceEvaluationsAddress = Slot->GetIntegerField(TEXT("offset"));
        if (Slot->GetStringField(TEXT("global_id")).Contains(TEXT("::Script.HandlerEvaluations:")))
            HandlerEvaluationsAddress = Slot->GetIntegerField(TEXT("offset"));
        if (Slot->GetStringField(TEXT("global_id")).Contains(TEXT("::Script.TargetSlot:")))
            TargetSlotAddress = Slot->GetIntegerField(TEXT("offset"));
        if (Slot->GetStringField(TEXT("global_id")).Contains(TEXT("::Script.TargetGeneration:")))
            TargetGenerationAddress = Slot->GetIntegerField(TEXT("offset"));
    }
    if (!TestTrue(TEXT("Compiler exposes event callback state"), CountAddress > 0 && ResultAddress > 0
        && SourceEvaluationsAddress > 0 && HandlerEvaluationsAddress > 0
        && TargetSlotAddress > 0 && TargetGenerationAddress > 0)) return false;
    if (!GEngine) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptCSharpEventLanguage"));
    if (!World) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

    for (const auto Backend : { EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr })
    {
        AddInfo(FString::Printf(TEXT("CSharp event language backend=%d"), static_cast<int32>(Backend)));
        AActor* Owner = World->SpawnActor<AActor>(Signal->ExpectedSourceClass);
        if (!TestNotNull(TEXT("Generated event owner spawned"), Owner)) return false;
        FAvidScriptObjectRegistry Registry;
        FAvidScriptObjectHandleResult HandleResult;
        FAvidScriptRuntimeSession Session;
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
            ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        Session.SetBackendSelectionForTesting(Selection);
        FAvidScriptWasmHostContext Context;
        Context.World = World; Context.ObjectRegistry = &Registry;
        Context.OwnerHandle = Registry.RegisterObject(Owner, HandleResult, false);
        Session.SetHostContext(Context);
        auto Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("csharp_event_language"));
        Manifest.BindingPackage = Package;
        Manifest.RequiredImports.Reset();
        for (const auto& Value : Guest->GetArrayField(TEXT("imports")))
        {
            auto Import = Value->AsObject();
            Manifest.RequiredImports.Add({ Import->GetStringField(TEXT("module")), Import->GetStringField(TEXT("name")) });
        }
        {
            FAvidScriptRuntimeSession InvalidSession;
            InvalidSession.SetBackendSelectionForTesting(Selection);
            InvalidSession.SetHostContext(Context);
            FAvidScriptWasmReloadResult InvalidLoaded;
            if (!InvalidSession.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, InvalidLoaded))
            { AddError(InvalidLoaded.ErrorMessage); return false; }
            auto* InvalidRuntime = InvalidSession.GetLiveRuntimeForTesting();
            FAvidScriptWasmSmokeResult InvalidResult;
            TestFalse(TEXT("Default event proxy rejects subscription"), InvalidRuntime->Tick(19.0f, InvalidResult));
            TestTrue(TEXT("Invalid source reports event-language source failure"),
                InvalidResult.ErrorMessage.Contains(TEXT("event_language_source")));
            TestEqual(TEXT("Invalid source publishes no bridge"), InvalidSession.GetDelegateSubscriptionCountForTesting(), 0);
            if (InvalidSession.IsLiveLoaded()) InvalidSession.StopAndUnload(InvalidResult);
        }
        for (const bool bLanguageFirst : { true, false })
        {
            FAvidScriptRuntimeSession ConflictSession;
            ConflictSession.SetBackendSelectionForTesting(Selection);
            ConflictSession.SetHostContext(Context);
            FAvidScriptWasmReloadResult ConflictLoaded;
            if (!ConflictSession.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, ConflictLoaded))
            { AddError(ConflictLoaded.ErrorMessage); return false; }
            auto* ConflictRuntime = ConflictSession.GetLiveRuntimeForTesting();
            FAvidScriptWasmSmokeResult ConflictResult;
            if (!ConflictRuntime->Tick(bLanguageFirst ? 11.0f : 21.0f, ConflictResult))
            { AddError(ConflictResult.ErrorMessage); return false; }
            TestEqual(TEXT("First singlecast owner installs one bridge"), ConflictSession.GetDelegateSubscriptionCountForTesting(), 1);
            int32 TokenValid = -1;
            if (!bLanguageFirst)
            {
                if (!ConflictRuntime->ReadStateBytes(ResultAddress,
                    MakeArrayView(reinterpret_cast<uint8*>(&TokenValid), 4), Error)) { AddError(Error); return false; }
                TestEqual(TEXT("Explicit singlecast token is valid before conflict"), TokenValid, 1);
            }
            if (bLanguageFirst)
            {
                if (!ConflictRuntime->Tick(21.0f, ConflictResult)) { AddError(ConflictResult.ErrorMessage); return false; }
                if (!ConflictRuntime->ReadStateBytes(ResultAddress,
                    MakeArrayView(reinterpret_cast<uint8*>(&TokenValid), 4), Error)) { AddError(Error); return false; }
                TestEqual(TEXT("Explicit token cannot replace language singlecast"), TokenValid, 0);
                TestEqual(TEXT("Rejected explicit bind leaves language bridge"), ConflictSession.GetDelegateSubscriptionCountForTesting(), 1);
            }
            else
            {
                TestFalse(TEXT("Language event cannot replace explicit singlecast"), ConflictRuntime->Tick(11.0f, ConflictResult));
                TestEqual(TEXT("Rejected language bind preserves original explicit bridge"), ConflictSession.GetDelegateSubscriptionCountForTesting(), 1);
            }
            if (ConflictSession.IsLiveLoaded()) ConflictSession.StopAndUnload(ConflictResult);
        }
        {
            FAvidScriptRuntimeSession ReloadSession;
            ReloadSession.SetBackendSelectionForTesting(Selection);
            ReloadSession.SetHostContext(Context);
            FAvidScriptWasmReloadResult ReloadResult;
            if (!ReloadSession.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, ReloadResult))
            { AddError(ReloadResult.ErrorMessage); return false; }
            auto* OldRuntime = ReloadSession.GetLiveRuntimeForTesting();
            FAvidScriptWasmSmokeResult TickResult;
            if (!OldRuntime->Tick(1.0f, TickResult)) { AddError(TickResult.ErrorMessage); return false; }
            TestEqual(TEXT("Live language bridge exists before candidate"), ReloadSession.GetDelegateSubscriptionCountForTesting(), 1);
            const TArray<uint8> InvalidBytes{ 0 };
            TestFalse(TEXT("Invalid reload candidate is rejected"), ReloadSession.ReloadModule(
                InvalidBytes.GetData(), InvalidBytes.Num(), Manifest, ReloadResult));
            TestTrue(TEXT("Rejected candidate preserves live runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
            TestEqual(TEXT("Rejected candidate preserves language bridge"), ReloadSession.GetDelegateSubscriptionCountForTesting(), 1);
            TestTrue(TEXT("Rejected candidate leaves UE event bound"), Signal->Signature.MulticastProperty->GetMulticastDelegate(
                Signal->Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))->IsBound());

            auto CandidateManifest = Manifest;
            CandidateManifest.ModuleId = TEXT("csharp_event_language_v2");
            if (!TestTrue(TEXT("Valid event module candidate commits"), ReloadSession.ReloadModule(
                Bytes.GetData(), Bytes.Num(), CandidateManifest, ReloadResult)))
            { AddError(ReloadResult.ErrorMessage); return false; }
            TestEqual(TEXT("Reload retires old language bridge"), ReloadSession.GetDelegateSubscriptionCountForTesting(), 0);
            TestFalse(TEXT("Reload unbinds old UE event"), Signal->Signature.MulticastProperty->GetMulticastDelegate(
                Signal->Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))->IsBound());
            auto* NewRuntime = ReloadSession.GetLiveRuntimeForTesting();
            if (!NewRuntime->Tick(1.0f, TickResult)) { AddError(TickResult.ErrorMessage); return false; }
            TestEqual(TEXT("New Runtime can establish its own language bridge"), ReloadSession.GetDelegateSubscriptionCountForTesting(), 1);
            if (!TestTrue(TEXT("Reloaded event Session stops"), ReloadSession.StopAndUnload(TickResult))) return false;
        }
        {
            UWorld* ForeignWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptCSharpEventForeignWorld"));
            if (!TestNotNull(TEXT("Foreign event World created"), ForeignWorld)) return false;
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(ForeignWorld);
            ON_SCOPE_EXIT { GEngine->DestroyWorldContext(ForeignWorld); ForeignWorld->DestroyWorld(false); };
            AActor* ForeignActor = ForeignWorld->SpawnActor<AActor>(Signal->ExpectedSourceClass);
            AActor* StaleActor = World->SpawnActor<AActor>(Signal->ExpectedSourceClass);
            if (!TestNotNull(TEXT("Foreign event source spawned"), ForeignActor)
                || !TestNotNull(TEXT("Stale event source spawned"), StaleActor)) return false;
            FAvidScriptObjectHandleResult SourceResult;
            const auto ForeignHandle = Registry.RegisterObject(ForeignActor, SourceResult, false);
            if (!TestTrue(TEXT("Foreign source handle registered"), SourceResult.bSucceeded)) return false;
            const auto StaleHandle = Registry.RegisterObject(StaleActor, SourceResult, false);
            if (!TestTrue(TEXT("Stale source handle registered"), SourceResult.bSucceeded)) return false;
            if (!TestTrue(TEXT("Stale source handle released"),
                Registry.ReleaseHandle(StaleHandle, SourceResult, false))) return false;
            for (const bool bForeignWorld : { true, false })
            {
                FAvidScriptRuntimeSession ProbeSession;
                ProbeSession.SetBackendSelectionForTesting(Selection);
                ProbeSession.SetHostContext(Context);
                FAvidScriptWasmReloadResult ProbeLoaded;
                if (!ProbeSession.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, ProbeLoaded))
                { AddError(ProbeLoaded.ErrorMessage); return false; }
                auto* ProbeRuntime = ProbeSession.GetLiveRuntimeForTesting();
                if (bForeignWorld && !TestTrue(TEXT("Foreign source capability granted before World check"),
                    ProbeSession.GetTestSnapshot().HostContext.ObjectOwnership->Borrow(
                        Registry, *ForeignActor, SourceResult))) return false;
                const auto TargetHandle = bForeignWorld ? ForeignHandle : StaleHandle;
                const int32 Slot = static_cast<int32>(TargetHandle.Slot);
                const int32 Generation = static_cast<int32>(TargetHandle.Generation);
                if (!TestTrue(TEXT("Inject CSharp event source slot"), ProbeRuntime->WriteStateBytes(
                    TargetSlotAddress, MakeArrayView(reinterpret_cast<const uint8*>(&Slot), 4), Error))
                    || !TestTrue(TEXT("Inject CSharp event source generation"), ProbeRuntime->WriteStateBytes(
                        TargetGenerationAddress, MakeArrayView(reinterpret_cast<const uint8*>(&Generation), 4), Error)))
                { AddError(Error); return false; }
                FAvidScriptWasmSmokeResult ProbeResult;
                TestFalse(TEXT("CSharp event rejects inaccessible source"), ProbeRuntime->Tick(22.0f, ProbeResult));
                TestTrue(TEXT("CSharp event reports source authority category"), ProbeResult.ErrorMessage.Contains(
                    bForeignWorld ? TEXT("event_language_world") : TEXT("event_language_source")));
                TestEqual(TEXT("Rejected CSharp source publishes no bridge"), ProbeSession.GetDelegateSubscriptionCountForTesting(), 0);
                TestFalse(TEXT("Rejected CSharp source leaves owner event unbound"),
                    Signal->Signature.MulticastProperty->GetMulticastDelegate(
                        Signal->Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))->IsBound());
                TestFalse(TEXT("Rejected CSharp source leaves foreign event unbound"),
                    Signal->Signature.MulticastProperty->GetMulticastDelegate(
                        Signal->Signature.MulticastProperty->ContainerPtrToValuePtr<void>(ForeignActor))->IsBound());
                if (ProbeSession.IsLiveLoaded()) ProbeSession.StopAndUnload(ProbeResult);
            }
            ForeignActor->Destroy();
            StaleActor->Destroy();
        }
        {
            AActor* GcOwner = World->SpawnActor<AActor>(Signal->ExpectedSourceClass);
            if (!TestNotNull(TEXT("GC event source spawned"), GcOwner)) return false;
            TWeakObjectPtr<AActor> WeakGcOwner = GcOwner;
            FAvidScriptObjectRegistry GcRegistry;
            FAvidScriptObjectHandleResult GcHandleResult;
            FAvidScriptWasmHostContext GcContext;
            GcContext.World = World;
            GcContext.ObjectRegistry = &GcRegistry;
            GcContext.OwnerHandle = GcRegistry.RegisterObject(GcOwner, GcHandleResult, false);
            if (!TestTrue(TEXT("GC event source registered"), GcHandleResult.bSucceeded)) return false;
            FAvidScriptRuntimeSession GcSession;
            GcSession.SetBackendSelectionForTesting(Selection);
            GcSession.SetHostContext(GcContext);
            FAvidScriptWasmReloadResult GcLoaded;
            if (!GcSession.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, GcLoaded))
            { AddError(GcLoaded.ErrorMessage); return false; }
            auto* GcRuntime = GcSession.GetLiveRuntimeForTesting();
            FAvidScriptWasmSmokeResult GcResult;
            if (!GcRuntime->Tick(1.0f, GcResult)) { AddError(GcResult.ErrorMessage); return false; }
            TestEqual(TEXT("GC source owns one language bridge"), GcSession.GetDelegateSubscriptionCountForTesting(), 1);
            TestEqual(TEXT("GC source owns one callback root"),
                GcRuntime->GetManagedHeapForTesting()->GetStats().LiveRoots, 1u);
            GcOwner->Destroy();
            GcOwner = nullptr;
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
            TestFalse(TEXT("Destroyed event source is invalid after GC"), WeakGcOwner.IsValid());
            TestEqual(TEXT("Post-GC removes language bridge"), GcSession.GetDelegateSubscriptionCountForTesting(), 0);
            TestEqual(TEXT("Post-GC releases callback root"),
                GcRuntime->GetManagedHeapForTesting()->GetStats().LiveRoots, 0u);
            if (GcSession.IsLiveLoaded() && !TestTrue(TEXT("GC event Session stops"),
                GcSession.StopAndUnload(GcResult))) return false;
        }
        FAvidScriptWasmReloadResult Loaded;
        if (!Session.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, Loaded))
        { AddError(Loaded.ErrorMessage); return false; }
        auto* Runtime = Session.GetLiveRuntimeForTesting();
        if (!TestNotNull(TEXT("Language event runtime loaded"), Runtime)) return false;
        TArray<FAvidScriptPreparedDelegateEvent> Events;
        if (!Runtime->BuildPreparedDelegateEvents(Events, Error) || Events.Num() != 3)
        { AddError(Error); return false; }
        const auto* SignalEvent = Events.FindByPredicate([](const auto& Candidate)
        {
            return Candidate.Signature.MulticastProperty != nullptr
                && Candidate.Signature.MulticastProperty->GetFName() == TEXT("OnScriptSignal");
        });
        const auto* RefOutEvent = Events.FindByPredicate([](const auto& Candidate)
        {
            return Candidate.Signature.MulticastProperty != nullptr
                && Candidate.Signature.MulticastProperty->GetFName() == TEXT("OnRefOutSignal");
        });
        const auto* SinglecastEvent = Events.FindByPredicate([](const auto& Candidate)
        {
            return Candidate.Signature.SinglecastProperty != nullptr
                && Candidate.Signature.SinglecastProperty->GetFName() == TEXT("OnSinglecastSignal");
        });
        if (!TestNotNull(TEXT("Generated signal event"), SignalEvent)
            || !TestNotNull(TEXT("Generated ref/out event"), RefOutEvent)
            || !TestNotNull(TEXT("Generated singlecast event"), SinglecastEvent)) return false;
        const auto& Event = *SignalEvent;
        if (!TestTrue(TEXT("Language event uses managed callback state"), Event.bRequiresManagedState)) return false;
        auto& Heap = *Runtime->GetManagedHeapForTesting();
        FAvidScriptWasmSmokeResult Result;
        const int32 ExpectedCounts[] = { 2, 6, 8, 8 };
        for (int32 Round = 0; Round < 4; ++Round)
        {
            if (!Runtime->Tick(static_cast<float>(Round + 1), Result))
            { AddError(Result.ErrorMessage); return false; }
            TestEqual(TEXT("One language bridge for repeated handlers"),
                Session.GetDelegateSubscriptionCountForTesting(), Round == 3 ? 0 : 1);
            FStructOnScope Frame(Event.Signature.SignatureFunction);
            FindFProperty<FObjectProperty>(Event.Signature.SignatureFunction, TEXT("SourceActor"))
                ->SetObjectPropertyValue_InContainer(Frame.GetStructMemory(), Owner);
            FindFProperty<FIntProperty>(Event.Signature.SignatureFunction, TEXT("Count"))
                ->SetPropertyValue_InContainer(Frame.GetStructMemory(), 2);
            FindFProperty<FFloatProperty>(Event.Signature.SignatureFunction, TEXT("Scale"))
                ->SetPropertyValue_InContainer(Frame.GetStructMemory(), 1.0f);
            Event.Signature.MulticastProperty->GetMulticastDelegate(
                Event.Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))
                ->ProcessDelegate<UObject>(Frame.GetStructMemory());
            if (!TestFalse(TEXT("Compiled event language avoids quarantine"), Session.GetSnapshot().bFaultQuarantined)) return false;
            int32 Count = 0;
            if (!Runtime->ReadStateBytes(CountAddress, MakeArrayView(reinterpret_cast<uint8*>(&Count), 4), Error))
            { AddError(Error); return false; }
            TestEqual(TEXT("CSharp += and -= follow duplicate/last-remove semantics"), Count, ExpectedCounts[Round]);
            TestTrue(TEXT("Collect between language event operations"), Heap.Collect() == EHeapError::Ok);
            TestEqual(TEXT("Language bridge retains only its active root"), Heap.GetStats().LiveRoots, Round == 3 ? 0u : 1u);
        }
        auto BroadcastSignal = [&]()
        {
            FStructOnScope Frame(Event.Signature.SignatureFunction);
            FindFProperty<FObjectProperty>(Event.Signature.SignatureFunction, TEXT("SourceActor"))
                ->SetObjectPropertyValue_InContainer(Frame.GetStructMemory(), Owner);
            FindFProperty<FIntProperty>(Event.Signature.SignatureFunction, TEXT("Count"))
                ->SetPropertyValue_InContainer(Frame.GetStructMemory(), 2);
            FindFProperty<FFloatProperty>(Event.Signature.SignatureFunction, TEXT("Scale"))
                ->SetPropertyValue_InContainer(Frame.GetStructMemory(), 1.0f);
            Event.Signature.MulticastProperty->GetMulticastDelegate(
                Event.Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))
                ->ProcessDelegate<UObject>(Frame.GetStructMemory());
        };
        auto ReadInt = [&](const uint32 Address, int32& Value)
        {
            return Runtime->ReadStateBytes(Address, MakeArrayView(reinterpret_cast<uint8*>(&Value), 4), Error);
        };
        if (!Runtime->Tick(5.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Captured handler opens one language bridge"), Session.GetDelegateSubscriptionCountForTesting(), 1);
        BroadcastSignal();
        int32 Count = 0, CapturedResult = 0;
        if (!ReadInt(CountAddress, Count) || !ReadInt(ResultAddress, CapturedResult))
        { AddError(Error); return false; }
        TestEqual(TEXT("Captured self-removal executes current callback once"), Count, 10);
        TestEqual(TEXT("Captured local mutates during event callback"), CapturedResult, 42);
        TestEqual(TEXT("Self-removal releases language bridge after dispatch"), Session.GetDelegateSubscriptionCountForTesting(), 0);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Self-removal affects the next event"), Count, 10);
        TestTrue(TEXT("Collect self-referential captured handler"), Heap.Collect() == EHeapError::Ok);
        TestEqual(TEXT("Self-removal releases captured cycle"), Heap.GetStats().LiveObjects, 0u);
        TestEqual(TEXT("Self-removal releases persistent root"), Heap.GetStats().LiveRoots, 0u);

        if (!Runtime->Tick(6.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Combined handler occupies one bridge"), Session.GetDelegateSubscriptionCountForTesting(), 1);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Removing last matching subsequence preserves first handler"), Count, 12);
        if (!Runtime->Tick(7.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Null add/remove leaves bridge unchanged"), Session.GetDelegateSubscriptionCountForTesting(), 1);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Null handler does not change callback list"), Count, 14);
        if (!Runtime->Tick(8.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Final removal releases combined bridge"), Session.GetDelegateSubscriptionCountForTesting(), 0);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Final removal stops later callbacks"), Count, 14);
        if (!Runtime->Tick(9.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Bound instance handler opens one bridge"), Session.GetDelegateSubscriptionCountForTesting(), 1);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Bound instance handler receives event"), Count, 16);
        TestEqual(TEXT("Equivalent bound method group removes bridge"), Session.GetDelegateSubscriptionCountForTesting(), 0);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Bound instance self-removal affects next event"), Count, 16);
        TestTrue(TEXT("Collect bound receiver after self-removal"), Heap.Collect() == EHeapError::Ok);
        TestEqual(TEXT("Bound receiver and language box are reclaimed"), Heap.GetStats().LiveObjects, 0u);

        if (!Runtime->Tick(10.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Language ref/out event owns one bridge"), Session.GetDelegateSubscriptionCountForTesting(), 1);
        {
            FStructOnScope Frame(RefOutEvent->Signature.SignatureFunction);
            auto* Value = FindFProperty<FIntProperty>(RefOutEvent->Signature.SignatureFunction, TEXT("Value"));
            auto* Doubled = FindFProperty<FIntProperty>(RefOutEvent->Signature.SignatureFunction, TEXT("Doubled"));
            Value->SetPropertyValue_InContainer(Frame.GetStructMemory(), 2);
            RefOutEvent->Signature.MulticastProperty->GetMulticastDelegate(
                RefOutEvent->Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))
                ->ProcessDelegate<UObject>(Frame.GetStructMemory());
            TestEqual(TEXT("Language ref callback updates UE ref"), Value->GetPropertyValue_InContainer(Frame.GetStructMemory()), 44);
            TestEqual(TEXT("Language ref callback updates UE out"), Doubled->GetPropertyValue_InContainer(Frame.GetStructMemory()), 88);
        }
        if (!ReadInt(CountAddress, Count) || !ReadInt(ResultAddress, CapturedResult))
        { AddError(Error); return false; }
        TestEqual(TEXT("Language ref callback executes once"), Count, 17);
        TestEqual(TEXT("Language ref callback preserves state"), CapturedResult, 44);

        if (!Runtime->Tick(11.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Language singlecast event adds one bridge"), Session.GetDelegateSubscriptionCountForTesting(), 2);
        {
            FStructOnScope Frame(SinglecastEvent->Signature.SignatureFunction);
            auto* Value = FindFProperty<FIntProperty>(SinglecastEvent->Signature.SignatureFunction, TEXT("Value"));
            auto* Doubled = FindFProperty<FIntProperty>(SinglecastEvent->Signature.SignatureFunction, TEXT("Doubled"));
            auto* ReturnValue = FindFProperty<FIntProperty>(SinglecastEvent->Signature.SignatureFunction, TEXT("ReturnValue"));
            Value->SetPropertyValue_InContainer(Frame.GetStructMemory(), 2);
            SinglecastEvent->Signature.SinglecastProperty->GetPropertyValuePtr_InContainer(Owner)
                ->ProcessDelegate<UObject>(Frame.GetStructMemory());
            TestEqual(TEXT("Language singlecast passes ref through both handlers"), Value->GetPropertyValue_InContainer(Frame.GetStructMemory()), 47);
            TestEqual(TEXT("Language singlecast passes out from last handler"), Doubled->GetPropertyValue_InContainer(Frame.GetStructMemory()), 94);
            TestEqual(TEXT("Language singlecast returns last handler result"), ReturnValue->GetPropertyValue_InContainer(Frame.GetStructMemory()), 95);
        }
        if (!ReadInt(CountAddress, Count) || !ReadInt(ResultAddress, CapturedResult))
        { AddError(Error); return false; }
        TestEqual(TEXT("Language singlecast executes both handlers"), Count, 19);
        TestEqual(TEXT("Language singlecast retains shared state"), CapturedResult, 47);

        if (!Runtime->Tick(12.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Mutation callback creates one language bridge"), Session.GetDelegateSubscriptionCountForTesting(), 3);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Handler added during dispatch waits until next event"), Count, 21);
        TestEqual(TEXT("In-callback replacement keeps one language bridge"), Session.GetDelegateSubscriptionCountForTesting(), 3);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Next event observes replacement handler"), Count, 121);
        if (!Runtime->Tick(13.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Removing callback-added handler releases language bridge"), Session.GetDelegateSubscriptionCountForTesting(), 2);

        if (!Runtime->Tick(14.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Explicit token subscribes beside other language events"), Session.GetDelegateSubscriptionCountForTesting(), 3);
        if (!Runtime->Tick(15.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Same-signal language event coexists with explicit token"), Session.GetDelegateSubscriptionCountForTesting(), 4);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Explicit and language handlers both receive event"), Count, 1123);
        if (!Runtime->Tick(16.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Language removal leaves explicit token bound"), Session.GetDelegateSubscriptionCountForTesting(), 3);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Explicit handler survives language removal"), Count, 2123);
        if (!Runtime->Tick(17.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Explicit cancellation leaves other language events bound"), Session.GetDelegateSubscriptionCountForTesting(), 2);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Cancelled explicit handler no longer runs"), Count, 2123);
        if (!Runtime->Tick(18.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Computed event source adds one language bridge"), Session.GetDelegateSubscriptionCountForTesting(), 3);
        int32 SourceEvaluations = 0, HandlerEvaluations = 0;
        if (!ReadInt(SourceEvaluationsAddress, SourceEvaluations)
            || !ReadInt(HandlerEvaluationsAddress, HandlerEvaluations)) { AddError(Error); return false; }
        TestEqual(TEXT("Event add evaluates source once"), SourceEvaluations, 1);
        TestEqual(TEXT("Event add evaluates handler once"), HandlerEvaluations, 1);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Computed event source binds correct Actor"), Count, 2125);
        if (!Runtime->Tick(20.0f, Result)) { AddError(Result.ErrorMessage); return false; }
        TestEqual(TEXT("Computed event removal leaves other events"), Session.GetDelegateSubscriptionCountForTesting(), 2);
        if (!ReadInt(SourceEvaluationsAddress, SourceEvaluations)
            || !ReadInt(HandlerEvaluationsAddress, HandlerEvaluations)) { AddError(Error); return false; }
        TestEqual(TEXT("Event remove evaluates source once"), SourceEvaluations, 2);
        TestEqual(TEXT("Event remove evaluates handler once"), HandlerEvaluations, 2);
        BroadcastSignal();
        if (!ReadInt(CountAddress, Count)) { AddError(Error); return false; }
        TestEqual(TEXT("Computed event removal stops callback"), Count, 2125);
        TestTrue(TEXT("Collect after independent event cancellations"), Heap.Collect() == EHeapError::Ok);
        TestEqual(TEXT("Only ref/out and singlecast roots remain"), Heap.GetStats().LiveRoots, 2u);
        if (Backend == EAvidScriptVmBackendKind::Wasmtime)
        {
            if (!TestTrue(TEXT("Session teardown succeeds with active language events"), Session.StopAndUnload(Result))) return false;
        }
        else
        {
            FAvidScriptRuntimeLifecycleCoordinator::Get().CleanupWorldForTesting(*World);
            TestTrue(TEXT("World teardown invalidates language event Session"), Session.GetSnapshot().bLifecycleInvalidated);
        }
        TestEqual(TEXT("Owner teardown cancels all language bridges"), Session.GetDelegateSubscriptionCountForTesting(), 0);
        TestFalse(TEXT("Owner teardown unbinds signal"), SignalEvent->Signature.MulticastProperty->GetMulticastDelegate(
            SignalEvent->Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))->IsBound());
        TestFalse(TEXT("Owner teardown unbinds ref/out"), RefOutEvent->Signature.MulticastProperty->GetMulticastDelegate(
            RefOutEvent->Signature.MulticastProperty->ContainerPtrToValuePtr<void>(Owner))->IsBound());
        TestFalse(TEXT("Owner teardown unbinds singlecast"), SinglecastEvent->Signature.SinglecastProperty
            ->GetPropertyValuePtr_InContainer(Owner)->IsBound());
        Owner->Destroy();
    }
    return true;
}
#endif
