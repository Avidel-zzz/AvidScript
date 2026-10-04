#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorCSharpWorkspaceService.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptComponent.h"
#include "AvidScriptHash.h"
#include "AvidScriptRuntimeArtifact.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildInvoker.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildPipeline.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJob.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.h"

#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tickable.h"
#include "UObject/UObjectGlobals.h"

namespace AvidScript::EditorTests::GameplayProfile
{
struct FOwnedRoot
{
    FString Parent = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AvidScriptTests/GameplayLanguageProfile"));
    FString Root = Parent / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FAutomationTestBase& Test;
    explicit FOwnedRoot(FAutomationTestBase& InTest) : Test(InTest)
    {
        Test.TestTrue(TEXT("Unique test workspace creates"), IFileManager::Get().MakeDirectory(*Root, true));
        Test.AddInfo(TEXT("Gameplay profile owned workspace: ") + Root);
    }
    ~FOwnedRoot()
    {
        // Only this instance's freshly created GUID directory can be removed.
        Test.TestTrue(TEXT("Owned workspace stays inside the test parent"),
            FPaths::GetPath(Root) == Parent && FGuid::ParseExact(FPaths::GetCleanFilename(Root), EGuidFormats::Digits, Owner));
        if (FPaths::GetPath(Root) == Parent && Owner.IsValid())
            Test.TestTrue(TEXT("Own workspace is reclaimed"), IFileManager::Get().DeleteDirectory(*Root, false, true));
    }
private:
    FGuid Owner;
};

bool WriteJson(const FString& Path, const TSharedRef<FJsonObject>& Object)
{
    FString Json;
    return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json))
        && FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

TSharedPtr<FJsonObject> ReadJson(const FString& Path)
{
    FString Json;
    TSharedPtr<FJsonObject> Object;
    if (!FFileHelper::LoadFileToString(Json, *Path)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object)) return nullptr;
    return Object;
}

FString HashFile(const FString& Path)
{
    TArray<uint8> Bytes;
    return FFileHelper::LoadFileToArray(Bytes, *Path) ? FAvidScriptHash::Sha256Hex(Bytes) : FString();
}

TSharedRef<FJsonObject> MakeProfile(const FString& Source, int32 Schema = 11)
{
    auto Profile = MakeShared<FJsonObject>();
    Profile->SetNumberField(TEXT("schema_version"), Schema);
    Profile->SetStringField(TEXT("language"), TEXT("csharp"));
    Profile->SetStringField(TEXT("source_path"), Source);
    if (Schema == 11) Profile->SetStringField(TEXT("language_profile"), TEXT("gameplay-v1"));
    return Profile;
}

TSharedRef<FJsonObject> MakeReceipt()
{
    auto Receipt = MakeShared<FJsonObject>();
    Receipt->SetNumberField(TEXT("schema_version"), 1);
    Receipt->SetNumberField(TEXT("diagnostic_schema_version"), 1);
    Receipt->SetStringField(TEXT("result"), TEXT("direct_abi_built"));
    Receipt->SetBoolField(TEXT("succeeded"), true);
    Receipt->SetArrayField(TEXT("diagnostics"), {});
    auto Counts = MakeShared<FJsonObject>();
    for (const TCHAR* Tool : { TEXT("frontend"), TEXT("semantic"), TEXT("guest_ir"), TEXT("wasm_backend") })
        Counts->SetNumberField(Tool, 1);
    Receipt->SetObjectField(TEXT("tool_invocations"), Counts);
    for (const TCHAR* Field : { TEXT("semantic_cache"), TEXT("compilation_cache") })
    {
        auto Cache = MakeShared<FJsonObject>();
        Cache->SetNumberField(TEXT("schema_version"), 1);
        Cache->SetBoolField(TEXT("enabled"), false);
        Cache->SetBoolField(TEXT("published"), false);
        Cache->SetStringField(TEXT("lookup"), TEXT("disabled"));
        for (const TCHAR* Key : { TEXT("key"), TEXT("toolchain_fingerprint"), TEXT("entry_report_file"),
            TEXT("entry_report_sha256"), TEXT("diagnostic_code"), TEXT("diagnostic_message") })
            Cache->SetStringField(Key, TEXT(""));
        Receipt->SetObjectField(Field, Cache);
    }
    return Receipt;
}

TSharedRef<FJsonObject> MakeProfileIdentity(const FString& Name, const FString& Hash)
{
    auto Identity = MakeShared<FJsonObject>();
    Identity->SetStringField(TEXT("name"), Name);
    Identity->SetStringField(TEXT("contract_sha256"), Hash);
    return Identity;
}

class FControlledWatch final : public IAvidScriptEditorCSharpLiveReloadWatchHost
{
public:
    bool Start(const FString&, FOnChangeBatch InCallback, FString&, FString&) override
    { Callback = MoveTemp(InCallback); return true; }
    void Stop() override { Callback = {}; }
    bool IsWatching() const override { return !!Callback; }
    void Emit(const FString& Source)
    {
        FAvidScriptEditorCSharpLiveReloadChangeBatch Batch;
        Batch.FilePaths.Add(Source);
        Callback(MoveTemp(Batch));
    }
private:
    FOnChangeBatch Callback;
};

bool WaitForReload(FAvidScriptEditorCSharpLiveReloadService& Service, const int32 ExpectedCompletions)
{
    const double Deadline = FPlatformTime::Seconds() + 600;
    do
    {
        Service.Tick();
        const auto Status = Service.GetLastResult().Status;
        const auto& Stats = Service.GetStats();
        if (Stats.BuildSucceededCount + Stats.BuildFailedCount == ExpectedCompletions
            && (Status == EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildSucceeded
                || Status == EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed)) return true;
        FPlatformProcess::Sleep(0.01f);
    } while (FPlatformTime::Seconds() < Deadline);
    return false;
}

int32 ReadResult(FAutomationTestBase& Test, UAvidScriptComponent& Component, const FString& IrPath)
{
    const auto Ir = ReadJson(IrPath);
    auto* Session = Component.GetRuntimeSessionForTesting();
    auto* Runtime = Session ? Session->GetLiveRuntimeForTesting() : nullptr;
    if (!Ir.IsValid() || !Runtime) { Test.AddError(TEXT("Compiled result layout/runtime is unavailable")); return MIN_int32; }
    const FString Id = TEXT("global:symbol:field:$static:") + FAvidScriptHash::Sha256HexUtf8(
        TEXT("type:global::ObjectCancellationScript\nsymbol:field:global::ObjectCancellationScript.Result:int32"));
    for (const auto& Value : Ir->GetObjectField(TEXT("memory_layout"))->GetArrayField(TEXT("state_slots")))
    {
        const auto Slot = Value->AsObject();
        if (Slot->GetStringField(TEXT("global_id")) != Id) continue;
        int32 Result = MIN_int32;
        FString Error;
        const int32 Offset = Slot->GetIntegerField(TEXT("offset"));
        if (Slot->GetIntegerField(TEXT("size")) == 4 && Offset >= 0
            && Runtime->ReadStateBytes(Offset, MakeArrayView(reinterpret_cast<uint8*>(&Result), sizeof(Result)), Error)) return Result;
        Test.AddError(TEXT("Compiled result read failed: ") + Error);
        return MIN_int32;
    }
    Test.AddError(TEXT("Compiler did not declare the result slot"));
    return MIN_int32;
}

bool FinishObjectLoad(FAutomationTestBase& Test, UWorld& World, UAvidScriptComponent& Component,
    const FString& IrPath, const int32 Expected)
{
    FlushAsyncLoading();
    for (int32 Frame = 0; Frame < 200; ++Frame)
    {
        // StreamableManager dispatches completion on a worldless tickable;
        // World.Tick alone only advances tickables belonging to this World.
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.01f);
        World.Tick(LEVELTICK_All, 0.01f);
        // A synchronous Automation test remains in one engine frame. Advance
        // the isolated world's frame driver so Tick and next-tick timers can
        // execute on subsequent simulated frames, as in the Runtime fixtures.
        ++GFrameCounter;
        const int32 Value = ReadResult(Test, Component, IrPath);
        if (Value == MIN_int32) return false;
        if (Value == Expected) return true;
        FPlatformProcess::Sleep(0.005f);
    }
    const auto* Session = Component.GetRuntimeSessionForTesting();
    Test.AddError(FString::Printf(TEXT("Object wait exceeded test frames: result=%d expected=%d pending=%d; %s"),
        ReadResult(Test, Component, IrPath), Expected, Session ? Session->GetLivePendingContinuationCount() : -1,
        *Component.GetRuntimeStats().LastErrorMessage));
    return false;
}
} // namespace AvidScript::EditorTests::GameplayProfile

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGameplayProfileSchemaTest,
    "AvidScript.Editor.GameplayLanguageProfile.SchemaCompatibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGameplayProfileSchemaTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::EditorTests::GameplayProfile;
    FOwnedRoot Owned(*this);
    const FString Path = Owned.Root / TEXT("profile.json");
    const FString Source = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath();
    auto Load = [this, &Path](const TSharedRef<FJsonObject>& Json, const bool Expected, const TCHAR* Category = TEXT(""))
    {
        TestTrue(TEXT("Profile fixture writes"), WriteJson(Path, Json));
        FAvidScriptEditorCSharpProfileLoadResult Result;
        TestEqual(TEXT("Profile admission matches contract"), FAvidScriptEditorCSharpProfileService::LoadProfile(Path, Result), Expected);
        TestEqual(TEXT("Profile diagnostic remains specific"), Result.ErrorCategory, FString(Category));
        return Result;
    };
    for (int32 Schema = 1; Schema <= 10; ++Schema)
    {
        auto Legacy = MakeProfile(Source, Schema);
        TestTrue(TEXT("Existing schemas retain legacy mode"), Load(Legacy, true).BuildConfig.LanguageProfile.IsEmpty());
        Legacy->SetStringField(TEXT("language_profile"), TEXT("gameplay-v1"));
        Load(Legacy, false, TEXT("language_profile_schema_unsupported"));
    }
    auto Profile = MakeProfile(Source);
    TestEqual(TEXT("Explicit gameplay profile is retained"), Load(Profile, true).BuildConfig.LanguageProfile, FString(TEXT("gameplay-v1")));
    Profile->SetStringField(TEXT("language_profile"), TEXT(""));
    TestTrue(TEXT("Schema 11 can explicitly choose legacy"), Load(Profile, true).BuildConfig.LanguageProfile.IsEmpty());
    Profile->SetStringField(TEXT("language_profile"), TEXT("future-compiler-profile"));
    TestEqual(TEXT("Compiler owns name resolution"), Load(Profile, true).BuildConfig.LanguageProfile, FString(TEXT("future-compiler-profile")));
    for (const FString& Bad : { FString(TEXT(" gameplay-v1")), FString(TEXT("gameplay-v1 ")), FString::ChrN(129, TCHAR('a')) })
    {
        Profile->SetStringField(TEXT("language_profile"), Bad);
        Load(Profile, false, TEXT("language_profile_invalid"));
    }
    Profile->SetNumberField(TEXT("language_profile"), 1);
    Load(Profile, false, TEXT("language_profile_invalid"));
    Profile->SetBoolField(TEXT("language_profile"), true);
    Load(Profile, false, TEXT("language_profile_invalid"));
    Profile->SetField(TEXT("language_profile"), MakeShared<FJsonValueNull>());
    Load(Profile, false, TEXT("language_profile_invalid"));
    Profile->RemoveField(TEXT("language_profile"));
    Load(Profile, false, TEXT("language_profile_invalid"));
    Profile->SetNumberField(TEXT("schema_version"), 12);
    Load(Profile, false, TEXT("profile_schema_unsupported"));

    auto Binding = MakeShared<FJsonObject>();
    Binding->SetStringField(TEXT("package_name"), TEXT("avidscript.test.gameplay_schema"));
    auto Class = MakeShared<FJsonObject>();
    Class->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.Actor"));
    const TArray<TSharedPtr<FJsonValue>> Functions = { MakeShared<FJsonValueString>(TEXT("K2_GetActorLocation")) };
    Class->SetArrayField(TEXT("include_functions"), Functions);
    Class->SetArrayField(TEXT("native_direct_functions"), Functions);
    Binding->SetArrayField(TEXT("classes"), { MakeShared<FJsonValueObject>(Class) });
    auto Schema10 = MakeProfile(Source, 10);
    Schema10->SetObjectField(TEXT("binding_profile"), Binding);
    const auto OldBinding = Load(Schema10, true);
    auto Schema11 = MakeProfile(Source);
    Schema11->SetObjectField(TEXT("binding_profile"), Binding);
    const auto NewBinding = Load(Schema11, true);
    TestEqual(TEXT("Language selection does not change binding authorization"), NewBinding.BindingSelectionHash, OldBinding.BindingSelectionHash);
    TestTrue(TEXT("Existing legacy profile writes"), WriteJson(Path, Schema10));
    const FString LegacyHash = HashFile(Path);
    FAvidScriptEditorCSharpProfileTemplateResult Template;
    TestTrue(TEXT("Template refresh preserves existing profile"), FAvidScriptEditorCSharpProfileService::WriteProfileTemplate(Path, Template));
    TestEqual(TEXT("Legacy profile bytes remain unchanged"), HashFile(Path), LegacyHash);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGameplayProfileReceiptTest,
    "AvidScript.Editor.GameplayLanguageProfile.ReceiptAndRollback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGameplayProfileReceiptTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::EditorTests::GameplayProfile;
    FOwnedRoot Owned(*this);
    const FString HashA = FString::ChrN(64, TCHAR('a'));
    const FString HashB = FString::ChrN(64, TCHAR('b'));
    FAvidScriptEditorCSharpBuildConfig Config;
    Config.SourcePath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath();
    Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
    Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
    Config.OutputRoot = Owned.Root;
    Config.ReportPath = Owned.Root / TEXT("flow.csharp.report.json");
    Config.ManifestPath = Owned.Root / TEXT("flow.avidscript.json");
    Config.ArtifactStem = TEXT("flow");
    Config.LanguageProfile = TEXT("gameplay-v1");
    Config.VmArtifactPolicy = EAvidScriptEditorVmArtifactPolicy::JitOnly;
    TestTrue(TEXT("Receipt manifest fixture writes"), FFileHelper::SaveStringToFile(TEXT("{}"), *Config.ManifestPath));
    const FString WasmPath = Owned.Root / TEXT("flow.wasm");
    TestTrue(TEXT("Receipt artifact fixture writes"), FFileHelper::SaveStringToFile(TEXT("old-wasm"), *WasmPath));
    auto Finalize = [this, &Config](const TSharedRef<FJsonObject>& Receipt, bool Expected, const TCHAR* Category = TEXT(""))
    {
        TestTrue(TEXT("Receipt fixture writes"), WriteJson(Config.ReportPath, Receipt));
        FAvidScriptEditorCSharpBuildInvocation Invocation;
        FAvidScriptEditorCSharpBuildResult Result;
        TestTrue(TEXT("Production invocation prepares"), FAvidScriptEditorCSharpBuildInvoker::Prepare(Config, Invocation, Result));
        if (!Config.LanguageProfile.IsEmpty()) TestTrue(TEXT("Invocation forwards requested profile"), Invocation.Parameters.Contains(TEXT("-LanguageProfile \"gameplay-v1\"")));
        TestEqual(TEXT("Receipt admission matches request"), FAvidScriptEditorCSharpBuildInvoker::Finalize(Invocation, 0, TEXT(""), TEXT(""), Result), Expected);
        TestEqual(TEXT("Receipt rejection has stable category"), Result.ErrorCategory, FString(Category));
        return Result;
    };
    auto Receipt = MakeReceipt();
    Finalize(Receipt, false, TEXT("language_profile_receipt_mismatch"));
    Receipt->SetObjectField(TEXT("language_profile"), MakeProfileIdentity(TEXT("other-profile"), HashA));
    Finalize(Receipt, false, TEXT("language_profile_receipt_mismatch"));
    Receipt->SetField(TEXT("language_profile"), MakeShared<FJsonValueNull>());
    Finalize(Receipt, false, TEXT("report_contract_invalid"));
    Receipt->SetStringField(TEXT("language_profile"), TEXT("gameplay-v1"));
    Finalize(Receipt, false, TEXT("report_contract_invalid"));
    for (const FString& BadHash : { FString(TEXT("a")), FString::ChrN(64, TCHAR('z')) })
    {
        Receipt->SetObjectField(TEXT("language_profile"), MakeProfileIdentity(TEXT("gameplay-v1"), BadHash));
        Finalize(Receipt, false, TEXT("report_contract_invalid"));
    }
    Receipt->SetObjectField(TEXT("language_profile"), MakeProfileIdentity(TEXT(""), HashA));
    Finalize(Receipt, false, TEXT("report_contract_invalid"));
    Receipt->SetObjectField(TEXT("language_profile"), MakeProfileIdentity(TEXT("gameplay-v1"), HashA.ToUpper()));
    TestEqual(TEXT("Valid identity is retained with canonical hash"), Finalize(Receipt, true).LanguageProfileContractSha256, HashA);
    Config.LanguageProfile.Reset();
    Finalize(Receipt, false, TEXT("language_profile_receipt_mismatch"));
    Receipt->RemoveField(TEXT("language_profile"));
    Finalize(Receipt, true);
    Config.LanguageProfile = TEXT("gameplay-v1");
    Receipt->SetObjectField(TEXT("language_profile"), MakeProfileIdentity(TEXT("gameplay-v1"), HashA));
    const auto Before = Finalize(Receipt, true);
    const FString ReportHash = HashFile(Config.ReportPath);
    const FString ManifestHash = HashFile(Config.ManifestPath);
    const FString WasmHash = HashFile(WasmPath);
    FAvidScriptEditorCSharpBuildPlan Plan;
    FAvidScriptEditorCSharpBuildResult Prepared;
    if (!TestTrue(TEXT("Artifact transaction prepares"), FAvidScriptEditorCSharpBuildPipeline::Prepare(Config, Plan, Prepared))) return false;
    ON_SCOPE_EXIT { FAvidScriptEditorCSharpBuildPipeline::Cleanup(Plan); };
    // Controlled fault at the stage boundary; real two-stage success is covered below.
    Plan.bAutomaticBindingSlice = true;
    Plan.bBootstrapCompleted = true;
    Plan.BootstrapResult = Before;
    FAvidScriptEditorCSharpBuildResult Candidate = Before;
    Candidate.LanguageProfileContractSha256 = HashB;
    FFileHelper::SaveStringToFile(TEXT("candidate-report"), *Config.ReportPath);
    FFileHelper::SaveStringToFile(TEXT("candidate-manifest"), *Config.ManifestPath);
    FFileHelper::SaveStringToFile(TEXT("candidate-wasm"), *WasmPath);
    FAvidScriptEditorCSharpBuildResult Rejected;
    TestFalse(TEXT("Changed contract between stages rejects publication"), FAvidScriptEditorCSharpBuildPipeline::CompleteFinal(Plan, Candidate, Rejected));
    TestEqual(TEXT("Stage mismatch category"), Rejected.ErrorCategory, FString(TEXT("language_profile_changed_during_build")));
    TestEqual(TEXT("Previous report restored"), HashFile(Config.ReportPath), ReportHash);
    TestEqual(TEXT("Previous manifest restored"), HashFile(Config.ManifestPath), ManifestHash);
    TestEqual(TEXT("Previous WASM restored"), HashFile(WasmPath), WasmHash);
    TestFalse(TEXT("Rejected artifact transaction closes"), Plan.bArtifactTransactionActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGameplayProfileEditorWorkflowTest,
    "AvidScript.Editor.GameplayLanguageProfile.RealWorkspaceBuildAndReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGameplayProfileEditorWorkflowTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::EditorTests::GameplayProfile;
    FOwnedRoot Owned(*this);
    FAvidScriptEditorCSharpWorkspaceConfig Config;
    Config.WorkspaceRoot = Owned.Root / TEXT("Workspace");
    Config.GeneratedRoot = Owned.Root / TEXT("Generated");
    Config.BindingPackageRoot = Owned.Root / TEXT("Bindings");
    Config.OutputRoot = Owned.Root / TEXT("Output");
    FAvidScriptEditorCSharpWorkspaceResult Workspace;
    if (!TestTrue(TEXT("Production workspace creates"), FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(Config, Workspace)))
    { AddError(Workspace.ErrorCategory + TEXT(": ") + Workspace.ErrorMessage); return false; }
    auto Profile = ReadJson(Workspace.ProfilePath);
    if (!TestTrue(TEXT("New profile is structured JSON"), Profile.IsValid())) return false;
    TestEqual(TEXT("New workspace selects gameplay"), Profile->GetStringField(TEXT("language_profile")), FString(TEXT("gameplay-v1")));
    Profile->SetStringField(TEXT("semantic_cache_root"), Owned.Root / TEXT("SemanticCache"));
    Profile->SetStringField(TEXT("compilation_cache_root"), Owned.Root / TEXT("CompilationCache"));
    TestTrue(TEXT("Test-owned caches are explicit"), WriteJson(Workspace.ProfilePath, Profile.ToSharedRef()));
    FAvidScriptEditorCSharpProfileLoadResult Loaded;
    if (!TestTrue(TEXT("Workspace profile loads"), FAvidScriptEditorCSharpProfileService::LoadProfile(Workspace.ProfilePath, Loaded))) return false;
    FAvidScriptEditorCSharpBuildResult Starter;
    if (!TestTrue(TEXT("Unmodified starter uses the production build service"),
        FAvidScriptEditorCSharpBuildService::BuildProfile(FAvidScriptEditorCSharpProfileService::MakeBuildRequest(Loaded), Starter)))
    { AddError(Starter.ErrorCategory + TEXT(": ") + Starter.ErrorMessage + TEXT(" ") + Starter.Stdout + Starter.Stderr); return false; }
    TestEqual(TEXT("Starter receipt confirms requested profile"), Starter.LanguageProfile, FString(TEXT("gameplay-v1")));
    TestEqual(TEXT("Starter contract hash exists"), Starter.LanguageProfileContractSha256.Len(), 64);
    TestTrue(TEXT("Default artifact policy publishes precompiled bytes"), Starter.bVmArtifactPublished);
    UWorld* World = GEngine ? UWorld::CreateWorld(EWorldType::Game, false) : nullptr;
    if (!TestNotNull(TEXT("Game World creates"), World)) return false;
    ON_SCOPE_EXIT
    {
        if (World->HasBegunPlay()) World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    };
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    World->BeginPlay();
    World->SetBegunPlay(true);
    AActor* StarterActor = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("Starter target spawns"), StarterActor)) return false;
    auto* StarterRoot = NewObject<USceneComponent>(StarterActor);
    StarterActor->SetRootComponent(StarterRoot);
    StarterActor->AddInstanceComponent(StarterRoot);
    StarterRoot->RegisterComponent();
    FAvidScriptEditorComponentBindingResult StarterBinding;
    if (!TestTrue(TEXT("Original starter binds through production service"),
            FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(Workspace.ReportPath, StarterActor, StarterBinding))
        || !TestNotNull(TEXT("Starter component exists"), StarterBinding.Component)) return false;
    if (!TestTrue(TEXT("Starter Runtime loads"), StarterBinding.Component->GetRuntimeStats().bRuntimeLoaded))
    { AddError(StarterBinding.Component->GetRuntimeStats().LastErrorMessage); return false; }
    TestTrue(TEXT("Generated typed gameplay router executes through the outcome boundary"),
        StarterBinding.Component->DispatchScriptInput(1, 0, FVector(2, 3, 4)));
    TestTrue(TEXT("Starter input reaches the actual Session"), StarterBinding.Component->GetRuntimeStats().EventCallbackCount > 0);
    StarterActor->Destroy();
    World->Tick(LEVELTICK_All, 0.01f);
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
    if (!TestTrue(TEXT("Fixture plugin exists"), Plugin.IsValid())) return false;
    FString Business, Entry;
    if (!TestTrue(TEXT("Shared business source reads"), FFileHelper::LoadFileToString(Business, *(Plugin->GetBaseDir() / TEXT("Fixtures/Phase66/ObjectLoadCancellationFlow.cs"))))
        || !TestTrue(TEXT("Guest entry source reads"), FFileHelper::LoadFileToString(Entry, *(Plugin->GetBaseDir() / TEXT("Fixtures/Phase66/ObjectLoadCancellationFlow.Guest.cs"))))) return false;
    const FString Source = Business + TEXT("\n") + Entry;
    TestTrue(TEXT("Shared method bodies are copied unchanged"), FFileHelper::SaveStringToFile(Source, *Workspace.SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    FAvidScriptEditorCSharpBuildResult Cold;
    if (!TestTrue(TEXT("Object/token/error composition builds through normal Editor service"),
        FAvidScriptEditorCSharpBuildService::BuildProfile(FAvidScriptEditorCSharpProfileService::MakeBuildRequest(Loaded), Cold)))
    { AddError(Cold.ErrorCategory + TEXT(": ") + Cold.ErrorMessage + TEXT(" ") + Cold.Stdout + Cold.Stderr); return false; }
    TestEqual(TEXT("Both source workloads use the same contract"), Cold.LanguageProfileContractSha256, Starter.LanguageProfileContractSha256);
    const FString ColdWasmHash = HashFile(Workspace.OutputRoot / TEXT("project_gameplay.wasm"));
    TestEqual(TEXT("Cold canonical WASM hash exists"), ColdWasmHash.Len(), 64);
    auto Job = FAvidScriptEditorCSharpAsyncBuildJobFactory::Create();
    if (!TestTrue(TEXT("Production asynchronous job starts"), Job->Start(Workspace.ProfilePath))) return false;
    const double Deadline = FPlatformTime::Seconds() + 600;
    bool SawBootstrap = false, SawFinal = false;
    while (!Job->IsFinished() && FPlatformTime::Seconds() < Deadline)
    {
        SawBootstrap |= Job->GetProgress().Stage == EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning;
        SawFinal |= Job->GetProgress().Stage == EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning;
        Job->Tick();
        FPlatformProcess::Sleep(0.01f);
    }
    if (!Job->IsFinished()) { Job->Cancel(); AddError(TEXT("Asynchronous build timed out")); return false; }
    FAvidScriptEditorCSharpAsyncBuildResult Warm;
    if (!TestTrue(TEXT("Async result consumes once"), Job->ConsumeResult(Warm))) return false;
    TestFalse(TEXT("Async result cannot consume twice"), Job->ConsumeResult(Warm));
    if (!TestTrue(TEXT("Real asynchronous build succeeds"), Warm.bSucceeded))
    { AddError(Warm.ErrorCategory + TEXT(": ") + Warm.ErrorMessage + Warm.BuildResult.Stdout + Warm.BuildResult.Stderr); return false; }
    TestTrue(TEXT("Async job traverses both real compiler stages"), SawBootstrap && SawFinal);
    TestEqual(TEXT("Async profile is retained"), Warm.BuildResult.LanguageProfile, Cold.LanguageProfile);
    TestEqual(TEXT("Async contract is retained"), Warm.BuildResult.LanguageProfileContractSha256, Cold.LanguageProfileContractSha256);
    TestEqual(TEXT("Warm build reuses source analysis"), Warm.BuildResult.FrontendInvocationCount, 0);
    TestEqual(TEXT("Warm and cold modules are identical"), HashFile(Workspace.OutputRoot / TEXT("project_gameplay.wasm")), ColdWasmHash);
    Job.Reset();

    FAvidScriptRuntimeArtifact Artifact;
    FAvidScriptRuntimeArtifactLoadResult ArtifactResult;
    if (!TestTrue(TEXT("Published artifact verifies before execution"), FAvidScriptRuntimeArtifactLoader::LoadFromFile(Workspace.ManifestPath, Artifact, ArtifactResult)))
    { AddError(ArtifactResult.CanonicalResult.ErrorMessage); return false; }
    FAvidScriptFrontendReport Report;
    FAvidScriptFrontendReportLoadResult ReportLoad;
    if (!TestTrue(TEXT("Actual report reads"), FAvidScriptFrontendReportReader::LoadFromFile(Workspace.ReportPath, Report, ReportLoad))) return false;
    TestEqual(TEXT("Formal report retains unchanged source bytes"), Report.SourceSha256, HashFile(Workspace.SourcePath));
    TestEqual(TEXT("Executed bytes match compiler receipt"), HashFile(Workspace.OutputRoot / TEXT("project_gameplay.wasm")), Artifact.Manifest.WasmSha256);
    // The formal report stores project-relative paths, independent of the
    // Editor process working directory.
    const FString GuestIrPath = FPaths::IsRelative(Report.GuestIrArtifact)
        ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Report.GuestIrArtifact)
        : Report.GuestIrArtifact;
    if (!TestTrue(TEXT("Readback layout stays inside the owned workspace"), FPaths::IsUnderDirectory(GuestIrPath, Owned.Root))
        || !TestEqual(TEXT("Readback layout is the executed IR"), HashFile(GuestIrPath), Artifact.Manifest.DebugProvenance.GuestIrSha256)) return false;
    AActor* Actor = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("Real target Actor spawns"), Actor)) return false;
    auto* Root = NewObject<USceneComponent>(Actor);
    Actor->SetRootComponent(Root);
    Actor->AddInstanceComponent(Root);
    Root->RegisterComponent();
    FAvidScriptEditorComponentBindingResult Binding;
    if (!TestTrue(TEXT("Production binding service attaches real module"), FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(Workspace.ReportPath, Actor, Binding)))
    { AddError(Binding.ErrorCategory + TEXT(": ") + Binding.ErrorMessage); return false; }
    if (!TestNotNull(TEXT("Production component exists"), Binding.Component)) return false;
    if (!TestTrue(TEXT("Production component loads actual Runtime"), Binding.Component->GetRuntimeStats().bRuntimeLoaded)
        || !TestNotNull(TEXT("Production Session exists"), Binding.Component->GetRuntimeSessionForTesting()))
    { AddError(Binding.Component->GetRuntimeStats().LastErrorMessage); return false; }
    TestTrue(TEXT("Component executes BeginPlay"), Binding.Component->GetRuntimeStats().bBeginPlayCalled);
    if (!TestTrue(TEXT("Actual streamed object completes shared business result"), FinishObjectLoad(*this, *World, *Binding.Component, GuestIrPath, 17))) return false;
    TestEqual(TEXT("Object work leaves no pending waits"), Binding.Component->GetRuntimeSessionForTesting()->GetLivePendingContinuationCount(), 0);
    auto Watch = MakeUnique<FControlledWatch>();
    auto* WatchPtr = Watch.Get();
    FAvidScriptEditorCSharpLiveReloadService Service(MoveTemp(Watch),
        [] { return FAvidScriptEditorCSharpAsyncBuildJobFactory::Create(); },
        [](const FString& Path, AActor* Target, FAvidScriptEditorComponentBindingResult& Result)
        { return FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(Path, Target, Result); },
        [] { return FPlatformTime::Seconds(); });
    ON_SCOPE_EXIT { Service.Stop(); };
    FAvidScriptEditorCSharpLiveReloadServiceConfig ReloadConfig;
    ReloadConfig.WorkspaceRoot = Workspace.WorkspaceRoot;
    ReloadConfig.ProfilePath = Workspace.ProfilePath;
    ReloadConfig.DebounceSeconds = 0;
    FAvidScriptEditorCSharpLiveReloadServiceResult Started;
    if (!TestTrue(TEXT("Live reload starts with real job and binder"), Service.Start(ReloadConfig, Actor, Started))) return false;
    // Only the entry wrapper changes; the same shared business methods remain intact.
    const FString ChangedEntry = Entry.Replace(TEXT("ObjectCancellationScript.Result = result;"), TEXT("ObjectCancellationScript.Result = result + 1;"));
    TestTrue(TEXT("Reload candidate changes executable entry"), ChangedEntry != Entry);
    TestTrue(TEXT("Reload candidate writes"), FFileHelper::SaveStringToFile(Business + TEXT("\n") + ChangedEntry, *Workspace.SourcePath));
    WatchPtr->Emit(Workspace.SourcePath);
    if (!TestTrue(TEXT("Real live reload completes"), WaitForReload(Service, 1))) return false;
    const auto Reloaded = Service.GetLastResult();
    if (!TestEqual(TEXT("Live reload publishes candidate"), Reloaded.Status, EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildSucceeded))
    { AddError(Reloaded.ErrorCategory + TEXT(": ") + Reloaded.ErrorMessage); return false; }
    TestEqual(TEXT("Live reload uses the same profile"), Reloaded.BuildResult.BuildResult.LanguageProfileContractSha256, Cold.LanguageProfileContractSha256);
    TestTrue(TEXT("Live reload actually applies to existing component"), Reloaded.BuildResult.BindingResult.bReloadApplied);
    if (!TestTrue(TEXT("Reloaded artifact verifies"), FAvidScriptRuntimeArtifactLoader::LoadFromFile(Workspace.ManifestPath, Artifact, ArtifactResult))
        || !TestEqual(TEXT("Reload readback layout matches the new executable"), HashFile(GuestIrPath), Artifact.Manifest.DebugProvenance.GuestIrSha256)
        || !TestTrue(TEXT("New entry executes with real streamed object"), FinishObjectLoad(*this, *World, *Binding.Component, GuestIrPath, 18))) return false;
    TestTrue(TEXT("Changed code produces changed WASM"), HashFile(Workspace.OutputRoot / TEXT("project_gameplay.wasm")) != ColdWasmHash);
    const FString PublishedReport = HashFile(Workspace.ReportPath);
    const FString PublishedManifest = HashFile(Workspace.ManifestPath);
    const FString PublishedWasm = HashFile(Workspace.OutputRoot / TEXT("project_gameplay.wasm"));
    const auto* PreviousRuntime = Binding.Component->GetRuntimeSessionForTesting()->GetLiveRuntimeForTesting();
    TestTrue(TEXT("Natural compiler failure candidate writes"), FFileHelper::SaveStringToFile(Business + TEXT("\n") + ChangedEntry
        + TEXT("\n#error gameplay candidate deliberately rejected\n"), *Workspace.SourcePath));
    WatchPtr->Emit(Workspace.SourcePath);
    if (!TestTrue(TEXT("Failed live reload completes"), WaitForReload(Service, 2))) return false;
    TestEqual(TEXT("Compiler error is reported"), Service.GetLastResult().Status, EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed);
    TestEqual(TEXT("Compiler failure preserves committed report"), HashFile(Workspace.ReportPath), PublishedReport);
    TestEqual(TEXT("Compiler failure preserves committed manifest"), HashFile(Workspace.ManifestPath), PublishedManifest);
    TestEqual(TEXT("Compiler failure preserves committed WASM"), HashFile(Workspace.OutputRoot / TEXT("project_gameplay.wasm")), PublishedWasm);
    TestTrue(TEXT("Compiler failure preserves the actual live runtime"), Binding.Component->GetRuntimeSessionForTesting()->GetLiveRuntimeForTesting() == PreviousRuntime);
    TestEqual(TEXT("Old behavior remains readable after compiler failure"), ReadResult(*this, *Binding.Component, GuestIrPath), 18);
    Service.Stop();
    Profile->SetNumberField(TEXT("schema_version"), 10);
    Profile->RemoveField(TEXT("language_profile"));
    TestTrue(TEXT("Existing workspace legacy profile writes"), WriteJson(Workspace.ProfilePath, Profile.ToSharedRef()));
    const FString LegacyProfileHash = HashFile(Workspace.ProfilePath);
    FAvidScriptEditorCSharpWorkspaceResult Refreshed;
    TestTrue(TEXT("Workspace refresh succeeds with existing legacy profile"), FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(Config, Refreshed));
    TestEqual(TEXT("Workspace refresh preserves legacy profile bytes"), HashFile(Workspace.ProfilePath), LegacyProfileHash);
    FAvidScriptEditorCSharpProfileLoadResult Preserved;
    TestTrue(TEXT("Preserved legacy workspace remains loadable"), FAvidScriptEditorCSharpProfileService::LoadProfile(Workspace.ProfilePath, Preserved));
    TestTrue(TEXT("Workspace refresh does not silently upgrade language"), Preserved.BuildConfig.LanguageProfile.IsEmpty());
    AddInfo(FString::Printf(TEXT("Gameplay Editor workflow: profile=%s contract=%s cold_wasm=%s published_wasm=%s"),
        *Cold.LanguageProfile, *Cold.LanguageProfileContractSha256, *ColdWasmHash, *PublishedWasm));
    return true;
}

#endif
