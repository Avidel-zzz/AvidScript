#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectGlobals.h"

namespace
{
constexpr const TCHAR* DynamicProjectileClassPath =
	TEXT("/Game/Variant_TwinStick/Blueprints/BP_TwinStickProjectile.BP_TwinStickProjectile_C");
constexpr const TCHAR* DynamicProjectileBaseClassPath =
	TEXT("/Script/AvidTPSTemplate.TwinStickProjectile");

FString GetAvidScriptDynamicProjectilePluginPath(const TCHAR* RelativePath)
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript"),
		RelativePath));
}

bool CreateAvidScriptDynamicProjectileWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptDynamicProjectileIntegrationWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	OutWorld->InitializeActorsForPlay(FURL());
	OutWorld->BeginPlay();
	OutWorld->SetBegunPlay(true);
	return true;
}

void DestroyAvidScriptDynamicProjectileWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}

	if (World->HasBegunPlay())
	{
		World->EndPlay(EEndPlayReason::Quit);
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

AActor* FindAvidScriptDynamicProjectile(UWorld& World, const UClass& ProjectileClass)
{
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		AActor* Actor = *It;
		if (IsValid(Actor) && Actor->GetClass() == &ProjectileClass)
		{
			return Actor;
		}
	}
	return nullptr;
}

bool HasAvidScriptRequiredImport(
	const FAvidScriptWasmReloadManifest& Manifest,
	const TCHAR* ModuleName,
	const TCHAR* ImportName)
{
	return Manifest.RequiredImports.ContainsByPredicate(
		[ModuleName, ImportName](const FAvidScriptWasmRequiredImport& Import)
		{
			return Import.ModuleName == ModuleName && Import.ImportName == ImportName;
		});
}

bool BuildAvidScriptDynamicProjectileCandidate(
	const FAvidScriptEditorCSharpBuildRequest& BaseRequest,
	const FString& SourcePath,
	const FString& OutputRoot,
	const FString& ArtifactStem,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	FAvidScriptEditorCSharpBuildRequest Request = BaseRequest;
	Request.Config.SourcePath = SourcePath;
	Request.Config.OutputRoot = OutputRoot;
	Request.Config.ArtifactStem = ArtifactStem;
	Request.Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
		OutputRoot,
		ArtifactStem);
	Request.Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
		OutputRoot,
		ArtifactStem);
	Request.Config.SemanticCacheRoot = FPaths::Combine(OutputRoot, TEXT("SemanticCache/v1"));
	return FAvidScriptEditorCSharpBuildService::BuildProfile(Request, OutResult);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorDynamicProjectileIntegrationTest,
	"AvidScript.Editor.DynamicProjectile.GameplayLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorDynamicProjectileIntegrationTest::RunTest(const FString& Parameters)
{
	const FString ProfilePath = GetAvidScriptDynamicProjectilePluginPath(
		TEXT("Samples/CSharp/DynamicProjectile/DynamicProjectile.csharp-profile.json"));
	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!TestTrue(
			TEXT("Dynamic projectile project binding profile loads"),
			FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, ProfileResult)))
	{
		AddError(ProfileResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Dynamic projectile profile uses schema v2"), ProfileResult.SchemaVersion, 2);
	TestFalse(TEXT("Dynamic projectile profile does not use EngineGameplay"), ProfileResult.bUsesEngineGameplayBindingProfile);
	TestEqual(TEXT("Dynamic projectile profile resolves one class reference"), ProfileResult.ResolvedClassReferences.Num(), 1);
	if (ProfileResult.ResolvedClassReferences.Num() != 1)
	{
		return false;
	}
	const FAvidScriptProjectBindingClassSpec& ClassReference = ProfileResult.ResolvedClassReferences[0];
	TestEqual(TEXT("Dynamic projectile class reference script name"), ClassReference.ScriptName, FString(TEXT("TwinStickProjectile")));
	TestEqual(TEXT("Dynamic projectile class reference path"), ClassReference.ClassPath, FString(DynamicProjectileClassPath));
	TestEqual(TEXT("Dynamic projectile class reference base"), ClassReference.BaseClassPath, FString(DynamicProjectileBaseClassPath));

	UClass* ProjectileClass = LoadObject<UClass>(nullptr, DynamicProjectileClassPath);
	UClass* ProjectileBaseClass = LoadObject<UClass>(nullptr, DynamicProjectileBaseClassPath);
	if (!TestNotNull(TEXT("Real TwinStick projectile Blueprint class loads"), ProjectileClass)
		|| !TestNotNull(TEXT("Native TwinStick projectile base class loads"), ProjectileBaseClass))
	{
		return false;
	}
	TestTrue(TEXT("Blueprint projectile satisfies the profile base constraint"), ProjectileClass->IsChildOf(ProjectileBaseClass));
	TestFalse(TEXT("Blueprint projectile is spawnable"), ProjectileClass->HasAnyClassFlags(CLASS_Abstract));

	const FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P49_4/DynamicProjectile")));
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	IFileManager::Get().MakeDirectory(*TestRoot, true);

	const FAvidScriptEditorCSharpBuildRequest BaseRequest =
		FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ProfileResult);
	FAvidScriptEditorCSharpBuildResult MainBuildResult;
	if (!TestTrue(
			TEXT("Dynamic projectile C# source builds through the project profile"),
			BuildAvidScriptDynamicProjectileCandidate(
				BaseRequest,
				ProfileResult.BuildConfig.SourcePath,
				FPaths::Combine(TestRoot, TEXT("Main")),
				TEXT("dynamic_projectile"),
				MainBuildResult)))
	{
		AddError(
			MainBuildResult.ErrorMessage
			+ TEXT("\nstdout:\n") + MainBuildResult.Stdout
			+ TEXT("\nstderr:\n") + MainBuildResult.Stderr);
		return false;
	}

	FAvidScriptEditorCSharpBuildResult CandidateBuildResult;
	if (!TestTrue(
			TEXT("Rejected reload C# candidate builds with the same module identity"),
			BuildAvidScriptDynamicProjectileCandidate(
				BaseRequest,
				GetAvidScriptDynamicProjectilePluginPath(
					TEXT("Tests/Fixtures/CSharp/P49_4_DynamicProjectileRejectedReload.cs")),
				FPaths::Combine(TestRoot, TEXT("RejectedReload")),
				TEXT("dynamic_projectile_rejected_reload"),
				CandidateBuildResult)))
	{
		AddError(
			CandidateBuildResult.ErrorMessage
			+ TEXT("\nstdout:\n") + CandidateBuildResult.Stdout
			+ TEXT("\nstderr:\n") + CandidateBuildResult.Stderr);
		return false;
	}

	FAvidScriptWasmReloadManifest MainManifest;
	FAvidScriptWasmReloadManifest CandidateManifest;
	TArray<uint8> MainBytecode;
	TArray<uint8> CandidateBytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!TestTrue(
			TEXT("Dynamic projectile manifest loads its WASM and runtime package"),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				MainBuildResult.ManifestPath,
				MainManifest,
				MainBytecode,
				ManifestLoadResult)))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	if (!TestTrue(
			TEXT("Rejected reload manifest loads its WASM and runtime package"),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				CandidateBuildResult.ManifestPath,
				CandidateManifest,
				CandidateBytecode,
				ManifestLoadResult)))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Reload candidate preserves the logical module id"), CandidateManifest.ModuleId, MainManifest.ModuleId);
	if (!TestTrue(TEXT("Main manifest owns a runtime binding package"), MainManifest.BindingPackage.IsValid()))
	{
		return false;
	}
	TestFalse(
		TEXT("Dynamic projectile script does not import packed owner access"),
		HasAvidScriptRequiredImport(
			MainManifest,
			TEXT("avidscript"),
			TEXT("avid_owner_get_handle")));
	TestNotNull(
		TEXT("Runtime package may retain authorized Self capability as a superset"),
		MainManifest.BindingPackage->GetExpectedSelfClass());
	TestEqual(TEXT("Runtime package caches one class reference"), MainManifest.BindingPackage->GetClassReferenceCount(), 1);
	UClass* CachedProjectileClass = nullptr;
	UClass* CachedBaseClass = nullptr;
	if (!TestTrue(
			TEXT("Runtime package resolves the immutable projectile class ordinal"),
			MainManifest.BindingPackage->TryResolveClassReference(0, CachedProjectileClass, CachedBaseClass)))
	{
		return false;
	}
	TestTrue(TEXT("Cached projectile class is the real Blueprint"), CachedProjectileClass == ProjectileClass);
	TestTrue(TEXT("Cached projectile base is the native class"), CachedBaseClass == ProjectileBaseClass);
	TestTrue(
		TEXT("Main WASM reaches SpawnActor"),
		HasAvidScriptRequiredImport(MainManifest, TEXT("avidscript"), TEXT("avid_object_spawn_actor")));
	TestTrue(
		TEXT("Main WASM reaches DestroyActor"),
		HasAvidScriptRequiredImport(MainManifest, TEXT("avidscript"), TEXT("avid_object_destroy_actor")));
	TestTrue(
		TEXT("Main WASM reaches IsA"),
		HasAvidScriptRequiredImport(MainManifest, TEXT("avidscript"), TEXT("avid_object_is_a")));
	TestTrue(
		TEXT("Main WASM reaches the timer service"),
		HasAvidScriptRequiredImport(MainManifest, TEXT("env"), TEXT("timer_set_once")));
	int32 ReflectedImportCount = 0;
	for (const FAvidScriptWasmRequiredImport& Import : MainManifest.RequiredImports)
	{
		if (Import.ModuleName == TEXT("avidscript")
			&& Import.ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
		{
			++ReflectedImportCount;
		}
	}
	TestEqual(TEXT("Main WASM reaches two ordinary generated Actor calls"), ReflectedImportCount, 2);

	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Dynamic projectile integration World starts"), CreateAvidScriptDynamicProjectileWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptDynamicProjectileWorld(World);
	};

	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Runtime owner Actor spawns"), Owner))
	{
		return false;
	}
	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult OwnerRegisterResult;
	const FAvidScriptObjectHandle OwnerHandle = Registry.RegisterObject(Owner, OwnerRegisterResult);
	if (!TestTrue(TEXT("Runtime owner registers"), OwnerRegisterResult.bSucceeded))
	{
		return false;
	}

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = OwnerHandle;
	HostContext.World = World;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptRuntimeSession Session;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	if (!TestTrue(
			TEXT("Dynamic projectile runtime enters BeginPlay through WAMR"),
			Session.LoadInitialModule(
				MainBytecode.GetData(),
				MainBytecode.Num(),
				MainManifest,
				ReloadResult)))
	{
		AddError(ReloadResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("BeginPlay schedules the Spawn timer"), Session.GetLivePendingTimerCount(), 1);
	TestNull(TEXT("BeginPlay does not directly mutate the World"), FindAvidScriptDynamicProjectile(*World, *ProjectileClass));

	FAvidScriptWasmSmokeResult TickResult;
	if (!TestTrue(TEXT("First live Tick fires the Spawn timer"), Session.TickLive(0.02f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	AActor* Projectile = FindAvidScriptDynamicProjectile(*World, *ProjectileClass);
	if (!TestNotNull(TEXT("C# timer spawns the real Blueprint projectile"), Projectile))
	{
		return false;
	}
	TestTrue(
		TEXT("SpawnActor preserves the guest FTransform location"),
		Projectile->GetActorLocation().Equals(FVector(300.0, 0.0, 150.0), 0.01));
	TestTrue(
		TEXT("SpawnActor starts from unit scale"),
		Projectile->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.001));
	TestEqual(TEXT("Spawn and owner handles are both live"), Registry.GetLiveHandleCount(), 2);

	FAvidScriptObjectHandleResult ProjectileRegisterResult;
	const FAvidScriptObjectHandle ProjectileHandle = Registry.RegisterObject(
		Projectile,
		ProjectileRegisterResult);
	TestTrue(TEXT("Spawned projectile handle is observable from the registry"), ProjectileRegisterResult.bSucceeded);
	TestEqual(TEXT("Duplicate registration preserves the live handle count"), Registry.GetLiveHandleCount(), 2);

	if (!TestTrue(TEXT("Second live Tick invokes ordinary reflected Actor APIs"), Session.TickLive(0.1f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Generated SetActorScale3D mutates the spawned Blueprint"),
		Projectile->GetActorScale3D().Equals(FVector(1.1, 1.1, 1.1), 0.01));
	const FVector ScaleBeforeRejectedReload = Projectile->GetActorScale3D();

	const int32 LiveHandlesBeforeRejectedReload = Registry.GetLiveHandleCount();
	TestFalse(
		TEXT("Candidate BeginPlay Spawn is rejected inside the reload transaction"),
		Session.ReloadModule(
			CandidateBytecode.GetData(),
			CandidateBytecode.Num(),
			CandidateManifest,
			ReloadResult));
	TestTrue(TEXT("Rejected candidate opens a host effect transaction"), ReloadResult.bHostEffectTransactionAttempted);
	TestTrue(TEXT("Rejected candidate rolls back its empty transaction"), ReloadResult.bHostEffectRollbackAttempted);
	TestTrue(TEXT("Rejected candidate preserves the old live runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestTrue(
		TEXT("Rejected candidate reports the non-transactional lifecycle category"),
		ReloadResult.ErrorMessage.Contains(TEXT("binding_reload_effect_unsupported"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("Rejected candidate does not register another Actor"), Registry.GetLiveHandleCount(), LiveHandlesBeforeRejectedReload);
	TestTrue(TEXT("Original projectile remains live after candidate rejection"), IsValid(Projectile));
	TestTrue(
		TEXT("Original projectile scale is unchanged by candidate BeginPlay"),
		Projectile->GetActorScale3D().Equals(ScaleBeforeRejectedReload, 0.001));

	if (!TestTrue(TEXT("Old runtime continues ticking after candidate rejection"), Session.TickLive(0.1f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Old runtime continues ordinary reflected gameplay"),
		Projectile->GetActorScale3D().Equals(FVector(1.2, 1.2, 1.2), 0.01));

	if (!TestTrue(TEXT("Old runtime fires its Destroy timer"), Session.TickLive(0.4f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("DestroyActor marks the spawned projectile for destruction"), !IsValid(Projectile) || Projectile->IsActorBeingDestroyed());
	TestEqual(TEXT("DestroyActor releases only the projectile handle"), Registry.GetLiveHandleCount(), 1);
	FAvidScriptObjectHandleResult StaleResolveResult;
	TestNull(TEXT("Released projectile handle no longer resolves"), Registry.ResolveObject(ProjectileHandle, StaleResolveResult));
	TestEqual(TEXT("Released projectile handle rejects its stale generation"), StaleResolveResult.ErrorCategory, FString(TEXT("generation_mismatch")));
	TestTrue(TEXT("Runtime owner remains live after projectile destruction"), IsValid(Owner));

	FAvidScriptWasmSmokeResult StopResult;
	if (!TestTrue(TEXT("Dynamic projectile runtime stops cleanly"), Session.StopAndUnload(StopResult)))
	{
		AddError(StopResult.ErrorMessage);
		return false;
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
