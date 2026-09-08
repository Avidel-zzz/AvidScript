#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorCSharpBindingEmitterTestTypes.h"
#include "AvidScriptHash.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildInvoker.h"
#include "CSharpBuild/AvidScriptEditorVmArtifactPublisher.h"
#include "Packages/AvidScriptModulePackage.h"

#include "Dom/JsonObject.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FString NormalizeAvidScriptCSharpBuildTestPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool LoadAvidScriptCSharpBuildTestJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *Path))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

FString QuoteAvidScriptPowerShellLiteral(FString Value)
{
	Value.ReplaceInline(TEXT("'"), TEXT("''"), ESearchCase::CaseSensitive);
	return TEXT("'") + Value + TEXT("'");
}

bool CreateAvidScriptCSharpPackageTestOwner(
	UWorld*& OutWorld,
	AActor*& OutOwner)
{
	OutWorld = nullptr;
	OutOwner = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	const FName WorldName(*(
		TEXT("AvidScriptCSharpPackageWorld_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext =
		GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	OutOwner = OutWorld->SpawnActor<AActor>();
	if (OutOwner == nullptr)
	{
		return false;
	}
	USceneComponent* const RootComponent =
		NewObject<USceneComponent>(OutOwner, TEXT("AvidScriptRoot"));
	OutOwner->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();
	return true;
}

void DestroyAvidScriptCSharpPackageTestOwner(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

FString MakeAvidScriptZeroBindingLifecycleSource()
{
	return TEXT(
		"using System.Runtime.InteropServices;\n"
		"\n"
		"namespace AvidScript;\n"
		"\n"
		"public static class ZeroBindingLifecycleScript\n"
		"{\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")]\n"
		"    public static void BeginPlay() {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_tick\")]\n"
		"    public static void Tick(float deltaSeconds) {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_end_play\")]\n"
		"    public static void EndPlay() {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_timer\")]\n"
		"    public static void OnTimer(int callbackId, int timerHandle) {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_event\")]\n"
		"    public static void OnEvent(int eventId, float value) {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_gameplay_event\")]\n"
		"    public static void OnGameplayEvent(\n"
		"        int eventType, int primaryId, int secondaryId, int objectSlot,\n"
		"        int objectGeneration, float x, float y, float z) {}\n"
		"}\n");
}

FString MakeAvidScriptGeneratedContainerFacadeSource()
{
	return TEXT(
		"using System.Runtime.InteropServices;\n"
		"\n"
		"namespace AvidScript;\n"
		"\n"
		"public static class GeneratedContainerFacadeScript\n"
		"{\n"
		"    private static int LastScore;\n"
		"\n"
		"    public static int Main() => LastScore;\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")]\n"
		"    public static void BeginPlay()\n"
		"    {\n"
		"        UAvidScriptCSharpBindingEmitterTestObject owner =\n"
		"            UAvidScriptCSharpBindingEmitterTestObject.TryCast(UE.Self);\n"
		"        FAvidArray<string> array = owner.ReadableStringArray;\n"
		"        FAvidSet<int> set = owner.ReadableIntSet;\n"
		"        FAvidMap<string, string> map = owner.ReadableNameStringMap;\n"
		"        string arrayValue;\n"
		"        string replacement = \"updated\";\n"
		"        int setValue = 7;\n"
		"        string mapKey = \"key\";\n"
		"        string mapValue = \"value\";\n"
		"        int score = 0;\n"
		"        if (AvidScriptContainer.TryGet(array, 0, out arrayValue)) score++;\n"
		"        if (AvidScriptContainer.TrySet(array, 0, in replacement)) score++;\n"
		"        if (AvidScriptContainer.Contains(set, in setValue)) score++;\n"
		"        if (AvidScriptContainer.Add(set, in setValue)) score++;\n"
		"        if (AvidScriptContainer.Remove(set, in setValue)) score++;\n"
		"        if (AvidScriptContainer.ContainsKey(map, in mapKey)) score++;\n"
		"        if (AvidScriptContainer.Set(map, in mapKey, in mapValue)) score++;\n"
		"        if (AvidScriptContainer.Remove(map, in mapKey)) score++;\n"
		"        LastScore = score;\n"
		"    }\n"
		"}\n");
}

bool AvidScriptCSharpBuildTestUsedImportsContain(
	const TSharedPtr<FJsonObject>& PackageObject,
	const TArray<FString>& ExpectedStableIds)
{
	const TArray<TSharedPtr<FJsonValue>>* UsedImports = nullptr;
	if (!PackageObject.IsValid()
		|| !PackageObject->TryGetArrayField(TEXT("used_imports"), UsedImports)
		|| UsedImports == nullptr)
	{
		return false;
	}

	TSet<FString> ActualStableIds;
	for (const TSharedPtr<FJsonValue>& UsedImportValue : *UsedImports)
	{
		const TSharedPtr<FJsonObject> UsedImport = UsedImportValue.IsValid()
			? UsedImportValue->AsObject()
			: nullptr;
		FString StableId;
		if (UsedImport.IsValid()
			&& UsedImport->TryGetStringField(TEXT("stable_id"), StableId))
		{
			ActualStableIds.Add(MoveTemp(StableId));
		}
	}

	for (const FString& ExpectedStableId : ExpectedStableIds)
	{
		if (!ActualStableIds.Contains(ExpectedStableId))
		{
			return false;
		}
	}
	return true;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildServiceCustomProfileTest,
	"AvidScript.Editor.CSharpBuildService.CustomProfileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceCustomProfileTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests"),
		TEXT("CSharpProfiles"),
		TEXT("CustomMover")));
	TestTrue(TEXT("Custom C# profile test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString SourcePath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(TestRoot, TEXT("CustomMoverScript.cs")));
	const FString GeneratedLifecycleSamplePath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Samples/CSharp/GeneratedBindingLifecycle/GeneratedBindingLifecycleScript.cs")));
	FString SourceText;
	if (!TestTrue(
		TEXT("Generated binding lifecycle sample can be read"),
		FFileHelper::LoadFileToString(SourceText, *GeneratedLifecycleSamplePath)))
	{
		return false;
	}
	const int32 ClosingBraceIndex = SourceText.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (!TestTrue(TEXT("Generated binding lifecycle sample has a closing type brace"), ClosingBraceIndex != INDEX_NONE))
	{
		return false;
	}
	SourceText = SourceText.Left(ClosingBraceIndex)
		+ TEXT("    private static void UnreachableBindingHelper()\n    {\n        _ = UE.Self.GetActorLocation();\n    }\n")
		+ SourceText.Mid(ClosingBraceIndex);
	TestTrue(TEXT("Custom C# source can be written"), FFileHelper::SaveStringToFile(SourceText, *SourcePath));

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = SourcePath;
	Config.ModuleId = TEXT("test_cooperative_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Config.ArtifactStem = TEXT("custom_mover");
	Config.OutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("CustomMover")));
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.SemanticCacheRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P43_5/CustomMover/CSharpSemanticCache/v1")));
	Config.CompilationCacheRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P61/A1/CustomMover/CSharpCompilationCache/v1")));
	Config.bEnableCooperativeSafepoints = true;
	Config.CooperativeSafepointInterval = 64;
	IFileManager::Get().DeleteDirectory(*Config.SemanticCacheRoot, false, true);
	IFileManager::Get().DeleteDirectory(*Config.CompilationCacheRoot, false, true);

	FAvidScriptEditorCSharpBuildResult BuildResult;
	const bool bBuildSucceeded = FAvidScriptEditorCSharpBuildService::BuildProfile(Config, BuildResult);
	TestTrue(
		TEXT("Custom C# profile automatically publishes the engine gameplay bindings"),
		bBuildSucceeded);
	TestTrue(TEXT("Custom C# profile build result succeeds"), BuildResult.bSucceeded);
	TestEqual(TEXT("Custom C# profile process exit code"), BuildResult.ProcessExitCode, 0);
	TestEqual(TEXT("Automatic custom C# profile performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Automatic custom C# profile reuses prepared Frontend"), BuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Automatic custom C# profile reuses prepared Semantic"), BuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Automatic custom C# profile runs Guest IR twice"), BuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Automatic custom C# profile runs WASM backend twice"), BuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Cold custom C# profile records a cache miss"), BuildResult.SemanticCacheLookup, FString(TEXT("miss")));
	TestFalse(TEXT("Cold custom C# profile records a semantic cache key"), BuildResult.SemanticCacheKey.IsEmpty());
	TestTrue(TEXT("Cold custom C# profile publishes a semantic cache entry"), BuildResult.bSemanticCachePublished);
	TestEqual(TEXT("Cooperative custom C# profile bypasses the compilation cache"), BuildResult.CompilationCacheLookup, FString(TEXT("disabled")));
	TestFalse(TEXT("Cooperative custom C# profile does not publish a compilation cache entry"), BuildResult.bCompilationCachePublished);
	TestTrue(
		TEXT("Custom C# profile records an authorization binding package manifest"),
		FPaths::FileExists(BuildResult.AuthorizationBindingPackagePath));
	TestTrue(
		TEXT("Custom C# profile records a runtime binding package manifest"),
		FPaths::FileExists(BuildResult.BindingPackagePath));
	TestNotEqual(
		TEXT("Automatic custom C# profile separates authorization and runtime packages"),
		BuildResult.AuthorizationBindingPackagePath,
		BuildResult.BindingPackagePath);
	TestTrue(TEXT("Custom C# profile report exists"), FPaths::FileExists(Config.ReportPath));
	TestTrue(TEXT("Custom C# profile manifest exists"), FPaths::FileExists(Config.ManifestPath));
	TestTrue(
		TEXT("Custom C# profile publishes formal WASM"),
		FPaths::FileExists(FPaths::Combine(Config.OutputRoot, TEXT("custom_mover.wasm"))));
	TestTrue(
		TEXT("Custom C# profile publishes a Wasmtime artifact"),
		BuildResult.bVmArtifactPublished);
	TestTrue(
		TEXT("Custom C# profile Wasmtime artifact exists"),
		FPaths::FileExists(BuildResult.VmArtifactPath));
	TestEqual(
		TEXT("Custom C# profile records the serialized format"),
		BuildResult.VmArtifactFormat,
		FString(TEXT("wasmtime_serialized_v1")));
	TestEqual(
		TEXT("Custom C# profile records an execution SHA-256"),
		BuildResult.VmArtifactSha256.Len(),
		64);
	TestEqual(
		TEXT("Custom C# profile records a session attestation"),
		BuildResult.VmArtifactAttestationId.Len(),
		32);
	TestEqual(
		TEXT("Custom C# profile selects the precompiled backend"),
		BuildResult.VmArtifactSelectedBackend,
		FString(TEXT("wasmtime.cranelift.precompiled")));
	TestTrue(
		TEXT("Custom C# profile publishes cooperative safepoints"),
		BuildResult.bVmArtifactCooperativeSafepoints);
	TestTrue(
		TEXT("Custom C# profile selects epoch-free Wasmtime codegen"),
		BuildResult.VmArtifactCompilerBuildIdentity.Contains(
			TEXT(";epoch_interruption=off;"),
			ESearchCase::CaseSensitive));

	TSharedPtr<FJsonObject> ReportObject;
	TestTrue(TEXT("Custom C# profile report is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ReportPath, ReportObject));
	if (!ReportObject.IsValid())
	{
		return true;
	}

	TestEqual(
		TEXT("Custom report declares direct ABI success"),
		ReportObject->GetStringField(TEXT("result")),
		FString(TEXT("direct_abi_built")));
	TestTrue(TEXT("Custom report records success"), ReportObject->GetBoolField(TEXT("succeeded")));
	const TSharedPtr<FJsonObject>* BindingAuthorizationObject = nullptr;
	if (!TestTrue(
		TEXT("Custom report contains binding authorization provenance"),
		ReportObject->TryGetObjectField(TEXT("binding_authorization"), BindingAuthorizationObject))
		|| BindingAuthorizationObject == nullptr
		|| !BindingAuthorizationObject->IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("Custom report keeps the gameplay profile and shared capabilities as its authorization ceiling"),
		static_cast<int32>((*BindingAuthorizationObject)->GetIntegerField(TEXT("profile_import_count"))),
		390);
	TestEqual(
		TEXT("Custom authorization records five reflected bindings and packed owner access"),
		static_cast<int32>((*BindingAuthorizationObject)->GetIntegerField(TEXT("used_import_count"))),
		6);
	TestEqual(
		TEXT("Custom authorization exposes six used stable identities"),
		(*BindingAuthorizationObject)->GetArrayField(TEXT("used_imports")).Num(),
		6);
	const TSharedPtr<FJsonObject>* BindingPackageObject = nullptr;
	if (!TestTrue(
		TEXT("Custom report contains binding package provenance"),
		ReportObject->TryGetObjectField(TEXT("binding_package"), BindingPackageObject))
		|| BindingPackageObject == nullptr
		|| !BindingPackageObject->IsValid())
	{
		return false;
	}
	TestNotEqual(
		TEXT("Custom report separates authorization and runtime manifests"),
		(*BindingAuthorizationObject)->GetStringField(TEXT("manifest_file")),
		(*BindingPackageObject)->GetStringField(TEXT("manifest_file")));
	TestTrue(
		TEXT("Custom report marks generated bindings required"),
		(*BindingPackageObject)->GetBoolField(TEXT("required")));
	TestEqual(
		TEXT("Custom report records the engine gameplay package"),
		(*BindingPackageObject)->GetStringField(TEXT("package_name")),
		FString(TEXT("avidscript.engine.gameplay")));
	TestFalse(
		TEXT("Custom report records a content-addressed package hash"),
		(*BindingPackageObject)->GetStringField(TEXT("package_hash")).IsEmpty());
	TestEqual(
		TEXT("Custom report publishes five bindings, object-type support, and packed owner access"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("profile_import_count"))),
		7);
	TestEqual(
		TEXT("Custom runtime package records five reflected bindings and packed owner access"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("used_import_count"))),
		6);
	TestEqual(TEXT("Custom report exposes six used stable identities"), (*BindingPackageObject)->GetArrayField(TEXT("used_imports")).Num(), 6);

	TSharedPtr<FJsonObject> ManifestObject;
	TestTrue(TEXT("Custom C# profile manifest is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ManifestPath, ManifestObject));
	if (ManifestObject.IsValid())
	{
		const TSharedPtr<FJsonObject>* ExecutionObject = nullptr;
		if (TestTrue(
			TEXT("Custom manifest contains execution provenance"),
			ManifestObject->TryGetObjectField(
				TEXT("execution"),
				ExecutionObject))
			&& ExecutionObject != nullptr
			&& ExecutionObject->IsValid())
		{
			TestEqual(
				TEXT("Custom manifest execution identity matches result"),
				(*ExecutionObject)->GetStringField(TEXT("sha256")),
				BuildResult.VmArtifactSha256);
			TestEqual(
				TEXT("Custom manifest canonical identity matches result"),
				(*ExecutionObject)->GetStringField(
					TEXT("canonical_sha256")),
				BuildResult.VmArtifactCanonicalSha256);
			TestEqual(
				TEXT("Custom manifest persists explicit policy"),
				(*ExecutionObject)->GetStringField(TEXT("policy")),
				FString(TEXT("prefer_precompiled")));
		}
		const TSharedPtr<FJsonObject>* ManifestBindingPackage = nullptr;
		if (TestTrue(
			TEXT("Custom manifest contains runtime binding package provenance"),
			ManifestObject->TryGetObjectField(TEXT("binding_package"), ManifestBindingPackage))
			&& ManifestBindingPackage != nullptr
			&& ManifestBindingPackage->IsValid())
		{
			TestEqual(
				TEXT("Custom manifest publishes five bindings, object-type support, and packed owner access"),
				static_cast<int32>((*ManifestBindingPackage)->GetIntegerField(TEXT("profile_import_count"))),
				7);
		}
	}

	const FString CatalogPath =
		FAvidScriptModulePackageResolver::GetDefaultCatalogPath();
	const bool bHadCatalog = FPaths::FileExists(CatalogPath);
	TArray<uint8> CatalogBackup;
	if (bHadCatalog
		&& !TestTrue(
			TEXT("Existing module catalog can be backed up"),
			FFileHelper::LoadFileToArray(CatalogBackup, *CatalogPath)))
	{
		return false;
	}
	const FString ModuleRoot = FPaths::Combine(
		FPaths::GetPath(CatalogPath),
		Config.ModuleId);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*ModuleRoot, false, true);
		if (bHadCatalog)
		{
			FFileHelper::SaveArrayToFile(CatalogBackup, *CatalogPath);
		}
		else
		{
			IFileManager::Get().Delete(*CatalogPath);
		}
	};

	const FString PublisherScript = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Build/AvidScriptModuleReleasePackage.ps1")));
	const FString ProjectRoot =
		NormalizeAvidScriptCSharpBuildTestPath(FPaths::ProjectDir());
	const FString PublishCommand = FString::Printf(
		TEXT("& { $ErrorActionPreference = 'Stop'; . %s; ")
		TEXT("Publish-AvidScriptModuleReleasePackage ")
		TEXT("-RuntimeManifestPath %s -ProjectRoot %s -ModuleId %s ")
		TEXT("-Configuration Development -TargetPlatform Win64 | ConvertTo-Json -Compress }"),
		*QuoteAvidScriptPowerShellLiteral(PublisherScript),
		*QuoteAvidScriptPowerShellLiteral(Config.ManifestPath),
		*QuoteAvidScriptPowerShellLiteral(ProjectRoot),
		*QuoteAvidScriptPowerShellLiteral(Config.ModuleId));
	const FString PublishArguments =
		TEXT("-NoProfile -Command \"") + PublishCommand + TEXT("\"");
	int32 PublishExitCode = INDEX_NONE;
	FString PublishStdout;
	FString PublishStderr;
	const bool bPublishLaunched = FPlatformProcess::ExecProcess(
		TEXT("pwsh.exe"),
		*PublishArguments,
		&PublishExitCode,
		&PublishStdout,
		&PublishStderr,
		*FPaths::GetPath(PublisherScript));
	if (!TestTrue(
			TEXT("Cooperative C# package publisher launches"),
			bPublishLaunched)
		|| !TestEqual(
			TEXT("Cooperative C# package publication succeeds"),
			PublishExitCode,
			0))
	{
		AddError(PublishStdout + TEXT("\n") + PublishStderr);
		return false;
	}
	FString PublishedCatalogText;
	if (!TestTrue(
			TEXT("Published module catalog can be read"),
			FFileHelper::LoadFileToString(PublishedCatalogText, *CatalogPath))
		|| !TestTrue(
			TEXT("Published module catalog contains the cooperative module"),
			PublishedCatalogText.Contains(
				Config.ModuleId,
				ESearchCase::CaseSensitive)))
	{
		AddError(
			TEXT("publisher stdout: ")
			+ PublishStdout
			+ TEXT("\npublisher stderr: ")
			+ PublishStderr
			+ TEXT("\ncatalog: ")
			+ PublishedCatalogText.Left(2048));
		return false;
	}

	FAvidScriptResolvedModulePackage ResolvedPackage;
	FAvidScriptModuleResolveResult ResolveResult;
	if (!TestTrue(
		TEXT("Cooperative C# package resolves from the default catalog"),
		FAvidScriptModulePackageResolver::ResolveModule(
			FName(*Config.ModuleId),
			ResolvedPackage,
			ResolveResult)))
	{
		AddError(
			ResolveResult.ErrorCategory
			+ TEXT(": ")
			+ ResolveResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("Editor keeps the mutable default catalog in the development trust domain"),
		ResolvedPackage.TrustDomain,
		EAvidScriptModulePackageTrustDomain::DevelopmentCatalog);

	FAvidScriptRuntimeArtifact PublishedArtifact;
	FAvidScriptRuntimeArtifactLoadResult PublishedLoadResult;
	if (!TestTrue(
		TEXT("Cooperative C# package loads through the published module path"),
		FAvidScriptRuntimeArtifactLoader::LoadPublishedModule(
			FName(*Config.ModuleId),
			ResolvedPackage.PackageId,
			PublishedArtifact,
			PublishedLoadResult)))
	{
		AddError(
			PublishedLoadResult.CanonicalResult.ErrorCategory
			+ TEXT(": ")
			+ PublishedLoadResult.CanonicalResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("Editor does not grant persistent trust to a mutable package"),
		PublishedArtifact.ArtifactTrust,
		EAvidScriptVmArtifactTrust::Untrusted);
	TestFalse(
		TEXT("Process-attested Editor package does not need JIT fallback"),
		PublishedLoadResult.bFellBackToJit);
	TestTrue(
		TEXT("Process-attested Editor package preserves cooperative proof"),
		PublishedArtifact.VmArtifact.bCooperativeSafepointProofVerified);

	UWorld* OwnerWorld = nullptr;
	AActor* OwnerActor = nullptr;
	if (!TestTrue(
		TEXT("Published package owner fixture is created"),
		CreateAvidScriptCSharpPackageTestOwner(OwnerWorld, OwnerActor)))
	{
		DestroyAvidScriptCSharpPackageTestOwner(OwnerWorld);
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptCSharpPackageTestOwner(OwnerWorld);
	};
	FAvidScriptObjectRegistry OwnerRegistry;
	FAvidScriptObjectHandleResult OwnerRegisterResult;
	const FAvidScriptObjectHandle OwnerHandle = OwnerRegistry.RegisterObject(
		OwnerActor,
		OwnerRegisterResult,
		false);
	if (!TestTrue(
		TEXT("Published package Actor owner registers"),
		OwnerRegisterResult.bSucceeded))
	{
		AddError(OwnerRegisterResult.ErrorMessage);
		return false;
	}

	FAvidScriptRuntimeSession PublishedSession;
	FAvidScriptWasmHostContext PublishedHostContext;
	PublishedHostContext.ObjectRegistry = &OwnerRegistry;
	PublishedHostContext.OwnerHandle = OwnerHandle;
	PublishedHostContext.World = OwnerWorld;
	PublishedHostContext.ActorWritePolicy =
		EAvidScriptActorWritePolicy::AllowWrites;
	PublishedSession.SetHostContext(PublishedHostContext);
	FAvidScriptWasmReloadResult PublishedSessionResult;
	if (!TestTrue(
		TEXT("Published cooperative C# package enters a Runtime Session"),
		PublishedSession.LoadInitialArtifact(
			PublishedArtifact,
			PublishedSessionResult)))
	{
		AddError(
			PublishedSessionResult.ErrorCategory
			+ TEXT(": ")
			+ PublishedSessionResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Published cooperative C# package reaches BeginPlay"),
		PublishedSessionResult.RuntimeResult.bBeginPlayCalled);
	const FAvidScriptVmLoadConfig::FExecutionBudget& PublishedBudget =
		PublishedSession.GetLiveRuntimeForTesting()
			->GetExecutionBudgetForTesting();
	TestEqual(
		TEXT("Process-attested cooperative package disables epoch ticks"),
		PublishedBudget.EpochDeadlineTicks,
		UINT64_C(0));
	TestEqual(
		TEXT("Process-attested cooperative package enables its deadline"),
		PublishedBudget.CooperativeTimeoutMilliseconds,
		100u);
	FAvidScriptWasmSmokeResult PublishedStopResult;
	TestTrue(
		TEXT("Published cooperative C# package stops cleanly"),
		PublishedSession.StopAndUnload(PublishedStopResult));

	FAvidScriptCSharpBindingEmitResult ExplicitPackage;
	if (!TestTrue(
		TEXT("Explicit gameplay binding package publishes"),
		FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(ExplicitPackage)))
	{
		AddError(ExplicitPackage.ErrorMessage);
		return false;
	}
	FAvidScriptEditorCSharpBuildConfig ExplicitConfig = Config;
	ExplicitConfig.OutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(TestRoot, TEXT("ExplicitPackage")));
	ExplicitConfig.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(ExplicitConfig.OutputRoot, ExplicitConfig.ArtifactStem);
	ExplicitConfig.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(ExplicitConfig.OutputRoot, ExplicitConfig.ArtifactStem);
	ExplicitConfig.BindingPackagePath = ExplicitPackage.ManifestPath;
	ExplicitConfig.PreparedBuildReportPath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		TestRoot,
		TEXT("CallerOwnedPreparedReport"),
		TEXT("missing.csharp.report.json")));
	ExplicitConfig.bEnableDataLaneFusion = false;
	ExplicitConfig.bDisableSemanticCache = true;
	ExplicitConfig.bDisableCompilationCache = true;
	FAvidScriptEditorCSharpBuildResult ExplicitResult;
	TestTrue(TEXT("Explicit package custom C# profile builds"), FAvidScriptEditorCSharpBuildService::BuildProfile(ExplicitConfig, ExplicitResult));
	TestEqual(TEXT("Explicit package uses one build invocation"), ExplicitResult.BuildInvocationCount, 1);
	TestEqual(TEXT("Explicit package runs Frontend once"), ExplicitResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Explicit package runs Semantic once"), ExplicitResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Explicit package runs Guest IR once"), ExplicitResult.GuestIrInvocationCount, 1);
	TestEqual(TEXT("Explicit package runs WASM backend once"), ExplicitResult.WasmBackendInvocationCount, 1);
	TestEqual(TEXT("Explicit diagnostic build disables semantic cache"), ExplicitResult.SemanticCacheLookup, FString(TEXT("disabled")));
	TestEqual(
		TEXT("Explicit package remains both authorization and runtime package"),
		ExplicitResult.AuthorizationBindingPackagePath,
		ExplicitResult.BindingPackagePath);

	FAvidScriptEditorCSharpBuildInvocation Invocation;
	FAvidScriptEditorCSharpBuildResult PreparedResult;
	TestTrue(
		TEXT("Explicit build invocation can be prepared independently"),
		FAvidScriptEditorCSharpBuildInvoker::Prepare(ExplicitConfig, Invocation, PreparedResult));
	TestEqual(
		TEXT("Prepared invocation uses PowerShell 7"),
		Invocation.ExecutablePath,
		FString(TEXT("pwsh.exe")));
	TestFalse(TEXT("Prepared invocation contains parameters"), Invocation.Parameters.IsEmpty());
	TestTrue(
		TEXT("Prepared invocation forwards disabled data-lane fusion"),
		Invocation.Parameters.Contains(TEXT("-DataLaneFusion \"disabled\"")));

	FAvidScriptEditorCSharpBuildResult FinalizedResult;
	TestTrue(
		TEXT("Existing explicit artifacts finalize through the shared contract"),
		FAvidScriptEditorCSharpBuildInvoker::Finalize(
			Invocation,
			ExplicitResult.ProcessExitCode,
			ExplicitResult.Stdout,
			ExplicitResult.Stderr,
			FinalizedResult));
	TestEqual(TEXT("Shared finalizer preserves report path"), FinalizedResult.ReportPath, ExplicitResult.ReportPath);
	TestEqual(TEXT("Shared finalizer preserves manifest path"), FinalizedResult.ManifestPath, ExplicitResult.ManifestPath);
	TestEqual(TEXT("Shared finalizer preserves Frontend count"), FinalizedResult.FrontendInvocationCount, ExplicitResult.FrontendInvocationCount);
	TestEqual(TEXT("Shared finalizer preserves Semantic count"), FinalizedResult.SemanticInvocationCount, ExplicitResult.SemanticInvocationCount);
	TestEqual(TEXT("Shared finalizer preserves Guest IR count"), FinalizedResult.GuestIrInvocationCount, ExplicitResult.GuestIrInvocationCount);
	TestEqual(TEXT("Shared finalizer preserves WASM backend count"), FinalizedResult.WasmBackendInvocationCount, ExplicitResult.WasmBackendInvocationCount);
	TestEqual(TEXT("Shared finalizer preserves cache lookup"), FinalizedResult.SemanticCacheLookup, ExplicitResult.SemanticCacheLookup);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildServiceGeneratedContainerFacadeTest,
	"AvidScript.Editor.CSharpBuildService.GeneratedContainerFacadeDirectAbi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceGeneratedContainerFacadeTest::RunTest(
	const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests/CSharpProfiles/GeneratedContainerFacade")));
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	if (!TestTrue(
		TEXT("Generated container facade test root can be created"),
		IFileManager::Get().MakeDirectory(*TestRoot, true)))
	{
		return false;
	}

	const FString OwnerPath =
		UAvidScriptCSharpBindingEmitterTestObject::StaticClass()->GetPathName();
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName =
		TEXT("avidscript.test.csharp_build_service.container_facade");
	Profile.ExplicitProperties = {
		{ OwnerPath, TEXT("ReadableStringArray"), true },
		{ OwnerPath, TEXT("ReadableIntSet"), true },
		{ OwnerPath, TEXT("ReadableNameStringMap"), true }
	};

	FAvidScriptCSharpBindingEmitResult BindingPackage;
	if (!TestTrue(
		TEXT("Generated container facade binding package publishes"),
		FAvidScriptEditorCSharpBindingEmitter::PublishProfile(
			Profile,
			FPaths::Combine(TestRoot, TEXT("GeneratedBindings")),
			BindingPackage)))
	{
		AddError(BindingPackage.ErrorCategory + TEXT(": ")
			+ BindingPackage.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Generated container facade reference source exists"),
		FPaths::FileExists(BindingPackage.ReferenceSourcePath));
	TestTrue(
		TEXT("Generated container facade manifest exists"),
		FPaths::FileExists(BindingPackage.ManifestPath));

	const FString SourcePath = NormalizeAvidScriptCSharpBuildTestPath(
		FPaths::Combine(TestRoot, TEXT("GeneratedContainerFacadeScript.cs")));
	if (!TestTrue(
		TEXT("Generated container facade user source can be written"),
		FFileHelper::SaveStringToFile(
			MakeAvidScriptGeneratedContainerFacadeSource(),
			*SourcePath)))
	{
		return false;
	}

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath =
		FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath =
		FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = SourcePath;
	Config.ModuleId = TEXT("csharp_generated_container_facade");
	Config.ArtifactStem = TEXT("generated_container_facade");
	Config.OutputRoot = NormalizeAvidScriptCSharpBuildTestPath(
		FPaths::Combine(TestRoot, TEXT("Output")));
	Config.ReportPath =
		FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
			Config.OutputRoot,
			Config.ArtifactStem);
	Config.ManifestPath =
		FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
			Config.OutputRoot,
			Config.ArtifactStem);
	Config.BindingPackagePath = BindingPackage.ManifestPath;
	Config.bEnableDataLaneFusion = false;
	Config.bDisableSemanticCache = true;
	Config.bDisableCompilationCache = true;

	FAvidScriptEditorCSharpBuildResult BuildResult;
	const bool bBuildSucceeded =
		FAvidScriptEditorCSharpBuildService::BuildProfile(Config, BuildResult);
	if (!TestTrue(
		TEXT("Generated container facade direct ABI profile builds"),
		bBuildSucceeded))
	{
		AddError(BuildResult.ErrorCategory + TEXT(": ")
			+ BuildResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Generated container facade build result succeeds"),
		BuildResult.bSucceeded);
	TestEqual(
		TEXT("Generated container facade process exit code"),
		BuildResult.ProcessExitCode,
		0);
	TestEqual(
		TEXT("Generated container facade uses one explicit build"),
		BuildResult.BuildInvocationCount,
		1);
	TestEqual(
		TEXT("Generated container facade runs WASM backend once"),
		BuildResult.WasmBackendInvocationCount,
		1);
	TestEqual(
		TEXT("Generated container facade preserves its authorization package"),
		BuildResult.AuthorizationBindingPackagePath,
		BindingPackage.ManifestPath);
	TestTrue(
		TEXT("Generated container facade publishes a WASM artifact"),
		FPaths::FileExists(FPaths::Combine(
			Config.OutputRoot,
			Config.ArtifactStem + TEXT(".wasm"))));

	TSharedPtr<FJsonObject> ReportObject;
	if (!TestTrue(
		TEXT("Generated container facade report is valid JSON"),
		LoadAvidScriptCSharpBuildTestJsonObject(
			Config.ReportPath,
			ReportObject))
		|| !ReportObject.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("Generated container facade report declares direct ABI success"),
		ReportObject->GetStringField(TEXT("result")),
		FString(TEXT("direct_abi_built")));

	const TSharedPtr<FJsonObject>* AuthorizationObject = nullptr;
	if (!TestTrue(
		TEXT("Generated container facade report contains authorization"),
		ReportObject->TryGetObjectField(
			TEXT("binding_authorization"),
			AuthorizationObject))
		|| AuthorizationObject == nullptr
		|| !AuthorizationObject->IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("Generated container facade authorization package name matches"),
		(*AuthorizationObject)->GetStringField(TEXT("package_name")),
		BindingPackage.PackageName);
	TestEqual(
		TEXT("Generated container facade authorization package hash matches"),
		(*AuthorizationObject)->GetStringField(TEXT("package_hash")),
		BindingPackage.PackageHash);
	TestTrue(
		TEXT("Generated facade calls use read, write, find, upsert, and remove imports"),
		AvidScriptCSharpBuildTestUsedImportsContain(
			*AuthorizationObject,
			{
				TEXT("avidscript.value_container_read.v1"),
				TEXT("avidscript.value_container_write.v1"),
				TEXT("avidscript.value_container_find.v1"),
				TEXT("avidscript.value_container_upsert.v1"),
				TEXT("avidscript.value_container_remove.v1")
			}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildServiceJitOnlyArtifactTest,
	"AvidScript.Editor.CSharpBuildService.JitOnlyArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceJitOnlyArtifactTest::RunTest(
	const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpBuildTestPath(
		FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScriptTests/CSharpProfiles/JitOnlyArtifact")));
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	TestTrue(
		TEXT("JIT-only artifact test root can be created"),
		IFileManager::Get().MakeDirectory(*TestRoot, true));

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.OutputRoot = TestRoot;
	Config.ArtifactStem = TEXT("jit_only");
	Config.ManifestPath = FPaths::Combine(
		TestRoot,
		TEXT("jit_only.avidscript.json"));
	Config.VmArtifactPolicy = EAvidScriptEditorVmArtifactPolicy::JitOnly;
	const FString WasmPath = FPaths::Combine(TestRoot, TEXT("jit_only.wasm"));
	const TArray<uint8> WasmBytes = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00
	};
	TestTrue(
		TEXT("JIT-only canonical WASM writes"),
		FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath));
	const FString ManifestJson = FString::Printf(
		TEXT("{\"schema_version\":1,\"wasm\":{\"file\":\"jit_only.wasm\",\"sha256\":\"%s\"},")
		TEXT("\"execution\":{\"format\":\"stale\"}}"),
		*FAvidScriptHash::Sha256Hex(WasmBytes));
	TestTrue(
		TEXT("JIT-only manifest writes"),
		FFileHelper::SaveStringToFile(ManifestJson, *Config.ManifestPath));
	const FString ArtifactPath =
		FAvidScriptEditorVmArtifactPublisher::MakeArtifactPath(Config);
	TestTrue(
		TEXT("JIT-only stale artifact writes"),
		FFileHelper::SaveStringToFile(TEXT("stale"), *ArtifactPath));

	FAvidScriptEditorCSharpBuildResult Result;
	Result.bSucceeded = true;
	TestTrue(
		TEXT("JIT-only publication succeeds"),
		FAvidScriptEditorVmArtifactPublisher::Publish(Config, Result));
	TestTrue(TEXT("JIT-only result remains successful"), Result.bSucceeded);
	TestFalse(
		TEXT("JIT-only publication removes stale cwasm"),
		FPaths::FileExists(ArtifactPath));
	TestEqual(
		TEXT("JIT-only publication records explicit fallback"),
		Result.VmArtifactFallbackCategory,
		FString(TEXT("jit_only")));
	TSharedPtr<FJsonObject> PublishedManifest;
	TestTrue(
		TEXT("JIT-only manifest remains valid JSON"),
		LoadAvidScriptCSharpBuildTestJsonObject(
			Config.ManifestPath,
			PublishedManifest));
	if (PublishedManifest.IsValid())
	{
		TestFalse(
			TEXT("JIT-only manifest removes execution provenance"),
			PublishedManifest->HasField(TEXT("execution")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildServiceZeroBindingProfileTest,
	"AvidScript.Editor.CSharpBuildService.ZeroBindingProfileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceZeroBindingProfileTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests/CSharpProfiles/ZeroBinding")));
	TestTrue(TEXT("Zero-binding profile root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));
	const FString SourcePath = FPaths::Combine(TestRoot, TEXT("ZeroBindingLifecycleScript.cs"));
	TestTrue(
		TEXT("Zero-binding C# source can be written"),
		FFileHelper::SaveStringToFile(MakeAvidScriptZeroBindingLifecycleSource(), *SourcePath));

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.SourcePath = SourcePath;
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.ModuleId = TEXT("csharp_zero_binding");
	Config.ArtifactStem = TEXT("zero_binding");
	Config.OutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(TestRoot, TEXT("Output")));
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.SemanticCacheRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P43_5/ZeroBinding/CSharpSemanticCache/v1")));
	Config.CompilationCacheRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P61/A1/ZeroBinding/CSharpCompilationCache/v1")));
	Config.bEnableCooperativeSafepoints = true;
	Config.CooperativeSafepointInterval = 64;
	IFileManager::Get().DeleteDirectory(*Config.SemanticCacheRoot, false, true);
	IFileManager::Get().DeleteDirectory(*Config.CompilationCacheRoot, false, true);

	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
		TEXT("Zero-binding custom C# profile builds"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(
			Config,
			BuildResult)))
	{
		AddError(
			BuildResult.ErrorCategory
			+ TEXT(": ")
			+ BuildResult.ErrorMessage
			+ TEXT(" | next: ")
			+ BuildResult.NextAction);
		return false;
	}
	TestEqual(TEXT("Zero-binding profile performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Zero-binding profile runs Frontend once"), BuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Zero-binding profile runs Semantic once"), BuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Zero-binding profile runs Guest IR twice"), BuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Zero-binding profile runs WASM backend twice"), BuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Cold zero-binding profile records a cache miss"), BuildResult.SemanticCacheLookup, FString(TEXT("miss")));
	TestTrue(TEXT("Zero-binding profile keeps authorization package"), FPaths::FileExists(BuildResult.AuthorizationBindingPackagePath));
	TestTrue(TEXT("Zero-binding profile omits runtime package path"), BuildResult.BindingPackagePath.IsEmpty());
	TestTrue(
		TEXT("Zero-binding profile publishes cooperative safepoints"),
		BuildResult.bVmArtifactCooperativeSafepoints);
	TestEqual(
		TEXT("Zero-binding profile preserves the safepoint interval"),
		BuildResult.VmArtifactSafepointInterval,
		64u);
	TestEqual(
		TEXT("Zero-binding profile records the safepoint receipt identity"),
		BuildResult.VmArtifactSafepointReceiptSha256.Len(),
		64);
	TestEqual(
		TEXT("Zero-binding profile records the safepoint site identity"),
		BuildResult.VmArtifactSafepointSiteSha256.Len(),
		64);
	TestTrue(
		TEXT("Zero-binding profile selects epoch-free Wasmtime codegen"),
		BuildResult.VmArtifactCompilerBuildIdentity.Contains(
			TEXT(";epoch_interruption=off;"),
			ESearchCase::CaseSensitive));

	TSharedPtr<FJsonObject> ManifestObject;
	TestTrue(TEXT("Zero-binding manifest is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ManifestPath, ManifestObject));
	if (ManifestObject.IsValid())
	{
		TestFalse(TEXT("Zero-binding manifest omits binding_package"), ManifestObject->HasField(TEXT("binding_package")));
		const TSharedPtr<FJsonObject>* ExecutionObject = nullptr;
		if (TestTrue(
			TEXT("Zero-binding manifest contains execution provenance"),
			ManifestObject->TryGetObjectField(
				TEXT("execution"),
				ExecutionObject))
			&& ExecutionObject != nullptr
			&& ExecutionObject->IsValid())
		{
			const TSharedPtr<FJsonObject>* SafepointObject = nullptr;
			if (TestTrue(
				TEXT("Zero-binding execution contains safepoint provenance"),
				(*ExecutionObject)->TryGetObjectField(
					TEXT("cooperative_safepoints"),
					SafepointObject))
				&& SafepointObject != nullptr
				&& SafepointObject->IsValid())
			{
				TestTrue(
					TEXT("Zero-binding execution enables cooperative safepoints"),
					(*SafepointObject)->GetBoolField(TEXT("enabled")));
				TestEqual(
					TEXT("Zero-binding execution safepoint identity matches the build result"),
					(*SafepointObject)->GetStringField(TEXT("site_sha256")),
					BuildResult.VmArtifactSafepointSiteSha256);
			}
		}
	}

	FAvidScriptRuntimeArtifact RuntimeArtifact;
	FAvidScriptRuntimeArtifactLoadResult RuntimeArtifactResult;
	if (!TestTrue(
		TEXT("Zero-binding cooperative artifact reloads from its published manifest"),
		FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			Config.ManifestPath,
			RuntimeArtifact,
			RuntimeArtifactResult)))
	{
		AddError(RuntimeArtifactResult.CanonicalResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Zero-binding runtime artifact preserves verified safepoints"),
		RuntimeArtifact.VmArtifact.bCooperativeSafepointProofVerified);
	TestEqual(
		TEXT("Zero-binding runtime artifact preserves the site identity"),
		RuntimeArtifact.VmArtifact.CooperativeSafepointSiteSha256,
		BuildResult.VmArtifactSafepointSiteSha256);
	RuntimeArtifact.ArtifactTrust =
		EAvidScriptVmArtifactTrust::VerifiedPackage;
	FAvidScriptRuntimeSession RuntimeSession;
	FAvidScriptWasmReloadResult RuntimeLoadResult;
	if (!TestTrue(
		TEXT("Verified cooperative C# artifact enters a Runtime Session"),
		RuntimeSession.LoadInitialArtifact(
			RuntimeArtifact,
			RuntimeLoadResult)))
	{
		AddError(
			RuntimeLoadResult.ErrorCategory
			+ TEXT(": ")
			+ RuntimeLoadResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Verified cooperative C# artifact reaches BeginPlay"),
		RuntimeLoadResult.RuntimeResult.bBeginPlayCalled);
	const FAvidScriptVmLoadConfig::FExecutionBudget& RuntimeBudget =
		RuntimeSession.GetLiveRuntimeForTesting()
			->GetExecutionBudgetForTesting();
	TestEqual(
		TEXT("Verified cooperative Session disables epoch ticks"),
		RuntimeBudget.EpochDeadlineTicks,
		UINT64_C(0));
	TestEqual(
		TEXT("Verified cooperative Session disables the epoch watchdog"),
		RuntimeBudget.EpochTimeoutMilliseconds,
		0u);
	TestEqual(
		TEXT("Verified cooperative Session enables its wall-clock deadline"),
		RuntimeBudget.CooperativeTimeoutMilliseconds,
		100u);
	FAvidScriptWasmSmokeResult RuntimeStopResult;
	TestTrue(
		TEXT("Verified cooperative C# Session stops cleanly"),
		RuntimeSession.StopAndUnload(RuntimeStopResult));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildServiceSourceMissingNextActionTest,
	"AvidScript.Editor.CSharpBuildService.SourceMissingNextActionSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceSourceMissingNextActionTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests"),
		TEXT("CSharpProfiles"),
		TEXT("MissingSource"),
		TEXT("MissingMover.cs")));

	FAvidScriptEditorCSharpBuildResult BuildResult;
	TestFalse(TEXT("Missing C# source build fails"), FAvidScriptEditorCSharpBuildService::BuildProfile(Config, BuildResult));
	TestFalse(TEXT("Missing C# source build result does not succeed"), BuildResult.bSucceeded);
	TestEqual(TEXT("Missing C# source error category"), BuildResult.ErrorCategory, FString(TEXT("source_missing")));
	TestFalse(TEXT("Missing C# source next action is set"), BuildResult.NextAction.IsEmpty());
	TestTrue(TEXT("Missing C# source next action mentions source or profile"), BuildResult.NextAction.Contains(TEXT("source")) || BuildResult.NextAction.Contains(TEXT("profile")));

	const FString BlockedOutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests"),
		TEXT("CSharpProfiles"),
		TEXT("Blocked*Output")));

	FAvidScriptEditorCSharpBuildConfig BlockedConfig;
	BlockedConfig.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	BlockedConfig.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	BlockedConfig.SourcePath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath();
	BlockedConfig.OutputRoot = BlockedOutputRoot;
	BlockedConfig.ReportPath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(BlockedOutputRoot, TEXT("blocked.csharp.report.json")));
	BlockedConfig.ManifestPath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(BlockedOutputRoot, TEXT("blocked.avidscript.json")));

	FAvidScriptEditorCSharpBuildResult BlockedResult;
	TestFalse(TEXT("Blocked output prevents process launch"), FAvidScriptEditorCSharpBuildService::BuildProfile(BlockedConfig, BlockedResult));
	TestEqual(TEXT("Blocked output has stable error category"), BlockedResult.ErrorCategory, FString(TEXT("output_directory_failed")));
	TestEqual(TEXT("Blocked output launches no build process"), BlockedResult.BuildInvocationCount, 0);
	TestEqual(TEXT("Blocked output launches no Frontend"), BlockedResult.FrontendInvocationCount, 0);
	TestEqual(TEXT("Blocked output launches no Semantic"), BlockedResult.SemanticInvocationCount, 0);
	TestEqual(TEXT("Blocked output launches no Guest IR"), BlockedResult.GuestIrInvocationCount, 0);
	TestEqual(TEXT("Blocked output launches no WASM backend"), BlockedResult.WasmBackendInvocationCount, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
