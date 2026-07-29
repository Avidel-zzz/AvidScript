#include "AvidScriptPerfRunner.h"

#include "AvidScriptGameplayFrameBenchmark.h"
#include "AvidScriptPerfFixture.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmReloadTypes.h"
#include "AvidScriptWasmRuntime.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "JSLogger.h"
#include "JSModuleLoader.h"
#include "JsEnv.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Ssl.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
	constexpr uint32 PerfRunnerMixMultiplier = 1664525u;
	constexpr uint32 PerfRunnerMixIncrement = 1013904223u;
	constexpr uint32 PerfRunnerExactSeedMask = 0x007fffffu;
	constexpr int32 PerfRunnerWorkloadShift = 24;
	constexpr int32 PerfRunnerIterationMask = 0x00ffffff;
	constexpr int32 PerfRunnerResultSchemaVersion = 2;
	constexpr float PerfRunnerTickDeltaSeconds = 1.0f / 60.0f;

	void AppendCanonicalJsonString(
		const FString& Value,
		FString& OutCanonical)
	{
		OutCanonical.AppendChar(TEXT('"'));
		for (const TCHAR Character : Value)
		{
			switch (Character)
			{
			case TEXT('\b'):
				OutCanonical.Append(TEXT("\\b"));
				break;
			case TEXT('\t'):
				OutCanonical.Append(TEXT("\\t"));
				break;
			case TEXT('\n'):
				OutCanonical.Append(TEXT("\\n"));
				break;
			case TEXT('\f'):
				OutCanonical.Append(TEXT("\\f"));
				break;
			case TEXT('\r'):
				OutCanonical.Append(TEXT("\\r"));
				break;
			case TEXT('"'):
				OutCanonical.Append(TEXT("\\\""));
				break;
			case TEXT('\\'):
				OutCanonical.Append(TEXT("\\\\"));
				break;
			default:
				if (static_cast<uint32>(Character) < 0x20u)
				{
					OutCanonical.Appendf(
						TEXT("\\u%04x"),
						static_cast<uint32>(Character));
				}
				else
				{
					OutCanonical.AppendChar(Character);
				}
				break;
			}
		}
		OutCanonical.AppendChar(TEXT('"'));
	}

	bool AppendCanonicalJsonValue(
		const TSharedPtr<FJsonValue>& Value,
		FString& OutCanonical,
		FString& OutError);

	bool AppendCanonicalJsonObject(
		const TSharedPtr<FJsonObject>& Object,
		const FString* ExcludedField,
		FString& OutCanonical,
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = TEXT("canonical lane catalog contains an invalid object");
			return false;
		}
		TArray<FString> Names;
		Names.Reserve(Object->Values.Num());
		for (const auto& Field : Object->Values)
		{
			Names.Add(FString(*Field.Key));
		}
		Names.Sort(
			[](const FString& Left, const FString& Right)
			{
				return Left.Compare(
					Right,
					ESearchCase::CaseSensitive) < 0;
			});

		OutCanonical.AppendChar(TEXT('{'));
		bool bFirst = true;
		for (const FString& Name : Names)
		{
			if (ExcludedField != nullptr &&
				Name.Equals(*ExcludedField, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (!bFirst)
			{
				OutCanonical.AppendChar(TEXT(','));
			}
			bFirst = false;
			AppendCanonicalJsonString(Name, OutCanonical);
			OutCanonical.AppendChar(TEXT(':'));
			if (!AppendCanonicalJsonValue(
				Object->TryGetField(Name),
				OutCanonical,
				OutError))
			{
				return false;
			}
		}
		OutCanonical.AppendChar(TEXT('}'));
		return true;
	}

	bool AppendCanonicalJsonValue(
		const TSharedPtr<FJsonValue>& Value,
		FString& OutCanonical,
		FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("canonical lane catalog contains an invalid value");
			return false;
		}
		switch (Value->Type)
		{
		case EJson::Null:
			OutCanonical.Append(TEXT("null"));
			return true;
		case EJson::String:
			AppendCanonicalJsonString(Value->AsString(), OutCanonical);
			return true;
		case EJson::Boolean:
			OutCanonical.Append(
				Value->AsBool() ? TEXT("true") : TEXT("false"));
			return true;
		case EJson::Array:
		{
			OutCanonical.AppendChar(TEXT('['));
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				if (Index > 0)
				{
					OutCanonical.AppendChar(TEXT(','));
				}
				if (!AppendCanonicalJsonValue(
					Items[Index],
					OutCanonical,
					OutError))
				{
					return false;
				}
			}
			OutCanonical.AppendChar(TEXT(']'));
			return true;
		}
		case EJson::Object:
			return AppendCanonicalJsonObject(
				Value->AsObject(),
				nullptr,
				OutCanonical,
				OutError);
		default:
			OutError = TEXT(
				"canonical lane catalog permits only null, string, boolean, array, and object values");
			return false;
		}
	}

	bool GetCanonicalJsonSha256(
		const FString& CanonicalJson,
		FString& OutSha256,
		FString& OutError)
	{
		const FTCHARToUTF8 Utf8(*CanonicalJson);
		if (Utf8.Length() < 0 ||
			static_cast<uint64>(Utf8.Length()) > MAX_uint32)
		{
			OutError = TEXT("canonical lane catalog UTF-8 payload is too large");
			return false;
		}
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		if (SHA256(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				static_cast<size_t>(Utf8.Length()),
				Digest) == nullptr)
		{
			OutError = TEXT("canonical lane catalog SHA-256 failed");
			return false;
		}
		OutSha256.Reset(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			OutSha256 += FString::Printf(TEXT("%02x"), Byte);
		}
		return true;
	}

	bool GetPerfFileSha256(
		const FString& Path,
		FString& OutSha256,
		FString& OutError)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			OutError = FString::Printf(
				TEXT("benchmark identity artifact could not be read: %s"),
				*Path);
			return false;
		}
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		if (SHA256(
				Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest) == nullptr)
		{
			OutError = FString::Printf(
				TEXT("benchmark identity artifact SHA-256 failed: %s"),
				*Path);
			return false;
		}
		OutSha256.Reset(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			OutSha256 += FString::Printf(TEXT("%02x"), Byte);
		}
		return true;
	}

	bool GetCanonicalLaneIdentitySha256(
		const TSharedPtr<FJsonObject>& Entry,
		FString& OutSha256,
		FString& OutError)
	{
		FString CanonicalJson;
		const FString ExcludedField = TEXT("lane_identity_sha256");
		if (!AppendCanonicalJsonObject(
			Entry,
			&ExcludedField,
			CanonicalJson,
			OutError))
		{
			return false;
		}
		return GetCanonicalJsonSha256(
			CanonicalJson,
			OutSha256,
			OutError);
	}

	bool GetCanonicalLaneCatalogSha256(
		const TArray<TSharedPtr<FJsonValue>>& Catalog,
		FString& OutSha256,
		FString& OutError)
	{
		FString CanonicalJson(TEXT("["));
		for (int32 Index = 0; Index < Catalog.Num(); ++Index)
		{
			if (Index > 0)
			{
				CanonicalJson.AppendChar(TEXT(','));
			}
			if (!AppendCanonicalJsonValue(
				Catalog[Index],
				CanonicalJson,
				OutError))
			{
				return false;
			}
		}
		CanonicalJson.AppendChar(TEXT(']'));
		return GetCanonicalJsonSha256(
			CanonicalJson,
			OutSha256,
			OutError);
	}

	uint32 PerfRunnerMix(const uint32 Value)
	{
		return Value * PerfRunnerMixMultiplier + PerfRunnerMixIncrement;
	}

	uint32 MakePerfRunnerWorkloadSeed(const int32 Seed, const int32 WorkloadIndex)
	{
		return PerfRunnerMix(
			static_cast<uint32>(Seed) ^
			static_cast<uint32>(WorkloadIndex + 1)) & PerfRunnerExactSeedMask;
	}

	class FAvidScriptPerfModuleLoader final : public puerts::IJSModuleLoader
	{
	public:
		FAvidScriptPerfModuleLoader(FString InWorkloadRoot, FString InRuntimeRoot)
			: WorkloadRoot(MoveTemp(InWorkloadRoot))
			, RuntimeRoot(MoveTemp(InRuntimeRoot))
		{
			WorkloadRoot = FPaths::ConvertRelativePathToFull(WorkloadRoot);
			RuntimeRoot = FPaths::ConvertRelativePathToFull(RuntimeRoot);
			FPaths::NormalizeDirectoryName(WorkloadRoot);
			FPaths::NormalizeDirectoryName(RuntimeRoot);
		}

		virtual bool Search(
			const FString& RequiredDir,
			const FString& RequiredModule,
			FString& Path,
			FString& AbsolutePath) override
		{
			TArray<FString, TInlineAllocator<3>> Candidates;
			if (!RequiredDir.IsEmpty())
			{
				Candidates.Add(FPaths::Combine(RequiredDir, RequiredModule));
			}
			Candidates.Add(FPaths::Combine(WorkloadRoot, RequiredModule));
			Candidates.Add(FPaths::Combine(RuntimeRoot, RequiredModule));

			for (FString& Candidate : Candidates)
			{
				if (FPaths::GetExtension(Candidate).IsEmpty())
				{
					Candidate += TEXT(".js");
				}
				Candidate = FPaths::ConvertRelativePathToFull(Candidate);
				FPaths::NormalizeFilename(Candidate);
				if (!IsUnderAllowedRoot(Candidate) || !FPaths::FileExists(Candidate))
				{
					continue;
				}

				Path = Candidate;
				AbsolutePath = Candidate;
				return true;
			}
			return false;
		}

		virtual bool Load(const FString& Path, TArray<uint8>& Content) override
		{
			return FFileHelper::LoadFileToArray(Content, *Path);
		}

		virtual FString& GetScriptRoot() override
		{
			return WorkloadRoot;
		}

	private:
		bool IsUnderAllowedRoot(const FString& Candidate) const
		{
			return Candidate.StartsWith(
					WorkloadRoot + TEXT("/"),
					ESearchCase::IgnoreCase)
				|| Candidate.StartsWith(
					RuntimeRoot + TEXT("/"),
					ESearchCase::IgnoreCase);
		}

		FString WorkloadRoot;
		FString RuntimeRoot;
	};

	struct FPuertsLane
	{
		AAvidScriptPerfFixture* Fixture = nullptr;
		int32 LaneId = 0;
		TSharedPtr<puerts::FJsEnv> Environment;

		bool Initialize(
			const FString& ModuleName,
			const int32 InLaneId,
			const FString& ExpectedScriptSha256,
			const FString& ExpectedRuntimeArtifactSha256,
			AAvidScriptPerfFixture& SharedFixture,
			FString& OutError)
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScriptPerfHarness"));
			const TSharedPtr<IPlugin> PuertsPlugin = IPluginManager::Get().FindPlugin(TEXT("Puerts"));
			if (!Plugin.IsValid() || !PuertsPlugin.IsValid())
			{
				OutError = TEXT("AvidScriptPerfHarness or Puerts plugin is not mounted");
				return false;
			}

			const FString ScriptRoot = FPaths::Combine(Plugin->GetContentDir(), TEXT("JavaScript"));
			const FString RuntimeRoot = FPaths::Combine(PuertsPlugin->GetContentDir(), TEXT("JavaScript"));
			if (!FPaths::DirectoryExists(ScriptRoot) || !FPaths::DirectoryExists(RuntimeRoot))
			{
				OutError = FString::Printf(
					TEXT("Puerts workload/runtime root is missing: workload=%s runtime=%s"),
					*ScriptRoot,
					*RuntimeRoot);
				return false;
			}

			const FString ScriptPath = FPaths::Combine(ScriptRoot, ModuleName);
			FString ActualScriptSha256;
			if (!GetPerfFileSha256(ScriptPath, ActualScriptSha256, OutError) ||
				!ActualScriptSha256.Equals(
					ExpectedScriptSha256,
					ESearchCase::CaseSensitive))
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("Puerts workload script identity mismatch: module=%s expected=%s actual=%s"),
						*ModuleName,
						*ExpectedScriptSha256,
						*ActualScriptSha256);
				}
				return false;
			}

			const FString RuntimeModulePath =
				FModuleManager::Get().GetModuleFilename(TEXT("JsEnv"));
			FString ActualRuntimeArtifactSha256;
			if (RuntimeModulePath.IsEmpty() ||
				!GetPerfFileSha256(
					RuntimeModulePath,
					ActualRuntimeArtifactSha256,
					OutError) ||
				!ActualRuntimeArtifactSha256.Equals(
					ExpectedRuntimeArtifactSha256,
					ESearchCase::CaseSensitive))
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("Puerts runtime artifact identity mismatch: expected=%s actual=%s"),
						*ExpectedRuntimeArtifactSha256,
						*ActualRuntimeArtifactSha256);
				}
				return false;
			}

			Fixture = &SharedFixture;
			LaneId = InLaneId;
			Environment = MakeShared<puerts::FJsEnv>(
				std::make_shared<FAvidScriptPerfModuleLoader>(ScriptRoot, RuntimeRoot),
				std::make_shared<puerts::FDefaultLogger>(),
				-1);
			Environment->Start(
				ModuleName,
				{ TPair<FString, UObject*>(TEXT("Fixture"), Fixture) });
			if (!Fixture->HasPuertsCallbacks(LaneId))
			{
				OutError = FString::Printf(TEXT("Puerts module did not register callbacks: %s"), *ModuleName);
				return false;
			}
			return true;
		}

		bool InitializeForCorrectnessSmoke(
			const FString& ModuleName,
			const int32 InLaneId,
			AAvidScriptPerfFixture& SharedFixture,
			FString& OutError)
		{
			const TSharedPtr<IPlugin> Plugin =
				IPluginManager::Get().FindPlugin(TEXT("AvidScriptPerfHarness"));
			if (!Plugin.IsValid())
			{
				OutError = TEXT("AvidScriptPerfHarness plugin is not mounted");
				return false;
			}

			FString ScriptSha256;
			FString RuntimeArtifactSha256;
			const FString ScriptPath = FPaths::Combine(
				Plugin->GetContentDir(),
				TEXT("JavaScript"),
				ModuleName);
			const FString RuntimeModulePath =
				FModuleManager::Get().GetModuleFilename(TEXT("JsEnv"));
			if (RuntimeModulePath.IsEmpty() ||
				!GetPerfFileSha256(ScriptPath, ScriptSha256, OutError) ||
				!GetPerfFileSha256(
					RuntimeModulePath,
					RuntimeArtifactSha256,
					OutError))
			{
				return false;
			}

			return Initialize(
				ModuleName,
				InLaneId,
				ScriptSha256,
				RuntimeArtifactSha256,
				SharedFixture,
				OutError);
		}
	};

	struct FAvidScriptLane
	{
		FAvidScriptObjectRegistry Registry;
		FAvidScriptRuntimeSession Session;
		FAvidScriptWasmReloadManifest Manifest;
		TArray<uint8> Bytecode;
		const FAvidScriptWasmStateSlot* ResultSlot = nullptr;
		int32 LastHostImportCallCount = 0;
		FAvidScriptBindingInvocationInstrumentation InvocationInstrumentation;
		uint64 LastQualifiedNativeDirectCount = 0;
		uint64 LastRequestedNativeDirectFallbackCount = 0;
		uint64 LastGeneratedS1HitCount = 0;
		uint64 LastGeneratedS1FallbackCount = 0;
		uint64 LastGeneratedS1RejectCount = 0;
		uint64 LastGeneratedFusedFastHitCount = 0;
		uint64 LastGeneratedFusedRevalidateCount = 0;
		uint64 LastGeneratedFusedCallSitePrepareCount = 0;
		uint64 LastGeneratedDirectReadPrepareCount = 0;
		uint64 LastGeneratedDirectWritePrepareCount = 0;
		uint64 LastGeneratedJournalSlowPathCount = 0;
		uint64 LastSemanticHitCount = 0;
		uint64 LastDataBoundaryCrossingCount = 0;
		uint64 LastDataAppliedCommandCount = 0;
		uint64 LastDataRejectedBufferCount = 0;
		uint64 CollectedQualifiedNativeDirectCount = 0;
		uint64 CollectedRequestedNativeDirectFallbackCount = 0;
		uint64 CollectedGeneratedS1HitCount = 0;
		uint64 CollectedGeneratedS1FallbackCount = 0;
		uint64 CollectedGeneratedS1RejectCount = 0;
		uint64 CollectedGeneratedFusedFastHitCount = 0;
		uint64 CollectedGeneratedFusedRevalidateCount = 0;
		uint64 CollectedGeneratedFusedCallSitePrepareCount = 0;
		uint64 CollectedGeneratedDirectReadPrepareCount = 0;
		uint64 CollectedGeneratedDirectWritePrepareCount = 0;
		uint64 CollectedGeneratedJournalSlowPathCount = 0;
		uint64 CollectedSemanticHitCount = 0;
		uint64 CollectedDataBoundaryCrossingCount = 0;
		uint64 CollectedDataAppliedCommandCount = 0;
		uint64 CollectedDataRejectedBufferCount = 0;
		bool bRequestsNativeDirect = false;
		bool bDataMode = false;
		bool bScalarAddNativeDirect = false;
		bool bBatchScalarNativeDirect = false;
		TSharedPtr<FJsonObject> BackendInfoJson;

		bool Initialize(
			const FAvidScriptVmBackendSelection& BackendSelection,
			const EAvidScriptBindingInvocationPolicy InvocationPolicy,
			const FString& ExpectedBindingInvocationMode,
			const FString& ExpectedBackendId,
			const FString& ExpectedRuntimeVersion,
			const FString& ExpectedExecutionMode,
			const FString& ExpectedArtifactFormat,
			const FString& ExpectedArtifactSha256,
			const FString& ExpectedSourceWasmSha256,
			const FString& ExpectedTargetTriple,
			const FString& ExpectedRuntimeBuildIdentity,
			const FString& ExpectedRuntimeArtifactSha256,
			const FString& ManifestRelativePath,
			AAvidScriptPerfFixture& SharedFixture,
			FString& OutError)
		{
			if (ManifestRelativePath.IsEmpty() ||
				FPaths::IsRelative(ManifestRelativePath) == false ||
				ManifestRelativePath.Contains(TEXT("..")))
			{
				OutError = TEXT(
					"AvidScript benchmark manifest_relative_path must be a safe Saved-relative path");
				return false;
			}
			const FString ManifestPath = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				ManifestRelativePath);
			FAvidScriptWasmReloadManifestLoadResult LoadResult;
			if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				ManifestPath,
				Manifest,
				Bytecode,
				LoadResult))
			{
				OutError = FString::Printf(
					TEXT("AvidScript benchmark manifest load failed: %s"),
					*LoadResult.ErrorMessage);
				return false;
			}

			ResultSlot = Manifest.StateMigration.Slots.FindByPredicate(
				[](const FAvidScriptWasmStateSlot& Slot)
				{
					return Slot.StableId.EndsWith(
						TEXT(":ResultChecksum"),
						ESearchCase::CaseSensitive);
				});
			if (ResultSlot == nullptr || ResultSlot->Size != sizeof(int32))
			{
				OutError = TEXT(
					"AvidScript benchmark manifest is missing the int32 ResultChecksum state slot");
				return false;
			}

			FAvidScriptObjectHandleResult RegisterResult;
			const FAvidScriptObjectHandle OwnerHandle =
				Registry.RegisterObject(&SharedFixture, RegisterResult, true);
			if (!RegisterResult.bSucceeded || !OwnerHandle.IsValid())
			{
				OutError = FString::Printf(
					TEXT("AvidScript benchmark owner registration failed: %s"),
					*RegisterResult.ErrorMessage);
				return false;
			}

			FAvidScriptWasmHostContext HostContext;
			HostContext.ObjectRegistry = &Registry;
			HostContext.OwnerHandle = OwnerHandle;
			HostContext.World = SharedFixture.GetWorld();
			HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
			HostContext.BindingInvocationPolicy = InvocationPolicy;
			HostContext.BindingInvocationInstrumentation =
				&InvocationInstrumentation;
			Session.SetHostContext(HostContext);
#if WITH_DEV_AUTOMATION_TESTS
			Session.SetBackendSelectionForTesting(BackendSelection);
#else
			OutError = TEXT("AvidScript benchmark backend selection requires WITH_DEV_AUTOMATION_TESTS");
			return false;
#endif

			FAvidScriptWasmReloadResult ReloadResult;
			if (!Session.LoadInitialModule(
					Bytecode.GetData(),
					Bytecode.Num(),
					Manifest,
					ReloadResult))
			{
				OutError = FString::Printf(
					TEXT("AvidScript benchmark runtime initialization failed: %s"),
					*ReloadResult.ErrorMessage);
				return false;
			}
			if (!Manifest.BindingPackage.IsValid())
			{
				OutError = TEXT("AvidScript benchmark manifest is missing the immutable binding package");
				Session.UnloadLive();
				return false;
			}
			bRequestsNativeDirect =
				InvocationPolicy == EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect;
			const bool bSemanticMode =
				ExpectedBindingInvocationMode.Equals(
					TEXT("semantic_process_event"),
					ESearchCase::CaseSensitive);
			const bool bGeneratedMode =
				ExpectedBindingInvocationMode.Equals(
					TEXT("generated_native_s1"),
					ESearchCase::CaseSensitive);
			bDataMode =
				ExpectedBindingInvocationMode.Equals(
					TEXT("data_command_buffer"),
					ESearchCase::CaseSensitive);
			if ((!bSemanticMode && !bGeneratedMode && !bDataMode) ||
				(bSemanticMode && bRequestsNativeDirect) ||
				(!bSemanticMode && !bRequestsNativeDirect))
			{
				OutError = TEXT("AvidScript benchmark binding invocation mode mismatch");
				Session.UnloadLive();
				return false;
			}
			uint32 ScalarOrdinal = MAX_uint32;
			EAvidScriptBindingInvocationMode ScalarMode =
				EAvidScriptBindingInvocationMode::SemanticProcessEvent;
			const EAvidScriptBindingInvocationMode ExpectedScalarMode =
				bSemanticMode
					? EAvidScriptBindingInvocationMode::SemanticProcessEvent
					: EAvidScriptBindingInvocationMode::GeneratedNativeS1;
			if (!Manifest.BindingPackage->TryFindFunctionOrdinal(
					*AAvidScriptPerfFixture::StaticClass(),
					FName(TEXT("ReflectAddInt32")),
					ScalarOrdinal) ||
				!Manifest.BindingPackage->TryGetInvocationMode(
					ScalarOrdinal,
					ScalarMode) ||
				ScalarMode != ExpectedScalarMode)
			{
				OutError = TEXT(
					"AvidScript benchmark immutable scalar invocation plan does not match the lane contract");
				Session.UnloadLive();
				return false;
			}
			const FAvidScriptVmBackendInfo& Actual = ReloadResult.RuntimeResult.BackendInfo;
			const FString ActualExecutionMode =
				Actual.ExecutionMode == EAvidScriptVmExecutionMode::Interpreter
					? TEXT("interpreter")
					: Actual.ExecutionMode == EAvidScriptVmExecutionMode::Jit
						? TEXT("jit")
						: TEXT("unsupported");
			const FString ActualArtifactFormat =
				Actual.ArtifactFormat == EAvidScriptVmArtifactFormat::WasmBytecode
					? TEXT("wasm_bytecode")
					: TEXT("unsupported");
			if (!Actual.StableBackendId.Equals(ExpectedBackendId, ESearchCase::CaseSensitive) ||
				!Actual.RuntimeVersion.Equals(ExpectedRuntimeVersion, ESearchCase::CaseSensitive) ||
				!ActualExecutionMode.Equals(ExpectedExecutionMode, ESearchCase::CaseSensitive) ||
				!ActualArtifactFormat.Equals(ExpectedArtifactFormat, ESearchCase::CaseSensitive) ||
				(!ExpectedArtifactSha256.IsEmpty() &&
				 !Manifest.WasmSha256.Equals(ExpectedArtifactSha256, ESearchCase::CaseSensitive)) ||
				(!ExpectedSourceWasmSha256.IsEmpty() &&
				 !Manifest.WasmSha256.Equals(ExpectedSourceWasmSha256, ESearchCase::CaseSensitive)) ||
				!Actual.TargetTriple.Equals(ExpectedTargetTriple, ESearchCase::CaseSensitive) ||
				(!ExpectedRuntimeBuildIdentity.IsEmpty() &&
				 !Actual.RuntimeBuildIdentity.Equals(
					 ExpectedRuntimeBuildIdentity,
					 ESearchCase::CaseSensitive)) ||
				(!ExpectedRuntimeArtifactSha256.IsEmpty() &&
				 !Actual.RuntimeArtifactSha256.Equals(
					 ExpectedRuntimeArtifactSha256,
					 ESearchCase::CaseSensitive)) ||
				Actual.RuntimeBuildIdentity.IsEmpty() ||
				Actual.RuntimeArtifactSha256.IsEmpty() ||
				BackendSelection.bAllowFallback)
			{
				OutError = FString::Printf(
					TEXT("AvidScript benchmark backend provenance mismatch: ")
					TEXT("expected=%s/%s/%s/%s/%s build=%s runtime_artifact=%s ")
					TEXT("actual=%s/%s/%s/%s/%s build=%s runtime_artifact=%s fallback_used=%s"),
					*ExpectedBackendId,
					*ExpectedRuntimeVersion,
					*ExpectedExecutionMode,
					*ExpectedArtifactFormat,
					*ExpectedTargetTriple,
					*ExpectedRuntimeBuildIdentity,
					*ExpectedRuntimeArtifactSha256,
					*Actual.StableBackendId,
					*Actual.RuntimeVersion,
					*ActualExecutionMode,
					*ActualArtifactFormat,
					*Actual.TargetTriple,
					*Actual.RuntimeBuildIdentity,
					*Actual.RuntimeArtifactSha256,
					BackendSelection.bAllowFallback ? TEXT("true") : TEXT("false"));
				Session.UnloadLive();
				return false;
			}
			BackendInfoJson = MakeShared<FJsonObject>();
			BackendInfoJson->SetStringField(TEXT("backend_id"), Actual.StableBackendId);
			BackendInfoJson->SetStringField(
				TEXT("binding_invocation_mode"),
				ExpectedBindingInvocationMode);
			BackendInfoJson->SetStringField(TEXT("runtime_version"), Actual.RuntimeVersion);
			BackendInfoJson->SetStringField(TEXT("execution_mode"), ActualExecutionMode);
			BackendInfoJson->SetStringField(TEXT("artifact_format"), ActualArtifactFormat);
			BackendInfoJson->SetStringField(TEXT("artifact_sha256"), Manifest.WasmSha256);
			BackendInfoJson->SetStringField(TEXT("source_wasm_sha256"), Manifest.WasmSha256);
			BackendInfoJson->SetStringField(
				TEXT("binding_package_name"),
				Manifest.BindingPackageName);
			BackendInfoJson->SetStringField(
				TEXT("binding_package_hash"),
				Manifest.BindingPackageHash);
			BackendInfoJson->SetStringField(TEXT("target_triple"), Actual.TargetTriple);
			BackendInfoJson->SetStringField(
				TEXT("runtime_build_identity"),
				Actual.RuntimeBuildIdentity);
			BackendInfoJson->SetStringField(
				TEXT("runtime_artifact_sha256"),
				Actual.RuntimeArtifactSha256);
			BackendInfoJson->SetBoolField(TEXT("fallback_used"), false);
			BackendInfoJson->SetNumberField(
				TEXT("execution_artifact_size_bytes"),
				Bytecode.Num());
			BackendInfoJson->SetStringField(
				TEXT("generated_code_size_status"),
				TEXT("runtime_not_exposed"));
			LastHostImportCallCount = ReloadResult.RuntimeResult.HostImportCallCount;
			LastQualifiedNativeDirectCount =
				InvocationInstrumentation.QualifiedNativeDirectCount;
			LastRequestedNativeDirectFallbackCount =
				InvocationInstrumentation.RequestedNativeDirectFallbackCount;
			LastGeneratedS1HitCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedNativeS1HitCount;
			LastGeneratedS1FallbackCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedNativeS1FallbackCount;
			LastGeneratedS1RejectCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedNativeS1RejectCount;
			LastGeneratedFusedFastHitCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedFusedFastHitCount;
			LastGeneratedFusedRevalidateCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedFusedRevalidateCount;
			LastGeneratedFusedCallSitePrepareCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedFusedCallSitePrepareCount;
			LastGeneratedDirectReadPrepareCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedDirectReadPrepareCount;
			LastGeneratedDirectWritePrepareCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedDirectWritePrepareCount;
			LastGeneratedJournalSlowPathCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.GeneratedJournalSlowPathCount;
			LastSemanticHitCount =
				ReloadResult.RuntimeResult.BindingInstrumentation
					.SemanticProcessEventCount;
			LastDataBoundaryCrossingCount =
				ReloadResult.RuntimeResult.DataBridgeMetrics.BoundaryCrossings;
			LastDataAppliedCommandCount =
				ReloadResult.RuntimeResult.DataBridgeMetrics.AppliedCommands;
			LastDataRejectedBufferCount =
				ReloadResult.RuntimeResult.DataBridgeMetrics.RejectedBuffers;
			return true;
		}

		void GetInvocationEvidence(
			uint64& OutDirectHitCount,
			uint64& OutRequestedDirectFallbackCount,
			uint64& OutGeneratedS1HitCount,
			uint64& OutGeneratedS1FallbackCount,
			uint64& OutGeneratedS1RejectCount,
			uint64& OutGeneratedFusedFastHitCount,
			uint64& OutGeneratedFusedRevalidateCount,
			uint64& OutGeneratedFusedCallSitePrepareCount,
			uint64& OutGeneratedDirectReadPrepareCount,
			uint64& OutGeneratedDirectWritePrepareCount,
			uint64& OutGeneratedJournalSlowPathCount,
			uint64& OutSemanticHitCount,
			uint64& OutDataCommandCount,
			uint64& OutDataCrossingCount,
			uint64& OutDataRejectedBufferCount) const
		{
			OutDirectHitCount = CollectedQualifiedNativeDirectCount;
			OutRequestedDirectFallbackCount =
				CollectedRequestedNativeDirectFallbackCount;
			OutGeneratedS1HitCount = CollectedGeneratedS1HitCount;
			OutGeneratedS1FallbackCount = CollectedGeneratedS1FallbackCount;
			OutGeneratedS1RejectCount = CollectedGeneratedS1RejectCount;
			OutGeneratedFusedFastHitCount =
				CollectedGeneratedFusedFastHitCount;
			OutGeneratedFusedRevalidateCount =
				CollectedGeneratedFusedRevalidateCount;
			OutGeneratedFusedCallSitePrepareCount =
				CollectedGeneratedFusedCallSitePrepareCount;
			OutGeneratedDirectReadPrepareCount =
				CollectedGeneratedDirectReadPrepareCount;
			OutGeneratedDirectWritePrepareCount =
				CollectedGeneratedDirectWritePrepareCount;
			OutGeneratedJournalSlowPathCount =
				CollectedGeneratedJournalSlowPathCount;
			OutSemanticHitCount = CollectedSemanticHitCount;
			OutDataCommandCount = CollectedDataAppliedCommandCount;
			OutDataCrossingCount = CollectedDataBoundaryCrossingCount;
			OutDataRejectedBufferCount = CollectedDataRejectedBufferCount;
		}

		bool PrepareWorkload(
			const EAvidScriptPerfWorkload Workload,
			const int32 Iterations,
			const uint32 Seed,
			int32& OutPackedWorkload,
			FString& OutError)
		{
			int32 WorkloadId = static_cast<int32>(Workload);
			if (bDataMode &&
				FAvidScriptGameplayFrameBenchmark::IsGameplayWorkload(Workload))
			{
				WorkloadId += 2;
			}
			if (WorkloadId < 0 ||
				WorkloadId > 0x7f ||
				Iterations <= 0 ||
				Iterations > PerfRunnerIterationMask ||
				Seed > PerfRunnerExactSeedMask)
			{
				OutError = TEXT("AvidScript benchmark event packing arguments are out of range");
				return false;
			}

			OutPackedWorkload =
				(WorkloadId << PerfRunnerWorkloadShift) |
				(Iterations & PerfRunnerIterationMask);
			return true;
		}

		bool DispatchWorkload(
			const int32 PackedWorkload,
			const uint32 Seed,
			FAvidScriptWasmSmokeResult& OutDispatchResult)
		{
			return Session.DispatchEvent(
				PackedWorkload,
				static_cast<float>(Seed),
				OutDispatchResult);
		}

		bool CollectWorkloadResult(
			const bool bDispatchSucceeded,
			const FAvidScriptWasmSmokeResult& DispatchResult,
			uint32& OutChecksum,
			int32& OutHostImportCallCount,
			FString& OutError)
		{
			if (!bDispatchSucceeded)
			{
				OutError = FString::Printf(
					TEXT("AvidScript benchmark workload dispatch failed: %s"),
					*DispatchResult.ErrorMessage);
				return false;
			}

			int32 Checksum = 0;
			FString ReadError;
			const FAvidScriptWasmRuntimeInstance* Runtime =
				Session.GetLiveRuntimeForTesting();
			if (Runtime == nullptr ||
				!Runtime->ReadStateBytes(
				ResultSlot->Offset,
				MakeArrayView(
					reinterpret_cast<uint8*>(&Checksum),
					sizeof(Checksum)),
				ReadError))
			{
				OutError = FString::Printf(
					TEXT("AvidScript benchmark checksum read failed: %s"),
					*ReadError);
				return false;
			}

			OutChecksum = static_cast<uint32>(Checksum);
			OutHostImportCallCount =
				DispatchResult.HostImportCallCount - LastHostImportCallCount;
			LastHostImportCallCount = DispatchResult.HostImportCallCount;
			CollectedQualifiedNativeDirectCount =
				InvocationInstrumentation.QualifiedNativeDirectCount
				- LastQualifiedNativeDirectCount;
			CollectedRequestedNativeDirectFallbackCount =
				InvocationInstrumentation.RequestedNativeDirectFallbackCount
				- LastRequestedNativeDirectFallbackCount;
			LastQualifiedNativeDirectCount =
				InvocationInstrumentation.QualifiedNativeDirectCount;
			LastRequestedNativeDirectFallbackCount =
				InvocationInstrumentation.RequestedNativeDirectFallbackCount;
			CollectedGeneratedS1HitCount =
				DispatchResult.BindingInstrumentation.GeneratedNativeS1HitCount
				- LastGeneratedS1HitCount;
			CollectedGeneratedS1FallbackCount =
				DispatchResult.BindingInstrumentation.GeneratedNativeS1FallbackCount
				- LastGeneratedS1FallbackCount;
			CollectedGeneratedS1RejectCount =
				DispatchResult.BindingInstrumentation.GeneratedNativeS1RejectCount
				- LastGeneratedS1RejectCount;
			CollectedGeneratedFusedFastHitCount =
				DispatchResult.BindingInstrumentation.GeneratedFusedFastHitCount
				- LastGeneratedFusedFastHitCount;
			CollectedGeneratedFusedRevalidateCount =
				DispatchResult.BindingInstrumentation.GeneratedFusedRevalidateCount
				- LastGeneratedFusedRevalidateCount;
			CollectedGeneratedFusedCallSitePrepareCount =
				DispatchResult.BindingInstrumentation.GeneratedFusedCallSitePrepareCount
				- LastGeneratedFusedCallSitePrepareCount;
			CollectedGeneratedDirectReadPrepareCount =
				DispatchResult.BindingInstrumentation.GeneratedDirectReadPrepareCount
				- LastGeneratedDirectReadPrepareCount;
			CollectedGeneratedDirectWritePrepareCount =
				DispatchResult.BindingInstrumentation.GeneratedDirectWritePrepareCount
				- LastGeneratedDirectWritePrepareCount;
			CollectedGeneratedJournalSlowPathCount =
				DispatchResult.BindingInstrumentation.GeneratedJournalSlowPathCount
				- LastGeneratedJournalSlowPathCount;
			CollectedSemanticHitCount =
				DispatchResult.BindingInstrumentation.SemanticProcessEventCount
				- LastSemanticHitCount;
			CollectedDataBoundaryCrossingCount =
				DispatchResult.DataBridgeMetrics.BoundaryCrossings
				- LastDataBoundaryCrossingCount;
			CollectedDataAppliedCommandCount =
				DispatchResult.DataBridgeMetrics.AppliedCommands
				- LastDataAppliedCommandCount;
			CollectedDataRejectedBufferCount =
				DispatchResult.DataBridgeMetrics.RejectedBuffers
				- LastDataRejectedBufferCount;
			LastGeneratedS1HitCount =
				DispatchResult.BindingInstrumentation.GeneratedNativeS1HitCount;
			LastGeneratedS1FallbackCount =
				DispatchResult.BindingInstrumentation.GeneratedNativeS1FallbackCount;
			LastGeneratedS1RejectCount =
				DispatchResult.BindingInstrumentation.GeneratedNativeS1RejectCount;
			LastGeneratedFusedFastHitCount =
				DispatchResult.BindingInstrumentation.GeneratedFusedFastHitCount;
			LastGeneratedFusedRevalidateCount =
				DispatchResult.BindingInstrumentation.GeneratedFusedRevalidateCount;
			LastGeneratedFusedCallSitePrepareCount =
				DispatchResult.BindingInstrumentation.GeneratedFusedCallSitePrepareCount;
			LastGeneratedDirectReadPrepareCount =
				DispatchResult.BindingInstrumentation.GeneratedDirectReadPrepareCount;
			LastGeneratedDirectWritePrepareCount =
				DispatchResult.BindingInstrumentation.GeneratedDirectWritePrepareCount;
			LastGeneratedJournalSlowPathCount =
				DispatchResult.BindingInstrumentation.GeneratedJournalSlowPathCount;
			LastSemanticHitCount =
				DispatchResult.BindingInstrumentation.SemanticProcessEventCount;
			LastDataBoundaryCrossingCount =
				DispatchResult.DataBridgeMetrics.BoundaryCrossings;
			LastDataAppliedCommandCount =
				DispatchResult.DataBridgeMetrics.AppliedCommands;
			LastDataRejectedBufferCount =
				DispatchResult.DataBridgeMetrics.RejectedBuffers;
			return true;
		}

		bool PrepareCallbackWorkload(
			const uint32 Seed,
			FString& OutError)
		{
			int32 PackedWorkload = 0;
			if (!PrepareWorkload(
					EAvidScriptPerfWorkload::CallbackTick,
					1,
					Seed,
					PackedWorkload,
					OutError))
			{
				return false;
			}

			FAvidScriptWasmSmokeResult ResetResult;
			const bool bResetSucceeded =
				DispatchWorkload(PackedWorkload, Seed, ResetResult);
			uint32 IgnoredChecksum = 0;
			int32 IgnoredHostImportCallCount = 0;
			return CollectWorkloadResult(
				bResetSucceeded,
				ResetResult,
				IgnoredChecksum,
				IgnoredHostImportCallCount,
				OutError);
		}

		bool DispatchCallbackWorkload(
			const EAvidScriptPerfWorkload Workload,
			const int32 Iterations,
			const bool bUseHotCallbackResults,
			FAvidScriptWasmSmokeResult& OutDispatchResult,
			FString& OutError)
		{
			if (Workload == EAvidScriptPerfWorkload::CallbackEmpty)
			{
				int32 PackedWorkload = 0;
				if (!PrepareWorkload(
						Workload,
						1,
						0,
						PackedWorkload,
						OutError))
				{
					return false;
				}
				for (int32 Index = 0; Index < Iterations; ++Index)
				{
					const bool bSucceeded = bUseHotCallbackResults
						? Session.DispatchEventHot(
							PackedWorkload,
							static_cast<float>(Index),
							OutDispatchResult)
						: Session.DispatchEvent(
							PackedWorkload,
							static_cast<float>(Index),
							OutDispatchResult);
					if (!bSucceeded)
					{
						return false;
					}
				}
				return true;
			}
			if (Workload == EAvidScriptPerfWorkload::CallbackTick)
			{
				for (int32 Index = 0; Index < Iterations; ++Index)
				{
					const bool bSucceeded = bUseHotCallbackResults
						? Session.TickHot(
							PerfRunnerTickDeltaSeconds,
							OutDispatchResult)
						: Session.Tick(
							PerfRunnerTickDeltaSeconds,
							OutDispatchResult);
					if (!bSucceeded)
					{
						return false;
					}
				}
				return true;
			}

			OutError = TEXT("AvidScript callback dispatcher received a non-callback workload");
			return false;
		}

		bool CollectCallbackWorkload(
			const bool bDispatchSucceeded,
			FAvidScriptWasmSmokeResult& DispatchResult,
			uint32& OutChecksum,
			int32& OutHostImportCallCount,
			FString& OutError)
		{
			if (bDispatchSucceeded &&
				!Session.CaptureLiveSnapshot(DispatchResult))
			{
				OutError = FString::Printf(
					TEXT("AvidScript callback snapshot failed: %s"),
					*DispatchResult.ErrorMessage);
				return false;
			}
			return CollectWorkloadResult(
				bDispatchSucceeded,
				DispatchResult,
				OutChecksum,
				OutHostImportCallCount,
				OutError);
		}

		bool RunWorkload(
			const EAvidScriptPerfWorkload Workload,
			const int32 Iterations,
			const uint32 Seed,
			uint32& OutChecksum,
			int32& OutHostImportCallCount,
			FString& OutError)
		{
			int32 PackedWorkload = 0;
			if (!PrepareWorkload(
				Workload,
				Iterations,
				Seed,
				PackedWorkload,
				OutError))
			{
				return false;
			}

			FAvidScriptWasmSmokeResult DispatchResult;
			const bool bDispatchSucceeded =
				DispatchWorkload(PackedWorkload, Seed, DispatchResult);
			if (!CollectWorkloadResult(
				bDispatchSucceeded,
				DispatchResult,
				OutChecksum,
				OutHostImportCallCount,
				OutError))
			{
				return false;
			}
			return true;
		}
	};

	bool CreateBenchmarkWorld(UWorld*& OutWorld)
	{
		OutWorld = nullptr;
		if (GEngine == nullptr)
		{
			return false;
		}
		OutWorld = UWorld::CreateWorld(
			EWorldType::Game,
			false,
			TEXT("AvidScriptPhase53BenchmarkWorld"));
		if (OutWorld == nullptr)
		{
			return false;
		}
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(OutWorld);
		OutWorld->InitializeActorsForPlay(FURL());
		return true;
	}

	void DestroyBenchmarkWorld(UWorld*& World)
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

	bool IsCallbackWorkload(const EAvidScriptPerfWorkload Workload)
	{
		return Workload == EAvidScriptPerfWorkload::CallbackEmpty ||
			Workload == EAvidScriptPerfWorkload::CallbackTick;
	}

	uint32 PackVectorRefOutResult(
		const FVector& InOutValue,
		const FVector& OutValue)
	{
		return static_cast<uint32>(
			static_cast<int32>(InOutValue.X) +
			static_cast<int32>(InOutValue.Y) * 37 +
			static_cast<int32>(InOutValue.Z) * 101 +
			static_cast<int32>(OutValue.X) * 257 +
			static_cast<int32>(OutValue.Y) * 521 +
			static_cast<int32>(OutValue.Z) * 1031);
	}

	uint32 RunNativeWorkload(
		AAvidScriptPerfFixture& Fixture,
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations,
		const uint32 Seed)
	{
		uint32 Accumulator = Seed;
		for (int32 Index = 0; Index < Iterations; ++Index)
		{
			switch (Workload)
			{
			case EAvidScriptPerfWorkload::PureInteger:
				Accumulator = PerfRunnerMix(Accumulator ^ static_cast<uint32>(Index));
				break;
			case EAvidScriptPerfWorkload::ScalarNoOp:
				Accumulator = PerfRunnerMix(static_cast<uint32>(Fixture.NativeNoOp(static_cast<int32>(Accumulator))) ^
					static_cast<uint32>(Index));
				break;
			case EAvidScriptPerfWorkload::ScalarAddInt32:
				Accumulator = PerfRunnerMix(static_cast<uint32>(Fixture.NativeAddInt32(
					static_cast<int32>(Accumulator),
					Index)));
				break;
			case EAvidScriptPerfWorkload::PropertyGetSet:
				Fixture.ScalarValue = static_cast<int32>(Accumulator ^ static_cast<uint32>(Index));
				Accumulator = PerfRunnerMix(static_cast<uint32>(Fixture.ScalarValue));
				break;
			case EAvidScriptPerfWorkload::VectorValue:
			{
				const FVector Value(
					static_cast<double>(Index & 31),
					static_cast<double>((Index * 3) & 31),
					static_cast<double>((Index * 7) & 31));
				const FVector Result = Fixture.NativeVectorValue(Value);
				const uint32 Packed = static_cast<uint32>(Result.X) ^
					(static_cast<uint32>(Result.Y) << 8) ^
					(static_cast<uint32>(Result.Z) << 16);
				Accumulator = PerfRunnerMix(Accumulator ^ Packed);
				break;
			}
			case EAvidScriptPerfWorkload::ObjectRoundtrip:
				Accumulator = PerfRunnerMix(Accumulator ^
					(Fixture.NativeObjectRoundtrip(&Fixture) == &Fixture ? static_cast<uint32>(Index) : 0xffffffffu));
				break;
			case EAvidScriptPerfWorkload::BatchScalar:
				Accumulator = PerfRunnerMix(static_cast<uint32>(Fixture.NativeBatchAdd(
					static_cast<int32>(Accumulator),
					8)));
				break;
			case EAvidScriptPerfWorkload::CallbackEmpty:
				Fixture.NativeEmptyCallback(Index);
				Accumulator = static_cast<uint32>(
					Fixture.GetNativeCallbackChecksum());
				break;
			case EAvidScriptPerfWorkload::CallbackTick:
				Fixture.NativeTickCallback(PerfRunnerTickDeltaSeconds);
				Accumulator = static_cast<uint32>(
					Fixture.GetNativeCallbackChecksum());
				break;
			case EAvidScriptPerfWorkload::VectorRefOut:
			{
				FVector InOutValue(
					static_cast<double>(Index & 31),
					static_cast<double>((Index * 3) & 31),
					static_cast<double>((Index * 7) & 31));
				FVector OutValue = FVector::ZeroVector;
				Fixture.NativeVectorRefOut(InOutValue, OutValue);
				Accumulator = PerfRunnerMix(
					Accumulator ^
					PackVectorRefOutResult(InOutValue, OutValue));
				break;
			}
			case EAvidScriptPerfWorkload::GameplayFrameSmall:
			case EAvidScriptPerfWorkload::GameplayFrameDense:
				return FAvidScriptGameplayFrameBenchmark::RunNative(
					Fixture,
					Workload,
					Iterations,
					Seed);
			default:
				checkNoEntry();
				break;
			}
		}
		return Accumulator;
	}

	uint64 GetExpectedOperationCallCount(
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations)
	{
		switch (Workload)
		{
		case EAvidScriptPerfWorkload::ScalarNoOp:
		case EAvidScriptPerfWorkload::ScalarAddInt32:
		case EAvidScriptPerfWorkload::VectorValue:
		case EAvidScriptPerfWorkload::ObjectRoundtrip:
		case EAvidScriptPerfWorkload::BatchScalar:
		case EAvidScriptPerfWorkload::VectorRefOut:
			return static_cast<uint64>(Iterations);
		case EAvidScriptPerfWorkload::GameplayFrameSmall:
		case EAvidScriptPerfWorkload::GameplayFrameDense:
		{
			const FAvidScriptGameplayFrameCounts Counts =
				FAvidScriptGameplayFrameBenchmark::GetCounts(
					Workload,
					Iterations);
			return Counts.LogicalOperationCount - Counts.PropertyWriteCount;
		}
		default:
			return 0;
		}
	}

	int32 GetExpectedAvidScriptHostCallCount(
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations)
	{
		switch (Workload)
		{
		case EAvidScriptPerfWorkload::PropertyGetSet:
			return Iterations * 2 + 1;
		case EAvidScriptPerfWorkload::ScalarNoOp:
		case EAvidScriptPerfWorkload::ScalarAddInt32:
		case EAvidScriptPerfWorkload::VectorValue:
		case EAvidScriptPerfWorkload::ObjectRoundtrip:
		case EAvidScriptPerfWorkload::BatchScalar:
		case EAvidScriptPerfWorkload::VectorRefOut:
			return Iterations + 1;
		case EAvidScriptPerfWorkload::GameplayFrameSmall:
		case EAvidScriptPerfWorkload::GameplayFrameDense:
		{
			const uint64 LogicalOperations =
				FAvidScriptGameplayFrameBenchmark::GetCounts(
					Workload,
					Iterations).LogicalOperationCount;
			return LogicalOperations < static_cast<uint64>(MAX_int32)
				? static_cast<int32>(LogicalOperations + 1u)
				: MAX_int32;
		}
		default:
			return 0;
		}
	}

	uint64 GetExpectedLogicalOperationCount(
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations)
	{
		if (FAvidScriptGameplayFrameBenchmark::IsGameplayWorkload(Workload))
		{
			return FAvidScriptGameplayFrameBenchmark::GetCounts(
				Workload,
				Iterations).LogicalOperationCount;
		}
		return static_cast<uint64>(Iterations);
	}

	uint64 GetExpectedGeneratedS1HitCount(
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations)
	{
		if (FAvidScriptGameplayFrameBenchmark::IsGameplayWorkload(Workload))
		{
			return GetExpectedLogicalOperationCount(Workload, Iterations);
		}
		switch (Workload)
		{
		case EAvidScriptPerfWorkload::ScalarAddInt32:
		case EAvidScriptPerfWorkload::VectorValue:
		case EAvidScriptPerfWorkload::ObjectRoundtrip:
		case EAvidScriptPerfWorkload::BatchScalar:
			return static_cast<uint64>(Iterations);
		case EAvidScriptPerfWorkload::PropertyGetSet:
			return static_cast<uint64>(Iterations) * 2u;
		default:
			return 0;
		}
	}

	uint64 GetExpectedSemanticHitCount(
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations)
	{
		if (FAvidScriptGameplayFrameBenchmark::IsGameplayWorkload(Workload))
		{
			return GetExpectedLogicalOperationCount(Workload, Iterations);
		}
		switch (Workload)
		{
		case EAvidScriptPerfWorkload::ScalarNoOp:
		case EAvidScriptPerfWorkload::ScalarAddInt32:
		case EAvidScriptPerfWorkload::VectorValue:
		case EAvidScriptPerfWorkload::ObjectRoundtrip:
		case EAvidScriptPerfWorkload::BatchScalar:
		case EAvidScriptPerfWorkload::VectorRefOut:
			return static_cast<uint64>(Iterations);
		case EAvidScriptPerfWorkload::PropertyGetSet:
			return static_cast<uint64>(Iterations) * 2u;
		default:
			return 0;
		}
	}

	uint64 GetExpectedPropertyWriteCount(
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations)
	{
		if (!FAvidScriptGameplayFrameBenchmark::IsGameplayWorkload(Workload))
		{
			return 0;
		}
		return FAvidScriptGameplayFrameBenchmark::GetCounts(
			Workload,
			Iterations).PropertyWriteCount;
	}

	int32 GetExpectedDataOrientedHostCallCount(
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations)
	{
		if (!FAvidScriptGameplayFrameBenchmark::IsGameplayWorkload(Workload))
		{
			return GetExpectedAvidScriptHostCallCount(Workload, Iterations);
		}

		const FAvidScriptGameplayFrameCounts Counts =
			FAvidScriptGameplayFrameBenchmark::GetCounts(Workload, Iterations);
		const uint64 ExpectedHostCalls =
			1u +
			(Counts.LogicalOperationCount - Counts.PropertyWriteCount) +
			(Counts.PropertyWriteCount / 2u);
		return ExpectedHostCalls <= static_cast<uint64>(MAX_int32)
			? static_cast<int32>(ExpectedHostCalls)
			: MAX_int32;
	}

	enum class EAvidScriptPerfLane : uint8
	{
		NativeCpp,
		PuertsV8Reflection,
		PuertsV8Static,
		AvidScriptWasmtimeSemantic,
		AvidScriptWasmtimeGeneratedS1,
		AvidScriptWasmtimeDataOriented
	};

	enum class EAvidScriptPerfBenchmarkMode : uint8
	{
		Combined,
		Calibrate,
		Timed
	};

	constexpr int32 PerfRunnerLaneCount = 6;

	struct FPerfLaneCatalogEntry
	{
		EAvidScriptPerfLane Lane = EAvidScriptPerfLane::NativeCpp;
		FString LaneIdentitySha256;
		FString BackendId;
		FString BindingInvocationMode;
		FString RuntimeVersion;
		FString ExecutionMode;
		FString ArtifactFormat;
		FString ArtifactSha256;
		FString SourceWasmSha256;
		FString TargetTriple;
		FString RuntimeBuildIdentity;
		FString RuntimeArtifactSha256;
		FString ManifestRelativePath;
		TSharedPtr<FJsonObject> Json;
	};

	struct FPerfBenchmarkRequest
	{
		EAvidScriptPerfBenchmarkMode Mode = EAvidScriptPerfBenchmarkMode::Combined;
		FString AttemptId;
		FString RequestSha256;
		int32 ProcessRun = 0;
		int32 Seed = 0;
		TArray<EAvidScriptPerfLane, TInlineAllocator<PerfRunnerLaneCount>> LaneOrder;
		TArray<EAvidScriptPerfWorkload> Workloads;
		int32 WarmupSamples = 0;
		int32 TimedSamples = 0;
		double MinimumSampleMilliseconds = 0.0;
		int32 MinimumIterations = 0;
		int32 MaximumIterations = 0;
		int32 CalibrationConfirmationSamples = 0;
		double DataLaneMaxCrossingRatio = 0.0;
		bool bUseHotCallbackResults = true;
		TSharedPtr<FJsonObject> Provenance;
		TArray<TSharedPtr<FJsonValue>> LaneCatalogJson;
		TArray<FPerfLaneCatalogEntry, TInlineAllocator<PerfRunnerLaneCount>> LaneCatalog;
		FString LaneCatalogSha256;
		FString EditorExecutableSha256;
		TArray<int32> FrozenIterationCounts;
		FString TemporaryResultPath;
	};

	struct FPerfLaneObservation
	{
		EAvidScriptPerfLane Lane = EAvidScriptPerfLane::NativeCpp;
		int32 LanePosition = 0;
		int32 Iterations = 0;
		uint64 ElapsedCycles = 0;
		uint32 Checksum = 0;
		uint32 ExpectedChecksum = 0;
		int32 FinalScalar = 0;
		int32 ExpectedFinalScalar = 0;
		uint64 OperationCallCount = 0;
		uint64 ExpectedOperationCallCount = 0;
		int32 HostImportCallCount = 0;
		int32 ExpectedHostImportCallCount = 0;
		uint64 DirectHitCount = 0;
		uint64 RequestedDirectFallbackCount = 0;
		uint64 GeneratedS1HitCount = 0;
		uint64 GeneratedS1FallbackCount = 0;
		uint64 GeneratedS1RejectCount = 0;
		uint64 GeneratedFusedFastHitCount = 0;
		uint64 GeneratedFusedRevalidateCount = 0;
		uint64 GeneratedFusedCallSitePrepareCount = 0;
		uint64 GeneratedDirectReadPrepareCount = 0;
		uint64 GeneratedDirectWritePrepareCount = 0;
		uint64 GeneratedJournalSlowPathCount = 0;
		uint64 DataLaneCommandCount = 0;
		uint64 DataLaneCrossingCount = 0;
		uint64 DataLaneRejectedBufferCount = 0;
		uint64 SemanticHitCount = 0;
		uint64 LogicalOperationCount = 0;
		uint64 MemoryUsedBeforeBytes = 0;
		uint64 MemoryUsedAfterBytes = 0;
		uint64 PeakMemoryBytes = 0;
		TSharedPtr<FJsonObject> BackendInfo;
	};

	struct FPerfOracle
	{
		uint32 Checksum = 0;
		int32 FinalScalar = 0;
		uint64 OperationCallCount = 0;
	};

	struct FPerfSample
	{
		int32 ProcessRun = 0;
		EAvidScriptPerfLane Lane = EAvidScriptPerfLane::NativeCpp;
		FString LaneIdentitySha256;
		int32 LanePosition = 0;
		EAvidScriptPerfWorkload Workload = EAvidScriptPerfWorkload::PureInteger;
		int32 SampleIndex = 0;
		uint32 Seed = 0;
		int32 Iterations = 0;
		uint64 ElapsedCycles = 0;
		uint32 Checksum = 0;
		uint32 ExpectedChecksum = 0;
		int32 FinalScalar = 0;
		int32 ExpectedFinalScalar = 0;
		uint64 OperationCallCount = 0;
		uint64 ExpectedOperationCallCount = 0;
		int32 HostImportCallCount = 0;
		int32 ExpectedHostImportCallCount = 0;
		uint64 DirectHitCount = 0;
		uint64 RequestedDirectFallbackCount = 0;
		uint64 GeneratedS1HitCount = 0;
		uint64 GeneratedS1FallbackCount = 0;
		uint64 GeneratedS1RejectCount = 0;
		uint64 GeneratedFusedFastHitCount = 0;
		uint64 GeneratedFusedRevalidateCount = 0;
		uint64 GeneratedFusedCallSitePrepareCount = 0;
		uint64 GeneratedDirectReadPrepareCount = 0;
		uint64 GeneratedDirectWritePrepareCount = 0;
		uint64 GeneratedJournalSlowPathCount = 0;
		uint64 DataLaneCommandCount = 0;
		uint64 DataLaneCrossingCount = 0;
		uint64 DataLaneRejectedBufferCount = 0;
		uint64 SemanticHitCount = 0;
		uint64 LogicalOperationCount = 0;
		uint64 MemoryUsedBeforeBytes = 0;
		uint64 MemoryUsedAfterBytes = 0;
		uint64 PeakMemoryBytes = 0;
		TSharedPtr<FJsonObject> BackendInfo;
	};

	int32 GetPerfIterationMatrixIndex(
		const int32 WorkloadIndex,
		const EAvidScriptPerfLane Lane)
	{
		return WorkloadIndex * PerfRunnerLaneCount +
			static_cast<int32>(Lane);
	}

	double GetSteadyStateMedianMilliseconds(TArray<double> Samples)
	{
		check(!Samples.IsEmpty());
		Samples.Sort();
		return Samples[Samples.Num() / 2];
	}

	const TCHAR* GetPerfLaneName(const EAvidScriptPerfLane Lane)
	{
		switch (Lane)
		{
		case EAvidScriptPerfLane::NativeCpp:
			return TEXT("native_cpp");
		case EAvidScriptPerfLane::PuertsV8Reflection:
			return TEXT("puerts_v8_reflection");
		case EAvidScriptPerfLane::PuertsV8Static:
			return TEXT("puerts_v8_static");
		case EAvidScriptPerfLane::AvidScriptWasmtimeSemantic:
			return TEXT("avidscript_wasmtime_semantic");
		case EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1:
			return TEXT("avidscript_wasmtime_generated_s1");
		case EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented:
			return TEXT("avidscript_wasmtime_data_oriented");
		default:
			checkNoEntry();
			return TEXT("");
		}
	}

	bool IsAvidScriptPerfLane(const EAvidScriptPerfLane Lane)
	{
		return Lane == EAvidScriptPerfLane::AvidScriptWasmtimeSemantic ||
			Lane == EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1 ||
			Lane == EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented;
	}

	struct FAvidScriptLaneSet
	{
		FAvidScriptLane WasmtimeSemantic;
		FAvidScriptLane WasmtimeGeneratedS1;
		FAvidScriptLane WasmtimeDataOriented;

		FAvidScriptLane& Get(const EAvidScriptPerfLane Lane)
		{
			check(IsAvidScriptPerfLane(Lane));
			switch (Lane)
			{
			case EAvidScriptPerfLane::AvidScriptWasmtimeSemantic:
				return WasmtimeSemantic;
			case EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1:
				return WasmtimeGeneratedS1;
			case EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented:
				return WasmtimeDataOriented;
			default:
				checkNoEntry();
				return WasmtimeSemantic;
			}
		}
	};

	bool TryParsePerfLane(const FString& Name, EAvidScriptPerfLane& OutLane)
	{
		for (int32 LaneIndex = 0; LaneIndex < PerfRunnerLaneCount; ++LaneIndex)
		{
			const EAvidScriptPerfLane Lane =
				static_cast<EAvidScriptPerfLane>(LaneIndex);
			if (Name.Equals(GetPerfLaneName(Lane), ESearchCase::CaseSensitive))
			{
				OutLane = Lane;
				return true;
			}
		}
		return false;
	}

	const TCHAR* GetPerfWorkloadName(const EAvidScriptPerfWorkload Workload)
	{
		switch (Workload)
		{
		case EAvidScriptPerfWorkload::PureInteger:
			return TEXT("pure_integer");
		case EAvidScriptPerfWorkload::ScalarNoOp:
			return TEXT("scalar_noop");
		case EAvidScriptPerfWorkload::ScalarAddInt32:
			return TEXT("scalar_add_int32");
		case EAvidScriptPerfWorkload::PropertyGetSet:
			return TEXT("property_get_set");
		case EAvidScriptPerfWorkload::VectorValue:
			return TEXT("vector_value");
		case EAvidScriptPerfWorkload::ObjectRoundtrip:
			return TEXT("object_roundtrip");
		case EAvidScriptPerfWorkload::BatchScalar:
			return TEXT("batch_scalar");
		case EAvidScriptPerfWorkload::CallbackEmpty:
			return TEXT("callback_empty");
		case EAvidScriptPerfWorkload::CallbackTick:
			return TEXT("callback_tick");
		case EAvidScriptPerfWorkload::VectorRefOut:
			return TEXT("vector_ref_out");
		case EAvidScriptPerfWorkload::GameplayFrameSmall:
			return TEXT("gameplay_frame_small");
		case EAvidScriptPerfWorkload::GameplayFrameDense:
			return TEXT("gameplay_frame_dense");
		default:
			checkNoEntry();
			return TEXT("");
		}
	}

	bool TryParsePerfWorkload(
		const FString& Name,
		EAvidScriptPerfWorkload& OutWorkload)
	{
		constexpr int32 WorkloadCount =
			static_cast<int32>(EAvidScriptPerfWorkload::Count);
		for (int32 WorkloadIndex = 0; WorkloadIndex < WorkloadCount; ++WorkloadIndex)
		{
			const EAvidScriptPerfWorkload Workload =
				static_cast<EAvidScriptPerfWorkload>(WorkloadIndex);
			if (Name.Equals(
				GetPerfWorkloadName(Workload),
				ESearchCase::CaseSensitive))
			{
				OutWorkload = Workload;
				return true;
			}
		}
		return false;
	}

	bool TryGetRequiredString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName,
		FString& OutValue,
		FString& OutError)
	{
		if (!Object.IsValid() ||
			!Object->TryGetStringField(FieldName, OutValue) ||
			OutValue.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("request field '%s' must be a non-empty string"),
				FieldName);
			return false;
		}
		return true;
	}

	bool TryGetRequiredInteger(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName,
		const int64 Minimum,
		const int64 Maximum,
		int64& OutValue,
		FString& OutError)
	{
		double Number = 0.0;
		if (!Object.IsValid() ||
			!Object->TryGetNumberField(FieldName, Number) ||
			!FMath::IsFinite(Number) ||
			FMath::Floor(Number) != Number ||
			Number < static_cast<double>(Minimum) ||
			Number > static_cast<double>(Maximum))
		{
			OutError = FString::Printf(
				TEXT("request field '%s' must be an integer in [%lld, %lld]"),
				FieldName,
				Minimum,
				Maximum);
			return false;
		}
		OutValue = static_cast<int64>(Number);
		return true;
	}

	bool TryGetRequiredStringArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() ||
			!Object->TryGetArrayField(FieldName, Values) ||
			Values == nullptr ||
			Values->IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("request field '%s' must be a non-empty string array"),
				FieldName);
			return false;
		}

		OutValues.Reset(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (!Value.IsValid() ||
				!Value->TryGetString(StringValue) ||
				StringValue.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("request field '%s' must contain only non-empty strings"),
					FieldName);
				return false;
			}
			OutValues.Add(MoveTemp(StringValue));
		}
		return true;
	}

	bool ParsePerfBenchmarkRequest(
		const FString& RequestPath,
		const FString& ResultPath,
		FPerfBenchmarkRequest& OutRequest,
		FString& OutError)
	{
		FString RequestJson;
		if (!FFileHelper::LoadFileToString(RequestJson, *RequestPath))
		{
			OutError = FString::Printf(
				TEXT("unable to read benchmark request: %s"),
				*RequestPath);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(RequestJson);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("benchmark request is not a JSON object");
			return false;
		}

		int64 IntegerValue = 0;
		if (!TryGetRequiredInteger(
				Root,
				TEXT("schema_version"),
				PerfRunnerResultSchemaVersion,
				PerfRunnerResultSchemaVersion,
				IntegerValue,
				OutError))
		{
			return false;
		}

		FString ModeName;
		if (Root->TryGetStringField(TEXT("mode"), ModeName))
		{
			if (ModeName.Equals(TEXT("calibrate"), ESearchCase::CaseSensitive) ||
				ModeName.Equals(TEXT("calibration"), ESearchCase::CaseSensitive))
			{
				OutRequest.Mode = EAvidScriptPerfBenchmarkMode::Calibrate;
			}
			else if (ModeName.Equals(TEXT("timed"), ESearchCase::CaseSensitive))
			{
				OutRequest.Mode = EAvidScriptPerfBenchmarkMode::Timed;
			}
			else
			{
				OutError = TEXT("request mode must be 'calibrate' or 'timed'");
				return false;
			}
		}
		else if (Root->HasField(TEXT("mode")))
		{
			OutError = TEXT("request mode must be a string");
			return false;
		}

		if (!TryGetRequiredString(
				Root,
				TEXT("attempt_id"),
				OutRequest.AttemptId,
				OutError))
		{
			return false;
		}
		FString CallbackResultMode;
		if (!TryGetRequiredString(
				Root,
				TEXT("callback_result_mode"),
				CallbackResultMode,
				OutError))
		{
			return false;
		}
		if (CallbackResultMode.Equals(
				TEXT("hot_failure_only"),
				ESearchCase::CaseSensitive))
		{
			OutRequest.bUseHotCallbackResults = true;
		}
		else if (CallbackResultMode.Equals(
				TEXT("full_snapshot"),
				ESearchCase::CaseSensitive))
		{
			OutRequest.bUseHotCallbackResults = false;
		}
		else
		{
			OutError = TEXT(
				"request callback_result_mode must be 'hot_failure_only' or 'full_snapshot'");
			return false;
		}
		FGuid AttemptId;
		if (!FGuid::Parse(OutRequest.AttemptId, AttemptId))
		{
			OutError = TEXT("request attempt_id must be a GUID");
			return false;
		}

		if (!TryGetRequiredInteger(
				Root,
				TEXT("process_run"),
				-1,
				MAX_int32,
				IntegerValue,
				OutError))
		{
			return false;
		}
		OutRequest.ProcessRun = static_cast<int32>(IntegerValue);

		if (!TryGetRequiredInteger(
				Root,
				TEXT("seed"),
				MIN_int32,
				MAX_int32,
				IntegerValue,
				OutError))
		{
			return false;
		}
		OutRequest.Seed = static_cast<int32>(IntegerValue);

		TArray<FString> LaneNames;
		if (!TryGetRequiredStringArray(
				Root,
				TEXT("lane_order"),
				LaneNames,
				OutError) ||
			LaneNames.Num() != PerfRunnerLaneCount)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("request lane_order must contain exactly six lanes");
			}
			return false;
		}
		TSet<int32> UniqueLanes;
		for (const FString& LaneName : LaneNames)
		{
			EAvidScriptPerfLane Lane = EAvidScriptPerfLane::NativeCpp;
			if (!TryParsePerfLane(LaneName, Lane) ||
				UniqueLanes.Contains(static_cast<int32>(Lane)))
			{
				OutError = TEXT(
					"request lane_order must contain each supported lane exactly once");
				return false;
			}
			UniqueLanes.Add(static_cast<int32>(Lane));
			OutRequest.LaneOrder.Add(Lane);
		}

		TArray<FString> RequestedLaneNames;
		if (!TryGetRequiredStringArray(
				Root,
				TEXT("lanes"),
				RequestedLaneNames,
				OutError) ||
			RequestedLaneNames.Num() != PerfRunnerLaneCount)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("request lanes must contain exactly six lanes");
			}
			return false;
		}
		TSet<int32> RequestedLanes;
		for (const FString& LaneName : RequestedLaneNames)
		{
			EAvidScriptPerfLane Lane = EAvidScriptPerfLane::NativeCpp;
			if (!TryParsePerfLane(LaneName, Lane) ||
				RequestedLanes.Contains(static_cast<int32>(Lane)))
			{
				OutError = TEXT(
					"request lanes must contain each supported lane exactly once");
				return false;
			}
			RequestedLanes.Add(static_cast<int32>(Lane));
		}

		const TArray<TSharedPtr<FJsonValue>>* LaneCatalogValues = nullptr;
		if (!Root->TryGetArrayField(TEXT("lane_catalog"), LaneCatalogValues) ||
			LaneCatalogValues == nullptr ||
			LaneCatalogValues->Num() != PerfRunnerLaneCount ||
			!TryGetRequiredString(
				Root,
				TEXT("lane_catalog_sha256"),
				OutRequest.LaneCatalogSha256,
				OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("request lane_catalog must contain exactly six entries");
			}
			return false;
		}
		OutRequest.LaneCatalogJson = *LaneCatalogValues;
		for (int32 LaneIndex = 0; LaneIndex < PerfRunnerLaneCount; ++LaneIndex)
		{
			const TSharedPtr<FJsonObject> EntryJson =
				(*LaneCatalogValues)[LaneIndex].IsValid()
					? (*LaneCatalogValues)[LaneIndex]->AsObject()
					: nullptr;
			FPerfLaneCatalogEntry Entry;
			FString LaneName;
			bool bFallbackUsed = true;
			if (!EntryJson.IsValid() ||
				!TryGetRequiredString(EntryJson, TEXT("lane_id"), LaneName, OutError) ||
				!TryParsePerfLane(LaneName, Entry.Lane) ||
				static_cast<int32>(Entry.Lane) != LaneIndex ||
				!TryGetRequiredString(EntryJson, TEXT("lane_identity_sha256"), Entry.LaneIdentitySha256, OutError) ||
				!TryGetRequiredString(EntryJson, TEXT("runtime_version"), Entry.RuntimeVersion, OutError) ||
				!TryGetRequiredString(EntryJson, TEXT("execution_mode"), Entry.ExecutionMode, OutError) ||
				!TryGetRequiredString(EntryJson, TEXT("execution_artifact_format"), Entry.ArtifactFormat, OutError) ||
				!TryGetRequiredString(EntryJson, TEXT("execution_artifact_sha256"), Entry.ArtifactSha256, OutError) ||
				!TryGetRequiredString(EntryJson, TEXT("target_triple"), Entry.TargetTriple, OutError) ||
				!TryGetRequiredString(EntryJson, TEXT("runtime_build_identity"), Entry.RuntimeBuildIdentity, OutError) ||
				!TryGetRequiredString(EntryJson, TEXT("runtime_artifact_sha256"), Entry.RuntimeArtifactSha256, OutError) ||
				!EntryJson->TryGetBoolField(TEXT("fallback_used"), bFallbackUsed) ||
				bFallbackUsed)
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("request lane_catalog entry is invalid or permits fallback: index=%d"),
						LaneIndex);
				}
				return false;
			}
			if (IsAvidScriptPerfLane(Entry.Lane) &&
				(!TryGetRequiredString(EntryJson, TEXT("backend_id"), Entry.BackendId, OutError) ||
				 !TryGetRequiredString(EntryJson, TEXT("binding_invocation_mode"), Entry.BindingInvocationMode, OutError) ||
				 !TryGetRequiredString(EntryJson, TEXT("source_wasm_sha256"), Entry.SourceWasmSha256, OutError) ||
				 !TryGetRequiredString(EntryJson, TEXT("manifest_relative_path"), Entry.ManifestRelativePath, OutError)))
			{
				return false;
			}
			FString ComputedLaneIdentitySha256;
			if (!GetCanonicalLaneIdentitySha256(
					EntryJson,
					ComputedLaneIdentitySha256,
					OutError) ||
				!ComputedLaneIdentitySha256.Equals(
					Entry.LaneIdentitySha256,
					ESearchCase::CaseSensitive))
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("request lane identity hash mismatch: lane=%s"),
						*LaneName);
				}
				return false;
			}
			Entry.Json = EntryJson;
			OutRequest.LaneCatalog.Add(MoveTemp(Entry));
		}
		const FPerfLaneCatalogEntry& SemanticEntry =
			OutRequest.LaneCatalog[static_cast<int32>(
				EAvidScriptPerfLane::AvidScriptWasmtimeSemantic)];
		const FPerfLaneCatalogEntry& GeneratedEntry =
			OutRequest.LaneCatalog[static_cast<int32>(
				EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1)];
		const FPerfLaneCatalogEntry& DataEntry =
			OutRequest.LaneCatalog[static_cast<int32>(
				EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented)];
		if (!SemanticEntry.BackendId.Equals(TEXT("wasmtime.cranelift.jit"), ESearchCase::CaseSensitive) ||
			!GeneratedEntry.BackendId.Equals(TEXT("wasmtime.cranelift.jit"), ESearchCase::CaseSensitive) ||
			!DataEntry.BackendId.Equals(TEXT("wasmtime.cranelift.jit"), ESearchCase::CaseSensitive) ||
			!SemanticEntry.BindingInvocationMode.Equals(TEXT("semantic_process_event"), ESearchCase::CaseSensitive) ||
			!GeneratedEntry.BindingInvocationMode.Equals(TEXT("generated_native_s1"), ESearchCase::CaseSensitive) ||
			!DataEntry.BindingInvocationMode.Equals(TEXT("data_command_buffer"), ESearchCase::CaseSensitive) ||
			SemanticEntry.LaneIdentitySha256.Equals(GeneratedEntry.LaneIdentitySha256, ESearchCase::CaseSensitive) ||
			SemanticEntry.LaneIdentitySha256.Equals(DataEntry.LaneIdentitySha256, ESearchCase::CaseSensitive) ||
			GeneratedEntry.LaneIdentitySha256.Equals(DataEntry.LaneIdentitySha256, ESearchCase::CaseSensitive) ||
			!SemanticEntry.RuntimeBuildIdentity.Equals(GeneratedEntry.RuntimeBuildIdentity, ESearchCase::CaseSensitive) ||
			!SemanticEntry.RuntimeBuildIdentity.Equals(DataEntry.RuntimeBuildIdentity, ESearchCase::CaseSensitive) ||
			!SemanticEntry.RuntimeArtifactSha256.Equals(GeneratedEntry.RuntimeArtifactSha256, ESearchCase::CaseSensitive) ||
			!SemanticEntry.RuntimeArtifactSha256.Equals(DataEntry.RuntimeArtifactSha256, ESearchCase::CaseSensitive))
		{
			OutError = TEXT(
				"request Wasmtime lanes must use one runtime build and three distinct invocation identities");
			return false;
		}
		FString ComputedLaneCatalogSha256;
		if (!GetCanonicalLaneCatalogSha256(
				*LaneCatalogValues,
				ComputedLaneCatalogSha256,
				OutError) ||
			!ComputedLaneCatalogSha256.Equals(
				OutRequest.LaneCatalogSha256,
				ESearchCase::CaseSensitive))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("request lane catalog hash mismatch");
			}
			return false;
		}

		TArray<FString> WorkloadNames;
		if (!TryGetRequiredStringArray(
				Root,
				TEXT("workloads"),
				WorkloadNames,
				OutError))
		{
			return false;
		}
		TSet<int32> UniqueWorkloads;
		for (const FString& WorkloadName : WorkloadNames)
		{
			EAvidScriptPerfWorkload Workload =
				EAvidScriptPerfWorkload::PureInteger;
			if (!TryParsePerfWorkload(WorkloadName, Workload))
			{
				OutError = FString::Printf(
					TEXT("unknown or unsupported warm workload: %s"),
					*WorkloadName);
				return false;
			}
			if (UniqueWorkloads.Contains(static_cast<int32>(Workload)))
			{
				OutError = FString::Printf(
					TEXT("duplicate warm workload: %s"),
					*WorkloadName);
				return false;
			}
			UniqueWorkloads.Add(static_cast<int32>(Workload));
			OutRequest.Workloads.Add(Workload);
		}

		if (!TryGetRequiredInteger(
				Root,
				TEXT("warmup_samples"),
				0,
				MAX_int32,
				IntegerValue,
				OutError))
		{
			return false;
		}
		OutRequest.WarmupSamples = static_cast<int32>(IntegerValue);
		if (!TryGetRequiredInteger(
				Root,
				TEXT("timed_samples"),
				0,
				MAX_int32,
				IntegerValue,
				OutError))
		{
			return false;
		}
		OutRequest.TimedSamples = static_cast<int32>(IntegerValue);
		if (OutRequest.Mode == EAvidScriptPerfBenchmarkMode::Calibrate)
		{
			if (OutRequest.ProcessRun != -1 || OutRequest.TimedSamples != 0)
			{
				OutError =
					TEXT("calibration mode requires process_run=-1 and timed_samples=0");
				return false;
			}
		}
		else if (OutRequest.ProcessRun < 0 || OutRequest.TimedSamples < 1)
		{
			OutError =
				TEXT("timed or combined mode requires process_run>=0 and timed_samples>=1");
			return false;
		}
		const int64 TimedSampleCount =
			static_cast<int64>(OutRequest.Workloads.Num()) *
			OutRequest.TimedSamples *
			PerfRunnerLaneCount;
		if (TimedSampleCount > MAX_int32)
		{
			OutError = TEXT("requested timed sample matrix is too large");
			return false;
		}

		if (!Root->TryGetNumberField(
				TEXT("minimum_sample_milliseconds"),
				OutRequest.MinimumSampleMilliseconds) ||
			!FMath::IsFinite(OutRequest.MinimumSampleMilliseconds) ||
			OutRequest.MinimumSampleMilliseconds <= 0.0)
		{
			OutError =
				TEXT("request minimum_sample_milliseconds must be positive");
			return false;
		}
		if (!GetPerfFileSha256(
				RequestPath,
				OutRequest.RequestSha256,
				OutError))
		{
			return false;
		}
		if (!Root->TryGetNumberField(
				TEXT("data_lane_max_crossing_ratio"),
				OutRequest.DataLaneMaxCrossingRatio) ||
			!FMath::IsFinite(OutRequest.DataLaneMaxCrossingRatio) ||
			OutRequest.DataLaneMaxCrossingRatio <= 0.0 ||
			OutRequest.DataLaneMaxCrossingRatio >= 1.0)
		{
			OutError =
				TEXT("request data_lane_max_crossing_ratio must be in (0, 1)");
			return false;
		}

		if (!TryGetRequiredInteger(
				Root,
				TEXT("minimum_iterations"),
				1,
				PerfRunnerIterationMask,
				IntegerValue,
				OutError))
		{
			return false;
		}
		OutRequest.MinimumIterations = static_cast<int32>(IntegerValue);
		if (!TryGetRequiredInteger(
				Root,
				TEXT("maximum_iterations"),
				OutRequest.MinimumIterations,
				PerfRunnerIterationMask,
				IntegerValue,
				OutError))
		{
			return false;
		}
		OutRequest.MaximumIterations = static_cast<int32>(IntegerValue);
		if (!TryGetRequiredInteger(
				Root,
				TEXT("calibration_confirmation_samples"),
				1,
				31,
				IntegerValue,
				OutError))
		{
			return false;
		}
		OutRequest.CalibrationConfirmationSamples =
			static_cast<int32>(IntegerValue);

		const TSharedPtr<FJsonObject>* ResultSchema = nullptr;
		if (!Root->TryGetObjectField(TEXT("result_schema"), ResultSchema) ||
			ResultSchema == nullptr ||
			!ResultSchema->IsValid())
		{
			OutError = TEXT("request result_schema must be an object");
			return false;
		}
		if (!TryGetRequiredInteger(
				*ResultSchema,
				TEXT("version"),
				PerfRunnerResultSchemaVersion,
				PerfRunnerResultSchemaVersion,
				IntegerValue,
				OutError))
		{
			return false;
		}
		FString ResultSchemaSha256;
		if (!TryGetRequiredString(
				*ResultSchema,
				TEXT("sha256"),
				ResultSchemaSha256,
				OutError))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Provenance = nullptr;
		if (!Root->TryGetObjectField(TEXT("provenance"), Provenance) ||
			Provenance == nullptr ||
			!Provenance->IsValid())
		{
			OutError = TEXT("request provenance must be an object");
			return false;
		}
		OutRequest.Provenance = *Provenance;
		if (!TryGetRequiredString(
				OutRequest.Provenance,
				TEXT("editor_executable_sha256"),
				OutRequest.EditorExecutableSha256,
				OutError))
		{
			return false;
		}
		FString ProvenanceLaneCatalogSha256;
		if (!TryGetRequiredString(
				OutRequest.Provenance,
				TEXT("lane_catalog_sha256"),
				ProvenanceLaneCatalogSha256,
				OutError) ||
			!ProvenanceLaneCatalogSha256.Equals(
				OutRequest.LaneCatalogSha256,
				ESearchCase::CaseSensitive))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("request lane_catalog_sha256 must equal provenance lane_catalog_sha256");
			}
			return false;
		}
		const TCHAR* ExpectedProvenanceSchemaHashField =
			OutRequest.Mode == EAvidScriptPerfBenchmarkMode::Calibrate
				? TEXT("calibration_schema_sha256")
				: TEXT("result_schema_sha256");
		FString ExpectedProvenanceSchemaSha256;
		if (!TryGetRequiredString(
				OutRequest.Provenance,
				ExpectedProvenanceSchemaHashField,
				ExpectedProvenanceSchemaSha256,
				OutError))
		{
			return false;
		}
		if (!ResultSchemaSha256.Equals(
				ExpectedProvenanceSchemaSha256,
				ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("request result_schema.sha256 must equal selected provenance schema hash: field=%s"),
				ExpectedProvenanceSchemaHashField);
			return false;
		}

		FString RequestedResultPath;
		if (!TryGetRequiredString(
				Root,
				TEXT("result_path"),
				RequestedResultPath,
				OutError))
		{
			return false;
		}
		RequestedResultPath = FPaths::ConvertRelativePathToFull(RequestedResultPath);
		FPaths::NormalizeFilename(RequestedResultPath);
		FString NormalizedResultPath =
			FPaths::ConvertRelativePathToFull(ResultPath);
		FPaths::NormalizeFilename(NormalizedResultPath);
		if (!RequestedResultPath.Equals(
			NormalizedResultPath,
			ESearchCase::IgnoreCase))
		{
			OutError =
				TEXT("request result_path does not match -AvidScriptPerfResult");
			return false;
		}

		const TSharedPtr<FJsonObject>* IterationCounts = nullptr;
		const bool bHasIterationCounts =
			Root->TryGetObjectField(TEXT("iteration_counts"), IterationCounts) &&
			IterationCounts != nullptr &&
			IterationCounts->IsValid();
		if (OutRequest.Mode == EAvidScriptPerfBenchmarkMode::Calibrate)
		{
			if (!bHasIterationCounts || !(*IterationCounts)->Values.IsEmpty())
			{
				OutError =
					TEXT("calibration mode requires an empty iteration_counts mapping");
				return false;
			}
		}
		else if (OutRequest.Mode == EAvidScriptPerfBenchmarkMode::Timed)
		{
			if (!bHasIterationCounts ||
				(*IterationCounts)->Values.Num() != OutRequest.Workloads.Num())
			{
				OutError =
					TEXT("timed mode requires an exact iteration_counts mapping");
				return false;
			}
			OutRequest.FrozenIterationCounts.Reserve(
				OutRequest.Workloads.Num() * PerfRunnerLaneCount);
			for (const EAvidScriptPerfWorkload Workload : OutRequest.Workloads)
			{
				const FString WorkloadName = GetPerfWorkloadName(Workload);
				const TSharedPtr<FJsonObject>* LaneIterationCounts = nullptr;
				if (!(*IterationCounts)->TryGetObjectField(
						WorkloadName,
						LaneIterationCounts) ||
					LaneIterationCounts == nullptr ||
					!LaneIterationCounts->IsValid() ||
					(*LaneIterationCounts)->Values.Num() != PerfRunnerLaneCount)
				{
					OutError =
						TEXT("timed mode requires an exact iteration_counts mapping");
					return false;
				}
				for (int32 LaneIndex = 0;
					LaneIndex < PerfRunnerLaneCount;
					++LaneIndex)
				{
					const EAvidScriptPerfLane Lane =
						static_cast<EAvidScriptPerfLane>(LaneIndex);
					if (!TryGetRequiredInteger(
							*LaneIterationCounts,
							GetPerfLaneName(Lane),
							OutRequest.MinimumIterations,
							OutRequest.MaximumIterations,
							IntegerValue,
							OutError))
					{
						OutError =
							TEXT("timed mode requires an exact iteration_counts mapping");
						return false;
					}
					OutRequest.FrozenIterationCounts.Add(
						static_cast<int32>(IntegerValue));
				}
			}
		}

		const TSharedPtr<FJsonObject>* ResultWrite = nullptr;
		FString WriteStrategy;
		bool bOverwrite = true;
		if (!Root->TryGetObjectField(TEXT("result_write"), ResultWrite) ||
			ResultWrite == nullptr ||
			!ResultWrite->IsValid() ||
			!TryGetRequiredString(
				*ResultWrite,
				TEXT("strategy"),
				WriteStrategy,
				OutError) ||
			!WriteStrategy.Equals(
				TEXT("same_directory_temporary_then_atomic_rename"),
				ESearchCase::CaseSensitive) ||
			!TryGetRequiredString(
				*ResultWrite,
				TEXT("temporary_path"),
				OutRequest.TemporaryResultPath,
				OutError) ||
			!(*ResultWrite)->TryGetBoolField(TEXT("overwrite"), bOverwrite) ||
			bOverwrite)
		{
			OutError =
				TEXT("request result_write must require same-directory atomic no-overwrite publication");
			return false;
		}
		if (FPaths::IsRelative(OutRequest.TemporaryResultPath))
		{
			OutError =
				TEXT("request result_write temporary_path must be absolute");
			return false;
		}
		OutRequest.TemporaryResultPath =
			FPaths::ConvertRelativePathToFull(OutRequest.TemporaryResultPath);
		FPaths::NormalizeFilename(OutRequest.TemporaryResultPath);
		if (!FPaths::GetPath(OutRequest.TemporaryResultPath).Equals(
				FPaths::GetPath(NormalizedResultPath),
				ESearchCase::IgnoreCase) ||
			OutRequest.TemporaryResultPath.Equals(
				NormalizedResultPath,
				ESearchCase::IgnoreCase))
		{
			OutError =
				TEXT("request result_write temporary_path must be a distinct file in the result directory");
			return false;
		}
		return true;
	}

	uint32 MakePerfRunnerSampleSeed(
		const int32 Seed,
		const int32 WorkloadIndex,
		const int32 SampleIndex)
	{
		return PerfRunnerMix(
			static_cast<uint32>(Seed) ^
			(static_cast<uint32>(WorkloadIndex + 1) * 0x9e3779b9u) ^
			static_cast<uint32>(SampleIndex + 1)) & PerfRunnerExactSeedMask;
	}

	int32 PositiveModulo(const int64 Value, const int32 Modulus)
	{
		const int32 Remainder = static_cast<int32>(Value % Modulus);
		return Remainder < 0 ? Remainder + Modulus : Remainder;
	}

	void BuildBalancedLaneOrder(
		const FPerfBenchmarkRequest& Request,
		const int32 WorkloadIndex,
		const int32 SampleIndex,
		TArray<EAvidScriptPerfLane, TInlineAllocator<PerfRunnerLaneCount>>& OutOrder)
	{
		static constexpr int32 BalancedRows[PerfRunnerLaneCount][PerfRunnerLaneCount] =
		{
			{ 0, 1, 5, 2, 4, 3 },
			{ 1, 2, 0, 3, 5, 4 },
			{ 2, 3, 1, 4, 0, 5 },
			{ 3, 4, 2, 5, 1, 0 },
			{ 4, 5, 3, 0, 2, 1 },
			{ 5, 0, 4, 1, 3, 2 }
		};
		const int32 Row = PositiveModulo(
			static_cast<int64>(Request.ProcessRun) +
				WorkloadIndex +
				SampleIndex,
			PerfRunnerLaneCount);
		OutOrder.Reset();
		for (int32 Position = 0; Position < PerfRunnerLaneCount; ++Position)
		{
			OutOrder.Add(Request.LaneOrder[BalancedRows[Row][Position]]);
		}
	}

	FPerfOracle RunNativeOracle(
		AAvidScriptPerfFixture& Fixture,
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations,
		const uint32 Seed)
	{
		Fixture.ScalarValue = 0;
		Fixture.ResetOperationCounts(static_cast<int32>(Workload));
		if (IsCallbackWorkload(Workload))
		{
			Fixture.ResetNativeCallbackState(static_cast<int32>(Seed));
		}
		FPerfOracle Oracle;
		Oracle.Checksum =
			RunNativeWorkload(Fixture, Workload, Iterations, Seed);
		Oracle.FinalScalar = Fixture.ScalarValue;
		Oracle.OperationCallCount =
			Fixture.GetOperationCallCount(static_cast<int32>(Workload));
		return Oracle;
	}

	bool PrepareCallbackWorkload(
		AAvidScriptPerfFixture& Fixture,
		FPuertsLane& Reflection,
		FPuertsLane& Static,
		FAvidScriptLaneSet& AvidScript,
		const EAvidScriptPerfLane Lane,
		const EAvidScriptPerfWorkload Workload,
		const uint32 Seed,
		FString& OutError)
	{
		if (!IsCallbackWorkload(Workload))
		{
			return true;
		}

		switch (Lane)
		{
		case EAvidScriptPerfLane::NativeCpp:
			Fixture.ResetNativeCallbackState(static_cast<int32>(Seed));
			return true;
		case EAvidScriptPerfLane::PuertsV8Reflection:
			Fixture.ResetPuertsCallbackState(
				Reflection.LaneId,
				static_cast<int32>(Seed));
			return true;
		case EAvidScriptPerfLane::PuertsV8Static:
			Fixture.ResetPuertsCallbackState(
				Static.LaneId,
				static_cast<int32>(Seed));
			return true;
		case EAvidScriptPerfLane::AvidScriptWasmtimeSemantic:
		case EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1:
		case EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented:
			return AvidScript.Get(Lane).PrepareCallbackWorkload(Seed, OutError);
		default:
			checkNoEntry();
			return false;
		}
	}

	void RunPuertsCallbackWorkload(
		AAvidScriptPerfFixture& Fixture,
		const int32 LaneId,
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations)
	{
		for (int32 Index = 0; Index < Iterations; ++Index)
		{
			if (Workload == EAvidScriptPerfWorkload::CallbackEmpty)
			{
				Fixture.RunPuertsEmptyCallback(LaneId, Index);
			}
			else
			{
				Fixture.RunPuertsTickCallback(
					LaneId,
					PerfRunnerTickDeltaSeconds);
			}
		}
	}

	void CollectPuertsWorkloadChecksum(
		AAvidScriptPerfFixture& Fixture,
		const int32 LaneId,
		FPerfLaneObservation& OutObservation)
	{
		OutObservation.Checksum = static_cast<uint32>(
			Fixture.GetPuertsCallbackChecksum(LaneId));
	}

	bool CollectCallbackWorkload(
		AAvidScriptPerfFixture& Fixture,
		FPuertsLane& Reflection,
		FPuertsLane& Static,
		FAvidScriptLaneSet& AvidScript,
		const EAvidScriptPerfLane Lane,
		const EAvidScriptPerfWorkload Workload,
		const bool bAvidScriptDispatchSucceeded,
		FAvidScriptWasmSmokeResult& AvidScriptDispatchResult,
		FPerfLaneObservation& OutObservation,
		FString& OutError)
	{
		if (!IsCallbackWorkload(Workload))
		{
			return true;
		}

		switch (Lane)
		{
		case EAvidScriptPerfLane::NativeCpp:
			OutObservation.Checksum = static_cast<uint32>(
				Fixture.GetNativeCallbackChecksum());
			return true;
		case EAvidScriptPerfLane::PuertsV8Reflection:
			OutObservation.Checksum = static_cast<uint32>(
				Fixture.GetPuertsCallbackChecksum(Reflection.LaneId));
			return true;
		case EAvidScriptPerfLane::PuertsV8Static:
			OutObservation.Checksum = static_cast<uint32>(
				Fixture.GetPuertsCallbackChecksum(Static.LaneId));
			return true;
		case EAvidScriptPerfLane::AvidScriptWasmtimeSemantic:
		case EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1:
		case EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented:
			return AvidScript.Get(Lane).CollectCallbackWorkload(
				bAvidScriptDispatchSucceeded,
				AvidScriptDispatchResult,
				OutObservation.Checksum,
				OutObservation.HostImportCallCount,
				OutError);
		default:
			checkNoEntry();
			return false;
		}
	}

	bool RunPerfLane(
		AAvidScriptPerfFixture& Fixture,
		FPuertsLane& Reflection,
		FPuertsLane& Static,
		FAvidScriptLaneSet& AvidScript,
		const EAvidScriptPerfLane Lane,
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations,
		const uint32 Seed,
		const bool bUseHotCallbackResults,
		const bool bTimed,
		FPerfLaneObservation& OutObservation,
		FString& OutError)
	{
		Fixture.ScalarValue = 0;
		Fixture.ResetOperationCounts(static_cast<int32>(Workload));
		OutObservation = FPerfLaneObservation{};
		OutObservation.Lane = Lane;
		if (!PrepareCallbackWorkload(
				Fixture,
				Reflection,
				Static,
				AvidScript,
				Lane,
				Workload,
				Seed,
				OutError))
		{
			return false;
		}

		uint64 StartCycles = 0;
		uint64 EndCycles = 0;
		FPlatformMemoryStats MemoryBefore;
		if (bTimed)
		{
			MemoryBefore = FPlatformMemory::GetStats();
		}
		bool bAvidScriptCallbackDispatchSucceeded = true;
		FAvidScriptWasmSmokeResult AvidScriptCallbackDispatchResult;
		switch (Lane)
		{
		case EAvidScriptPerfLane::NativeCpp:
			if (bTimed)
			{
				StartCycles = FPlatformTime::Cycles64();
				OutObservation.Checksum =
					RunNativeWorkload(Fixture, Workload, Iterations, Seed);
				EndCycles = FPlatformTime::Cycles64();
			}
			else
			{
				OutObservation.Checksum =
					RunNativeWorkload(Fixture, Workload, Iterations, Seed);
			}
			break;
		case EAvidScriptPerfLane::PuertsV8Reflection:
			if (bTimed)
			{
				StartCycles = FPlatformTime::Cycles64();
				if (IsCallbackWorkload(Workload))
				{
					RunPuertsCallbackWorkload(
						Fixture,
						Reflection.LaneId,
						Workload,
						Iterations);
				}
				else
				{
					Fixture.RunPuertsWorkload(
						Reflection.LaneId,
						static_cast<int32>(Workload),
						Iterations,
						static_cast<int32>(Seed));
				}
				EndCycles = FPlatformTime::Cycles64();
			}
			else if (IsCallbackWorkload(Workload))
			{
				RunPuertsCallbackWorkload(
					Fixture,
					Reflection.LaneId,
					Workload,
					Iterations);
			}
			else
			{
				Fixture.RunPuertsWorkload(
					Reflection.LaneId,
					static_cast<int32>(Workload),
					Iterations,
					static_cast<int32>(Seed));
			}
			break;
		case EAvidScriptPerfLane::PuertsV8Static:
			if (bTimed)
			{
				StartCycles = FPlatformTime::Cycles64();
				if (IsCallbackWorkload(Workload))
				{
					RunPuertsCallbackWorkload(
						Fixture,
						Static.LaneId,
						Workload,
						Iterations);
				}
				else
				{
					Fixture.RunPuertsWorkload(
						Static.LaneId,
						static_cast<int32>(Workload),
						Iterations,
						static_cast<int32>(Seed));
				}
				EndCycles = FPlatformTime::Cycles64();
			}
			else if (IsCallbackWorkload(Workload))
			{
				RunPuertsCallbackWorkload(
					Fixture,
					Static.LaneId,
					Workload,
					Iterations);
			}
			else
			{
				Fixture.RunPuertsWorkload(
					Static.LaneId,
					static_cast<int32>(Workload),
					Iterations,
					static_cast<int32>(Seed));
			}
			break;
		case EAvidScriptPerfLane::AvidScriptWasmtimeSemantic:
		case EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1:
		case EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented:
		{
			FAvidScriptLane& SelectedAvidScript = AvidScript.Get(Lane);
			if (IsCallbackWorkload(Workload))
			{
				if (bTimed)
				{
					StartCycles = FPlatformTime::Cycles64();
					bAvidScriptCallbackDispatchSucceeded =
						SelectedAvidScript.DispatchCallbackWorkload(
							Workload,
							Iterations,
							bUseHotCallbackResults,
							AvidScriptCallbackDispatchResult,
							OutError);
					EndCycles = FPlatformTime::Cycles64();
				}
				else
				{
					bAvidScriptCallbackDispatchSucceeded =
						SelectedAvidScript.DispatchCallbackWorkload(
							Workload,
							Iterations,
							bUseHotCallbackResults,
							AvidScriptCallbackDispatchResult,
							OutError);
				}
				break;
			}

			int32 PackedWorkload = 0;
			if (!SelectedAvidScript.PrepareWorkload(
				Workload,
				Iterations,
				Seed,
				PackedWorkload,
				OutError))
			{
				return false;
			}
			FAvidScriptWasmSmokeResult DispatchResult;
			bool bDispatchSucceeded = false;
			if (bTimed)
			{
				StartCycles = FPlatformTime::Cycles64();
				bDispatchSucceeded =
					SelectedAvidScript.DispatchWorkload(
						PackedWorkload,
						Seed,
						DispatchResult);
				EndCycles = FPlatformTime::Cycles64();
			}
			else
			{
				bDispatchSucceeded =
					SelectedAvidScript.DispatchWorkload(
						PackedWorkload,
						Seed,
						DispatchResult);
			}
			if (!SelectedAvidScript.CollectWorkloadResult(
				bDispatchSucceeded,
				DispatchResult,
				OutObservation.Checksum,
				OutObservation.HostImportCallCount,
				OutError))
			{
				return false;
			}
			break;
		}
		default:
			checkNoEntry();
			return false;
		}

		if (!IsCallbackWorkload(Workload))
		{
			if (Lane == EAvidScriptPerfLane::PuertsV8Reflection)
			{
				CollectPuertsWorkloadChecksum(
					Fixture,
					Reflection.LaneId,
					OutObservation);
			}
			else if (Lane == EAvidScriptPerfLane::PuertsV8Static)
			{
				CollectPuertsWorkloadChecksum(
					Fixture,
					Static.LaneId,
					OutObservation);
			}
		}

		if (!CollectCallbackWorkload(
				Fixture,
				Reflection,
				Static,
				AvidScript,
				Lane,
				Workload,
				bAvidScriptCallbackDispatchSucceeded,
				AvidScriptCallbackDispatchResult,
				OutObservation,
				OutError))
		{
			return false;
		}
		OutObservation.ElapsedCycles =
			bTimed ? EndCycles - StartCycles : 0;
		if (bTimed)
		{
			const FPlatformMemoryStats MemoryAfter =
				FPlatformMemory::GetStats();
			OutObservation.MemoryUsedBeforeBytes = MemoryBefore.UsedPhysical;
			OutObservation.MemoryUsedAfterBytes = MemoryAfter.UsedPhysical;
			OutObservation.PeakMemoryBytes = MemoryAfter.PeakUsedPhysical;
		}
		OutObservation.FinalScalar = Fixture.ScalarValue;
		OutObservation.OperationCallCount =
			Fixture.GetOperationCallCount(static_cast<int32>(Workload));
		if (IsAvidScriptPerfLane(Lane))
		{
			const FAvidScriptLane& SelectedAvidScript = AvidScript.Get(Lane);
			OutObservation.BackendInfo = SelectedAvidScript.BackendInfoJson;
			SelectedAvidScript.GetInvocationEvidence(
				OutObservation.DirectHitCount,
				OutObservation.RequestedDirectFallbackCount,
				OutObservation.GeneratedS1HitCount,
				OutObservation.GeneratedS1FallbackCount,
				OutObservation.GeneratedS1RejectCount,
				OutObservation.GeneratedFusedFastHitCount,
				OutObservation.GeneratedFusedRevalidateCount,
				OutObservation.GeneratedFusedCallSitePrepareCount,
				OutObservation.GeneratedDirectReadPrepareCount,
				OutObservation.GeneratedDirectWritePrepareCount,
				OutObservation.GeneratedJournalSlowPathCount,
				OutObservation.SemanticHitCount,
				OutObservation.DataLaneCommandCount,
				OutObservation.DataLaneCrossingCount,
				OutObservation.DataLaneRejectedBufferCount);
		}
		OutObservation.LogicalOperationCount =
			GetExpectedLogicalOperationCount(Workload, Iterations);
		return true;
	}

	bool ValidatePerfObservation(
		const FPerfLaneObservation& Observation,
		const FPerfOracle& Oracle,
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations,
		const double DataLaneMaxCrossingRatio,
		FString& OutError)
	{
		const uint64 ExpectedOperationCallCount =
			GetExpectedOperationCallCount(Workload, Iterations);
		const bool bDataLane =
			Observation.Lane ==
				EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented;
		const bool bDataGameplay =
			bDataLane &&
			FAvidScriptGameplayFrameBenchmark::IsGameplayWorkload(Workload);
		const int32 ExpectedHostImportCallCount =
			IsAvidScriptPerfLane(Observation.Lane)
				? (bDataLane
					? GetExpectedDataOrientedHostCallCount(Workload, Iterations)
					: GetExpectedAvidScriptHostCallCount(Workload, Iterations))
				: 0;
		const uint64 ExpectedLogicalOperationCount =
			GetExpectedLogicalOperationCount(Workload, Iterations);
		const uint64 ExpectedGeneratedS1HitCount =
			GetExpectedGeneratedS1HitCount(Workload, Iterations);
		const uint64 ExpectedSemanticHitCount =
			GetExpectedSemanticHitCount(Workload, Iterations);
		const uint64 ExpectedPropertyWriteCount =
			GetExpectedPropertyWriteCount(Workload, Iterations);
		const uint64 ExpectedDataGeneratedS1HitCount =
			ExpectedLogicalOperationCount - ExpectedPropertyWriteCount;
		const uint64 ExpectedDataCrossingCount =
			ExpectedPropertyWriteCount / 2u;
		const bool bGeneratedS1Invalid =
			Observation.Lane ==
				EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1 &&
			(Observation.GeneratedS1HitCount != ExpectedGeneratedS1HitCount ||
			 Observation.GeneratedS1FallbackCount != 0 ||
			 Observation.GeneratedS1RejectCount != 0);
		const bool bSemanticInvalid =
			Observation.Lane ==
				EAvidScriptPerfLane::AvidScriptWasmtimeSemantic &&
			Observation.SemanticHitCount != ExpectedSemanticHitCount;
		const bool bGeneratedLaneDataInvalid =
			Observation.Lane ==
				EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1 &&
			(Observation.DataLaneCommandCount != 0 ||
			 Observation.DataLaneCrossingCount != 0 ||
			 Observation.DataLaneRejectedBufferCount != 0);
		const bool bDataGameplayInvalid =
			bDataGameplay &&
			(Observation.GeneratedS1HitCount != ExpectedDataGeneratedS1HitCount ||
			 Observation.GeneratedS1FallbackCount != 0 ||
			 Observation.GeneratedS1RejectCount != 0 ||
			 Observation.DataLaneCommandCount != ExpectedPropertyWriteCount ||
			 Observation.DataLaneCrossingCount != ExpectedDataCrossingCount ||
			 Observation.DataLaneRejectedBufferCount != 0 ||
			 Observation.DataLaneCommandCount == 0 ||
			 static_cast<double>(Observation.DataLaneCrossingCount) /
				 static_cast<double>(Observation.DataLaneCommandCount) >
				 DataLaneMaxCrossingRatio);
		const bool bDataMicroInvalid =
			bDataLane &&
			!bDataGameplay &&
			(Observation.GeneratedS1HitCount != ExpectedGeneratedS1HitCount ||
			 Observation.GeneratedS1FallbackCount != 0 ||
			 Observation.GeneratedS1RejectCount != 0 ||
			 Observation.DataLaneCommandCount != 0 ||
			 Observation.DataLaneCrossingCount != 0 ||
			 Observation.DataLaneRejectedBufferCount != 0);
		const bool bFusedLane =
			Observation.Lane ==
				EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1
			|| bDataLane;
		const uint64 ExpectedFusedGeneratedHits = bDataGameplay
			? ExpectedDataGeneratedS1HitCount
			: ExpectedGeneratedS1HitCount;
		const bool bHasFusedCalls = ExpectedFusedGeneratedHits > 0;
		const uint64 DirectPrepareCount =
			Observation.GeneratedDirectReadPrepareCount
			+ Observation.GeneratedDirectWritePrepareCount;
		const bool bFusedPathInvalid = bFusedLane
			? (Observation.GeneratedJournalSlowPathCount != 0
				|| (bHasFusedCalls
					&& (Observation.GeneratedFusedRevalidateCount != 1
						|| Observation.GeneratedFusedFastHitCount + 1
							!= ExpectedFusedGeneratedHits
						|| Observation.GeneratedFusedCallSitePrepareCount == 0
						|| DirectPrepareCount
							!= Observation.GeneratedFusedCallSitePrepareCount))
				|| (!bHasFusedCalls
					&& (Observation.GeneratedFusedFastHitCount != 0
						|| Observation.GeneratedFusedRevalidateCount != 0
						|| Observation.GeneratedFusedCallSitePrepareCount != 0
						|| DirectPrepareCount != 0)))
			: (Observation.GeneratedFusedFastHitCount != 0
				|| Observation.GeneratedFusedRevalidateCount != 0
				|| Observation.GeneratedFusedCallSitePrepareCount != 0
				|| DirectPrepareCount != 0
				|| Observation.GeneratedJournalSlowPathCount != 0);
		if (Oracle.OperationCallCount != ExpectedOperationCallCount ||
			Observation.Checksum != Oracle.Checksum ||
			Observation.FinalScalar != Oracle.FinalScalar ||
			Observation.OperationCallCount != ExpectedOperationCallCount ||
			Observation.HostImportCallCount != ExpectedHostImportCallCount ||
			Observation.LogicalOperationCount != ExpectedLogicalOperationCount ||
			bGeneratedS1Invalid ||
			bSemanticInvalid ||
			bGeneratedLaneDataInvalid ||
			bDataGameplayInvalid ||
			bDataMicroInvalid ||
			bFusedPathInvalid)
		{
			OutError = FString::Printf(
				TEXT("correctness failure lane=%s workload=%s iterations=%d ")
				TEXT("checksum=%u expected_checksum=%u final_scalar=%d ")
				TEXT("expected_final_scalar=%d operation_count=%llu ")
				TEXT("expected_operation_count=%llu host_count=%d ")
				TEXT("expected_host_count=%d generated_hit=%llu ")
				TEXT("expected_generated_hit=%llu generated_fallback=%llu ")
				TEXT("generated_reject=%llu semantic_hit=%llu ")
				TEXT("expected_semantic_hit=%llu logical=%llu ")
				TEXT("data_generated_expected=%llu data_commands=%llu ")
				TEXT("data_commands_expected=%llu data_crossings=%llu ")
				TEXT("data_crossings_expected=%llu data_rejected=%llu ")
				TEXT("fused_fast=%llu fused_revalidate=%llu ")
				TEXT("fused_call_site_prepare=%llu direct_prepare=%llu ")
				TEXT("journal_slow=%llu"),
				GetPerfLaneName(Observation.Lane),
				GetPerfWorkloadName(Workload),
				Iterations,
				Observation.Checksum,
				Oracle.Checksum,
				Observation.FinalScalar,
				Oracle.FinalScalar,
				Observation.OperationCallCount,
				ExpectedOperationCallCount,
				Observation.HostImportCallCount,
				ExpectedHostImportCallCount,
				Observation.GeneratedS1HitCount,
				ExpectedGeneratedS1HitCount,
				Observation.GeneratedS1FallbackCount,
				Observation.GeneratedS1RejectCount,
				Observation.SemanticHitCount,
				ExpectedSemanticHitCount,
				ExpectedLogicalOperationCount,
				ExpectedDataGeneratedS1HitCount,
				Observation.DataLaneCommandCount,
				ExpectedPropertyWriteCount,
				Observation.DataLaneCrossingCount,
				ExpectedDataCrossingCount,
				Observation.DataLaneRejectedBufferCount,
				Observation.GeneratedFusedFastHitCount,
				Observation.GeneratedFusedRevalidateCount,
				Observation.GeneratedFusedCallSitePrepareCount,
				DirectPrepareCount,
				Observation.GeneratedJournalSlowPathCount);
			return false;
		}
		return true;
	}

	bool RunPerfRound(
		AAvidScriptPerfFixture& Fixture,
		FPuertsLane& Reflection,
		FPuertsLane& Static,
		FAvidScriptLaneSet& AvidScript,
		const FPerfBenchmarkRequest& Request,
		const int32 WorkloadIndex,
		const int32 SampleIndex,
		const TArray<int32>& IterationCounts,
		const uint32 Seed,
		const bool bTimed,
		TArray<FPerfLaneObservation, TInlineAllocator<PerfRunnerLaneCount>>& OutObservations,
		FString& OutError)
	{
		TArray<EAvidScriptPerfLane, TInlineAllocator<PerfRunnerLaneCount>> LaneOrder;
		BuildBalancedLaneOrder(
			Request,
			WorkloadIndex,
			SampleIndex,
			LaneOrder);
		OutObservations.Reset();
		for (int32 LanePosition = 0;
			LanePosition < LaneOrder.Num();
			++LanePosition)
		{
			const EAvidScriptPerfLane Lane = LaneOrder[LanePosition];
			const int32 Iterations = IterationCounts[
				GetPerfIterationMatrixIndex(WorkloadIndex, Lane)];
			const FPerfOracle Oracle = RunNativeOracle(
				Fixture,
				Request.Workloads[WorkloadIndex],
				Iterations,
				Seed);
			FPerfLaneObservation Observation;
			if (!RunPerfLane(
					Fixture,
					Reflection,
					Static,
					AvidScript,
					Lane,
					Request.Workloads[WorkloadIndex],
					Iterations,
					Seed,
					Request.bUseHotCallbackResults,
					bTimed,
					Observation,
					OutError) ||
				!ValidatePerfObservation(
					Observation,
					Oracle,
					Request.Workloads[WorkloadIndex],
					Iterations,
					Request.DataLaneMaxCrossingRatio,
					OutError))
			{
				return false;
			}
			Observation.LanePosition = LanePosition;
			Observation.Iterations = Iterations;
			Observation.ExpectedChecksum = Oracle.Checksum;
			Observation.ExpectedFinalScalar = Oracle.FinalScalar;
			Observation.ExpectedOperationCallCount =
				GetExpectedOperationCallCount(
					Request.Workloads[WorkloadIndex],
					Iterations);
			Observation.ExpectedHostImportCallCount =
				IsAvidScriptPerfLane(Lane)
					? (Lane == EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented
						? GetExpectedDataOrientedHostCallCount(
							Request.Workloads[WorkloadIndex],
							Iterations)
						: GetExpectedAvidScriptHostCallCount(
							Request.Workloads[WorkloadIndex],
							Iterations))
					: 0;
			OutObservations.Add(Observation);
		}
		return true;
	}

	bool CalibrateLaneIterations(
		AAvidScriptPerfFixture& Fixture,
		FPuertsLane& Reflection,
		FPuertsLane& Static,
		FAvidScriptLaneSet& AvidScript,
		const FPerfBenchmarkRequest& Request,
		const int32 WorkloadIndex,
		const EAvidScriptPerfLane Lane,
		int32& OutIterations,
		FString& OutError)
	{
		int32 Iterations = Request.MinimumIterations;
		int32 CalibrationRound = 0;
		auto RunCalibrationSample =
			[&](double& OutElapsedMilliseconds) -> bool
		{
			const uint32 Seed = MakePerfRunnerSampleSeed(
				Request.Seed,
				WorkloadIndex,
				CalibrationRound++);
			const FPerfOracle Oracle = RunNativeOracle(
				Fixture,
				Request.Workloads[WorkloadIndex],
				Iterations,
				Seed);
			FPerfLaneObservation Observation;
			if (!RunPerfLane(
					Fixture,
					Reflection,
					Static,
					AvidScript,
					Lane,
					Request.Workloads[WorkloadIndex],
					Iterations,
					Seed,
					Request.bUseHotCallbackResults,
					true,
					Observation,
					OutError) ||
				!ValidatePerfObservation(
					Observation,
					Oracle,
					Request.Workloads[WorkloadIndex],
					Iterations,
					Request.DataLaneMaxCrossingRatio,
					OutError))
			{
				return false;
			}

			OutElapsedMilliseconds =
				static_cast<double>(Observation.ElapsedCycles) *
				FPlatformTime::GetSecondsPerCycle64() *
				1000.0;
			return true;
		};
		for (;;)
		{
			double CandidateElapsedMilliseconds = 0.0;
			if (!RunCalibrationSample(CandidateElapsedMilliseconds))
			{
				return false;
			}

			if (CandidateElapsedMilliseconds >= Request.MinimumSampleMilliseconds)
			{
				TArray<double> SteadyStateSamples;
				SteadyStateSamples.Reserve(
					Request.CalibrationConfirmationSamples);
				for (int32 SampleIndex = 0;
					SampleIndex < Request.CalibrationConfirmationSamples;
					++SampleIndex)
				{
					double ElapsedMilliseconds = 0.0;
					if (!RunCalibrationSample(ElapsedMilliseconds))
					{
						return false;
					}
					SteadyStateSamples.Add(ElapsedMilliseconds);
				}

				const double SteadyStateMedianMilliseconds =
					GetSteadyStateMedianMilliseconds(MoveTemp(SteadyStateSamples));
				if (SteadyStateMedianMilliseconds >= Request.MinimumSampleMilliseconds)
				{
					OutIterations = Iterations;
					return true;
				}
			}
			if (Iterations >= Request.MaximumIterations)
			{
				OutError = FString::Printf(
					TEXT("calibration could not confirm %.3f ms steady state by ")
						TEXT("maximum_iterations=%d workload=%s lane=%s"),
					Request.MinimumSampleMilliseconds,
					Request.MaximumIterations,
					GetPerfWorkloadName(Request.Workloads[WorkloadIndex]),
					GetPerfLaneName(Lane));
				return false;
			}

			const int32 NextIterations =
				Iterations > Request.MaximumIterations / 2
					? Request.MaximumIterations
					: Iterations * 2;
			if (NextIterations <= Iterations)
			{
				OutError = TEXT("calibration iteration doubling overflowed");
				return false;
			}
			Iterations = NextIterations;
		}
	}

	void SetExactIntegerField(
		const TSharedRef<FJsonObject>& Object,
		const TCHAR* FieldName,
		const int64 Value)
	{
		Object->SetField(
			FieldName,
			MakeShared<FJsonValueNumberString>(LexToString(Value)));
	}

	void SetExactUnsignedField(
		const TSharedRef<FJsonObject>& Object,
		const TCHAR* FieldName,
		const uint64 Value)
	{
		Object->SetField(
			FieldName,
			MakeShared<FJsonValueNumberString>(LexToString(Value)));
	}

	TSharedRef<FJsonObject> MakePerfSampleJson(const FPerfSample& Sample)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		SetExactIntegerField(Object, TEXT("process_run"), Sample.ProcessRun);
		Object->SetStringField(TEXT("lane"), GetPerfLaneName(Sample.Lane));
		Object->SetStringField(
			TEXT("lane_identity_sha256"),
			Sample.LaneIdentitySha256);
		SetExactIntegerField(Object, TEXT("lane_position"), Sample.LanePosition);
		Object->SetStringField(
			TEXT("workload"),
			GetPerfWorkloadName(Sample.Workload));
		SetExactIntegerField(Object, TEXT("sample_index"), Sample.SampleIndex);
		SetExactUnsignedField(Object, TEXT("seed"), Sample.Seed);
		SetExactIntegerField(Object, TEXT("iterations"), Sample.Iterations);
		SetExactUnsignedField(
			Object,
			TEXT("elapsed_cycles"),
			Sample.ElapsedCycles);
		SetExactUnsignedField(Object, TEXT("checksum"), Sample.Checksum);
		SetExactUnsignedField(
			Object,
			TEXT("expected_checksum"),
			Sample.ExpectedChecksum);
		SetExactIntegerField(Object, TEXT("final_scalar"), Sample.FinalScalar);
		SetExactIntegerField(
			Object,
			TEXT("expected_final_scalar"),
			Sample.ExpectedFinalScalar);
		SetExactUnsignedField(
			Object,
			TEXT("operation_call_count"),
			Sample.OperationCallCount);
		SetExactUnsignedField(
			Object,
			TEXT("expected_operation_call_count"),
			Sample.ExpectedOperationCallCount);
		SetExactIntegerField(
			Object,
			TEXT("host_import_call_count"),
			Sample.HostImportCallCount);
		SetExactIntegerField(
			Object,
			TEXT("expected_host_import_call_count"),
			Sample.ExpectedHostImportCallCount);
		SetExactUnsignedField(
			Object,
			TEXT("direct_hit_count"),
			Sample.DirectHitCount);
		SetExactUnsignedField(
			Object,
			TEXT("requested_direct_fallback_count"),
			Sample.RequestedDirectFallbackCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_s1_hit_count"),
			Sample.GeneratedS1HitCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_s1_fallback_count"),
			Sample.GeneratedS1FallbackCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_s1_reject_count"),
			Sample.GeneratedS1RejectCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_fused_fast_hit_count"),
			Sample.GeneratedFusedFastHitCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_fused_revalidate_count"),
			Sample.GeneratedFusedRevalidateCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_fused_call_site_prepare_count"),
			Sample.GeneratedFusedCallSitePrepareCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_direct_read_prepare_count"),
			Sample.GeneratedDirectReadPrepareCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_direct_write_prepare_count"),
			Sample.GeneratedDirectWritePrepareCount);
		SetExactUnsignedField(
			Object,
			TEXT("generated_journal_slow_path_count"),
			Sample.GeneratedJournalSlowPathCount);
		SetExactUnsignedField(
			Object,
			TEXT("data_lane_command_count"),
			Sample.DataLaneCommandCount);
		SetExactUnsignedField(
			Object,
			TEXT("data_lane_crossing_count"),
			Sample.DataLaneCrossingCount);
		SetExactUnsignedField(
			Object,
			TEXT("data_lane_rejected_buffer_count"),
			Sample.DataLaneRejectedBufferCount);
		SetExactUnsignedField(
			Object,
			TEXT("semantic_hit_count"),
			Sample.SemanticHitCount);
		SetExactUnsignedField(
			Object,
			TEXT("logical_operation_count"),
			Sample.LogicalOperationCount);
		SetExactUnsignedField(
			Object,
			TEXT("peak_memory_bytes"),
			Sample.PeakMemoryBytes);
		const TSharedRef<FJsonObject> Allocations = MakeShared<FJsonObject>();
		Allocations->SetStringField(
			TEXT("status"),
			TEXT("allocator_count_not_exposed"));
		Allocations->SetField(
			TEXT("count"),
			MakeShared<FJsonValueNull>());
		SetExactIntegerField(
			Allocations,
			TEXT("net_physical_bytes"),
			static_cast<int64>(Sample.MemoryUsedAfterBytes) -
				static_cast<int64>(Sample.MemoryUsedBeforeBytes));
		Object->SetObjectField(TEXT("allocations"), Allocations);
		const TSharedRef<FJsonObject> GeneratedCodeSize =
			MakeShared<FJsonObject>();
		GeneratedCodeSize->SetStringField(
			TEXT("status"),
			TEXT("runtime_not_exposed"));
		GeneratedCodeSize->SetField(
			TEXT("bytes"),
			MakeShared<FJsonValueNull>());
		Object->SetObjectField(
			TEXT("generated_code_size"),
			GeneratedCodeSize);
		Object->SetBoolField(TEXT("correct"), true);
		if (Sample.BackendInfo.IsValid())
		{
			Object->SetObjectField(TEXT("backend_info"), Sample.BackendInfo.ToSharedRef());
		}
		return Object;
	}

	bool PublishPerfResult(
		const FString& ResultPath,
		const FPerfBenchmarkRequest& Request,
		const TArray<int32>& IterationCounts,
		const TArray<FPerfSample>& Samples,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		SetExactIntegerField(
			Root,
			TEXT("schema_version"),
			PerfRunnerResultSchemaVersion);
		SetExactUnsignedField(
			Root,
			TEXT("timer_frequency_hz"),
			static_cast<uint64>(
				1.0 / FPlatformTime::GetSecondsPerCycle64()));
		Root->SetArrayField(TEXT("lane_catalog"), Request.LaneCatalogJson);
		Root->SetStringField(
			TEXT("lane_catalog_sha256"),
			Request.LaneCatalogSha256);
		Root->SetStringField(TEXT("request_sha256"), Request.RequestSha256);
		Root->SetObjectField(TEXT("provenance"), Request.Provenance.ToSharedRef());

		if (Request.Mode == EAvidScriptPerfBenchmarkMode::Calibrate)
		{
			Root->SetStringField(
				TEXT("calibration_id"),
				Request.AttemptId);
			const TSharedRef<FJsonObject> IterationCountsJson =
				MakeShared<FJsonObject>();
			for (int32 WorkloadIndex = 0;
				WorkloadIndex < Request.Workloads.Num();
				++WorkloadIndex)
			{
				const TSharedRef<FJsonObject> LaneIterationCountsJson =
					MakeShared<FJsonObject>();
				for (int32 LaneIndex = 0;
					LaneIndex < PerfRunnerLaneCount;
					++LaneIndex)
				{
					const EAvidScriptPerfLane Lane =
						static_cast<EAvidScriptPerfLane>(LaneIndex);
					SetExactIntegerField(
						LaneIterationCountsJson,
						GetPerfLaneName(Lane),
						IterationCounts[
							GetPerfIterationMatrixIndex(
								WorkloadIndex,
								Lane)]);
				}
				IterationCountsJson->SetObjectField(
					GetPerfWorkloadName(Request.Workloads[WorkloadIndex]),
					LaneIterationCountsJson);
			}
			Root->SetObjectField(
				TEXT("iteration_counts"),
				IterationCountsJson);
		}
		else
		{
			Root->SetStringField(TEXT("run_id"), Request.AttemptId);
			SetExactIntegerField(
				Root,
				TEXT("process_run"),
				Request.ProcessRun);

			TArray<TSharedPtr<FJsonValue>> LaneOrderJson;
			for (const EAvidScriptPerfLane Lane : Request.LaneOrder)
			{
				LaneOrderJson.Add(
					MakeShared<FJsonValueString>(GetPerfLaneName(Lane)));
			}
			Root->SetArrayField(
				TEXT("lane_order"),
				MoveTemp(LaneOrderJson));

			TArray<TSharedPtr<FJsonValue>> SamplesJson;
			SamplesJson.Reserve(Samples.Num());
			for (const FPerfSample& Sample : Samples)
			{
				SamplesJson.Add(
					MakeShared<FJsonValueObject>(
						MakePerfSampleJson(Sample)));
			}
			Root->SetArrayField(TEXT("samples"), MoveTemp(SamplesJson));
		}

		FString ResultJson;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&ResultJson);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			OutError = TEXT("unable to serialize warm benchmark result");
			return false;
		}
		ResultJson += TEXT("\n");
		if (!FFileHelper::SaveStringToFile(
			ResultJson,
			*Request.TemporaryResultPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_NoReplaceExisting))
		{
			OutError = FString::Printf(
				TEXT("refused to overwrite or could not write temporary result: %s"),
				*Request.TemporaryResultPath);
			return false;
		}
		if (!IFileManager::Get().Move(
			*ResultPath,
			*Request.TemporaryResultPath,
			false,
			false,
			false,
			true))
		{
			IFileManager::Get().Delete(
				*Request.TemporaryResultPath,
				false,
				true,
				true);
			OutError = FString::Printf(
				TEXT("refused to overwrite or could not atomically publish result: %s"),
				*ResultPath);
			return false;
		}
		return true;
	}
}

bool FAvidScriptPerfRunner::RunWarmBenchmarkFromFiles(
	const FString& RequestPath,
	const FString& ResultPath,
	FString& OutError)
{
	OutError.Reset();
	if (FPaths::IsRelative(RequestPath) || FPaths::IsRelative(ResultPath))
	{
		OutError = TEXT("benchmark request and result paths must be absolute");
		return false;
	}
	if (!FPaths::FileExists(RequestPath))
	{
		OutError = FString::Printf(
			TEXT("benchmark request does not exist: %s"),
			*RequestPath);
		return false;
	}
	if (FPaths::FileExists(ResultPath))
	{
		OutError = FString::Printf(
			TEXT("refusing to overwrite existing benchmark result: %s"),
			*ResultPath);
		return false;
	}
	const FString ResultDirectory = FPaths::GetPath(ResultPath);
	if (!FPaths::DirectoryExists(ResultDirectory))
	{
		OutError = FString::Printf(
			TEXT("benchmark result directory does not exist: %s"),
			*ResultDirectory);
		return false;
	}

	FPerfBenchmarkRequest Request;
	if (!ParsePerfBenchmarkRequest(
		RequestPath,
		ResultPath,
		Request,
		OutError))
	{
		return false;
	}
	FString ActualEditorExecutableSha256;
	if (!GetPerfFileSha256(
			FPlatformProcess::ExecutablePath(),
			ActualEditorExecutableSha256,
			OutError) ||
		!ActualEditorExecutableSha256.Equals(
			Request.EditorExecutableSha256,
			ESearchCase::CaseSensitive))
	{
		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Editor executable identity mismatch: expected=%s actual=%s"),
				*Request.EditorExecutableSha256,
				*ActualEditorExecutableSha256);
		}
		return false;
	}
	if (FPaths::FileExists(Request.TemporaryResultPath))
	{
		OutError = FString::Printf(
			TEXT("refusing to overwrite existing temporary result: %s"),
			*Request.TemporaryResultPath);
		return false;
	}

	UWorld* World = nullptr;
	if (!CreateBenchmarkWorld(World))
	{
		OutError = TEXT("unable to create the shared benchmark world");
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyBenchmarkWorld(World);
	};

	AAvidScriptPerfFixture* Fixture =
		World->SpawnActor<AAvidScriptPerfFixture>();
	if (Fixture == nullptr)
	{
		OutError = TEXT("unable to spawn the shared benchmark fixture actor");
		return false;
	}

	FPuertsLane Reflection;
	const FPerfLaneCatalogEntry& ReflectionCatalog =
		Request.LaneCatalog[static_cast<int32>(
			EAvidScriptPerfLane::PuertsV8Reflection)];
	if (!Reflection.Initialize(
		TEXT("reflection.js"),
		AAvidScriptPerfFixture::ReflectionLaneId,
		ReflectionCatalog.ArtifactSha256,
		ReflectionCatalog.RuntimeArtifactSha256,
		*Fixture,
		OutError))
	{
		return false;
	}
	FPuertsLane Static;
	const FPerfLaneCatalogEntry& StaticCatalog =
		Request.LaneCatalog[static_cast<int32>(
			EAvidScriptPerfLane::PuertsV8Static)];
	if (!Static.Initialize(
		TEXT("static.js"),
		AAvidScriptPerfFixture::StaticLaneId,
		StaticCatalog.ArtifactSha256,
		StaticCatalog.RuntimeArtifactSha256,
		*Fixture,
		OutError))
	{
		return false;
	}
	FAvidScriptLaneSet AvidScript;
	FAvidScriptVmBackendSelection WasmtimeSelection;
	WasmtimeSelection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	WasmtimeSelection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	WasmtimeSelection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	WasmtimeSelection.bAllowFallback = false;
	const FPerfLaneCatalogEntry& SemanticCatalog =
		Request.LaneCatalog[static_cast<int32>(
			EAvidScriptPerfLane::AvidScriptWasmtimeSemantic)];
	if (!AvidScript.WasmtimeSemantic.Initialize(
			WasmtimeSelection,
			EAvidScriptBindingInvocationPolicy::SemanticProcessEvent,
			SemanticCatalog.BindingInvocationMode,
			SemanticCatalog.BackendId,
			SemanticCatalog.RuntimeVersion,
			SemanticCatalog.ExecutionMode,
			SemanticCatalog.ArtifactFormat,
			SemanticCatalog.ArtifactSha256,
			SemanticCatalog.SourceWasmSha256,
			SemanticCatalog.TargetTriple,
			SemanticCatalog.RuntimeBuildIdentity,
			SemanticCatalog.RuntimeArtifactSha256,
			SemanticCatalog.ManifestRelativePath,
			*Fixture,
			OutError))
	{
		return false;
	}
	const FPerfLaneCatalogEntry& GeneratedCatalog =
		Request.LaneCatalog[static_cast<int32>(
			EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1)];
	if (!AvidScript.WasmtimeGeneratedS1.Initialize(
			WasmtimeSelection,
			EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect,
			GeneratedCatalog.BindingInvocationMode,
			GeneratedCatalog.BackendId,
			GeneratedCatalog.RuntimeVersion,
			GeneratedCatalog.ExecutionMode,
			GeneratedCatalog.ArtifactFormat,
			GeneratedCatalog.ArtifactSha256,
			GeneratedCatalog.SourceWasmSha256,
			GeneratedCatalog.TargetTriple,
			GeneratedCatalog.RuntimeBuildIdentity,
			GeneratedCatalog.RuntimeArtifactSha256,
			GeneratedCatalog.ManifestRelativePath,
			*Fixture,
			OutError))
	{
		return false;
	}
	const FPerfLaneCatalogEntry& DataCatalog =
		Request.LaneCatalog[static_cast<int32>(
			EAvidScriptPerfLane::AvidScriptWasmtimeDataOriented)];
	if (!AvidScript.WasmtimeDataOriented.Initialize(
			WasmtimeSelection,
			EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect,
			DataCatalog.BindingInvocationMode,
			DataCatalog.BackendId,
			DataCatalog.RuntimeVersion,
			DataCatalog.ExecutionMode,
			DataCatalog.ArtifactFormat,
			DataCatalog.ArtifactSha256,
			DataCatalog.SourceWasmSha256,
			DataCatalog.TargetTriple,
			DataCatalog.RuntimeBuildIdentity,
			DataCatalog.RuntimeArtifactSha256,
			DataCatalog.ManifestRelativePath,
			*Fixture,
			OutError))
	{
		return false;
	}
	if (AvidScript.WasmtimeGeneratedS1.Manifest.BindingPackageName.IsEmpty() ||
		AvidScript.WasmtimeGeneratedS1.Manifest.BindingPackageHash.IsEmpty() ||
		!AvidScript.WasmtimeGeneratedS1.Manifest.BindingPackageName.Equals(
			AvidScript.WasmtimeDataOriented.Manifest.BindingPackageName,
			ESearchCase::CaseSensitive) ||
		!AvidScript.WasmtimeGeneratedS1.Manifest.BindingPackageHash.Equals(
			AvidScript.WasmtimeDataOriented.Manifest.BindingPackageHash,
			ESearchCase::CaseSensitive))
	{
		OutError = TEXT(
			"generated S1 and data-oriented manifests must share one generated binding package name and hash");
		return false;
	}

	TArray<int32> IterationCounts;
	IterationCounts.Reserve(
		Request.Workloads.Num() * PerfRunnerLaneCount);
	if (Request.Mode != EAvidScriptPerfBenchmarkMode::Timed)
	{
		for (int32 WorkloadIndex = 0;
			WorkloadIndex < Request.Workloads.Num();
			++WorkloadIndex)
		{
			for (int32 LaneIndex = 0;
				LaneIndex < PerfRunnerLaneCount;
				++LaneIndex)
			{
				const EAvidScriptPerfLane Lane =
					static_cast<EAvidScriptPerfLane>(LaneIndex);
				int32 Iterations = 0;
				if (!CalibrateLaneIterations(
						*Fixture,
						Reflection,
						Static,
						AvidScript,
						Request,
						WorkloadIndex,
						Lane,
						Iterations,
						OutError))
				{
					return false;
				}
				IterationCounts.Add(Iterations);
			}
		}
	}
	else
	{
		IterationCounts = Request.FrozenIterationCounts;
	}

	TArray<FPerfSample> Samples;
	if (Request.Mode != EAvidScriptPerfBenchmarkMode::Calibrate)
	{
		Samples.Reserve(
			Request.Workloads.Num() *
			Request.TimedSamples *
			PerfRunnerLaneCount);
		for (int32 WorkloadIndex = 0;
			WorkloadIndex < Request.Workloads.Num();
			++WorkloadIndex)
		{
			const EAvidScriptPerfWorkload Workload =
				Request.Workloads[WorkloadIndex];

			for (int32 WarmupIndex = 0;
				WarmupIndex < Request.WarmupSamples;
				++WarmupIndex)
			{
				const uint32 Seed = MakePerfRunnerSampleSeed(
					Request.Seed,
					WorkloadIndex,
					WarmupIndex);
				TArray<
					FPerfLaneObservation,
					TInlineAllocator<PerfRunnerLaneCount>> Observations;
				if (!RunPerfRound(
					*Fixture,
					Reflection,
					Static,
					AvidScript,
					Request,
					WorkloadIndex,
					WarmupIndex,
					IterationCounts,
					Seed,
					false,
					Observations,
					OutError))
				{
					return false;
				}
			}

			for (int32 SampleIndex = 0;
				SampleIndex < Request.TimedSamples;
				++SampleIndex)
			{
				const uint32 Seed = MakePerfRunnerSampleSeed(
					Request.Seed,
					WorkloadIndex,
					SampleIndex);
				TArray<
					FPerfLaneObservation,
					TInlineAllocator<PerfRunnerLaneCount>> Observations;
				if (!RunPerfRound(
					*Fixture,
					Reflection,
					Static,
					AvidScript,
					Request,
					WorkloadIndex,
					SampleIndex,
					IterationCounts,
					Seed,
					true,
					Observations,
					OutError))
				{
					return false;
				}

				for (const FPerfLaneObservation& Observation : Observations)
				{
					FPerfSample& Sample = Samples.AddDefaulted_GetRef();
					Sample.ProcessRun = Request.ProcessRun;
					Sample.Lane = Observation.Lane;
					Sample.LaneIdentitySha256 =
						Request.LaneCatalog[static_cast<int32>(Observation.Lane)]
							.LaneIdentitySha256;
					Sample.LanePosition = Observation.LanePosition;
					Sample.Workload = Workload;
					Sample.SampleIndex = SampleIndex;
					Sample.Seed = Seed;
					Sample.Iterations = Observation.Iterations;
					Sample.ElapsedCycles = Observation.ElapsedCycles;
					Sample.Checksum = Observation.Checksum;
					Sample.ExpectedChecksum =
						Observation.ExpectedChecksum;
					Sample.FinalScalar = Observation.FinalScalar;
					Sample.ExpectedFinalScalar =
						Observation.ExpectedFinalScalar;
					Sample.OperationCallCount =
						Observation.OperationCallCount;
					Sample.ExpectedOperationCallCount =
						Observation.ExpectedOperationCallCount;
					Sample.HostImportCallCount =
						Observation.HostImportCallCount;
					Sample.ExpectedHostImportCallCount =
						Observation.ExpectedHostImportCallCount;
					Sample.DirectHitCount = Observation.DirectHitCount;
					Sample.RequestedDirectFallbackCount =
						Observation.RequestedDirectFallbackCount;
					Sample.GeneratedS1HitCount =
						Observation.GeneratedS1HitCount;
					Sample.GeneratedS1FallbackCount =
						Observation.GeneratedS1FallbackCount;
					Sample.GeneratedS1RejectCount =
						Observation.GeneratedS1RejectCount;
					Sample.GeneratedFusedFastHitCount =
						Observation.GeneratedFusedFastHitCount;
					Sample.GeneratedFusedRevalidateCount =
						Observation.GeneratedFusedRevalidateCount;
					Sample.GeneratedFusedCallSitePrepareCount =
						Observation.GeneratedFusedCallSitePrepareCount;
					Sample.GeneratedDirectReadPrepareCount =
						Observation.GeneratedDirectReadPrepareCount;
					Sample.GeneratedDirectWritePrepareCount =
						Observation.GeneratedDirectWritePrepareCount;
					Sample.GeneratedJournalSlowPathCount =
						Observation.GeneratedJournalSlowPathCount;
					Sample.DataLaneCommandCount =
						Observation.DataLaneCommandCount;
					Sample.DataLaneCrossingCount =
						Observation.DataLaneCrossingCount;
					Sample.DataLaneRejectedBufferCount =
						Observation.DataLaneRejectedBufferCount;
					Sample.SemanticHitCount =
						Observation.SemanticHitCount;
					Sample.LogicalOperationCount =
						Observation.LogicalOperationCount;
					Sample.MemoryUsedBeforeBytes =
						Observation.MemoryUsedBeforeBytes;
					Sample.MemoryUsedAfterBytes =
						Observation.MemoryUsedAfterBytes;
					Sample.PeakMemoryBytes =
						Observation.PeakMemoryBytes;
					Sample.BackendInfo = Observation.BackendInfo;
				}
			}
		}
	}

	return PublishPerfResult(
		ResultPath,
		Request,
		IterationCounts,
		Samples,
		OutError);
}

bool FAvidScriptPerfRunner::RunFiveLaneCorrectnessSmoke(
	const int32 IterationsPerWorkload,
	const int32 Seed,
	FAvidScriptPerfSmokeResult& OutResult)
{
	OutResult = FAvidScriptPerfSmokeResult{};
	OutResult.IterationsPerWorkload = IterationsPerWorkload;
	if (IterationsPerWorkload <= 0)
	{
		OutResult.Error = TEXT("IterationsPerWorkload must be positive");
		return false;
	}

	UWorld* World = nullptr;
	if (!CreateBenchmarkWorld(World))
	{
		OutResult.Error = TEXT("Unable to create the shared benchmark world");
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyBenchmarkWorld(World);
	};

	AAvidScriptPerfFixture* Fixture = World->SpawnActor<AAvidScriptPerfFixture>();
	if (Fixture == nullptr)
	{
		OutResult.Error = TEXT("Unable to spawn the shared benchmark fixture actor");
		return false;
	}

	FString Error;
	FPuertsLane Reflection;
	if (!Reflection.InitializeForCorrectnessSmoke(
		TEXT("reflection.js"),
		AAvidScriptPerfFixture::ReflectionLaneId,
		*Fixture,
		Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}
	FPuertsLane Static;
	if (!Static.InitializeForCorrectnessSmoke(
		TEXT("static.js"),
		AAvidScriptPerfFixture::StaticLaneId,
		*Fixture,
		Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}
	FAvidScriptLaneSet AvidScript;
	FAvidScriptVmBackendSelection WasmtimeSelection;
	WasmtimeSelection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	WasmtimeSelection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	WasmtimeSelection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	WasmtimeSelection.bAllowFallback = false;
	if (!AvidScript.WasmtimeSemantic.Initialize(
			WasmtimeSelection,
			EAvidScriptBindingInvocationPolicy::SemanticProcessEvent,
			TEXT("semantic_process_event"),
			TEXT("wasmtime.cranelift.jit"),
			TEXT("45.0.0"),
			TEXT("jit"),
			TEXT("wasm_bytecode"),
			FString(),
			FString(),
			TEXT("x86_64-pc-windows-msvc"),
			FString(),
			FString(),
			TEXT("AvidScriptCSharpGuest/Profiles/profile_phase54_6_semantic/")
				TEXT("profile_phase54_6_semantic.avidscript.json"),
			*Fixture,
			Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}
	if (!AvidScript.WasmtimeGeneratedS1.Initialize(
			WasmtimeSelection,
			EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect,
			TEXT("generated_native_s1"),
			TEXT("wasmtime.cranelift.jit"),
			TEXT("45.0.0"),
			TEXT("jit"),
			TEXT("wasm_bytecode"),
			FString(),
			FString(),
			TEXT("x86_64-pc-windows-msvc"),
			FString(),
			FString(),
			TEXT("AvidScriptCSharpGuest/Profiles/profile_phase54_6_generated_s1/")
				TEXT("profile_phase54_6_generated_s1.avidscript.json"),
			*Fixture,
			Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}

	constexpr int32 WorkloadCount = 10;
	constexpr int32 SmokeLaneCount = 5;
	uint32 NativeAggregate = 0;
	uint32 ReflectionAggregate = 0;
	uint32 StaticAggregate = 0;
	uint32 AvidScriptWasmtimeSemanticAggregate = 0;
	uint32 AvidScriptWasmtimeNativeDirectAggregate = 0;
	uint64 AvidScriptWasmtimeSemanticHostCallCount = 0;
	uint64 AvidScriptWasmtimeNativeDirectHostCallCount = 0;

	for (int32 WorkloadIndex = 0; WorkloadIndex < WorkloadCount; ++WorkloadIndex)
	{
		const EAvidScriptPerfWorkload Workload = static_cast<EAvidScriptPerfWorkload>(WorkloadIndex);
		const uint32 WorkloadSeed = MakePerfRunnerWorkloadSeed(Seed, WorkloadIndex);
		const FPerfOracle Oracle = RunNativeOracle(
			*Fixture,
			Workload,
			IterationsPerWorkload,
			WorkloadSeed);
		for (int32 LaneIndex = 0;
			LaneIndex < SmokeLaneCount;
			++LaneIndex)
		{
			const EAvidScriptPerfLane Lane =
				static_cast<EAvidScriptPerfLane>(LaneIndex);
			FPerfLaneObservation Observation;
			if (!RunPerfLane(
					*Fixture,
					Reflection,
					Static,
					AvidScript,
					Lane,
					Workload,
					IterationsPerWorkload,
					WorkloadSeed,
					true,
					false,
					Observation,
					Error) ||
				!ValidatePerfObservation(
					Observation,
					Oracle,
					Workload,
					IterationsPerWorkload,
					0.25,
					Error))
			{
				OutResult.Error = MoveTemp(Error);
				return false;
			}

			switch (Lane)
			{
			case EAvidScriptPerfLane::NativeCpp:
				NativeAggregate = PerfRunnerMix(
					NativeAggregate ^ Observation.Checksum);
				break;
			case EAvidScriptPerfLane::PuertsV8Reflection:
				ReflectionAggregate = PerfRunnerMix(
					ReflectionAggregate ^ Observation.Checksum);
				break;
			case EAvidScriptPerfLane::PuertsV8Static:
				StaticAggregate = PerfRunnerMix(
					StaticAggregate ^ Observation.Checksum);
				break;
			case EAvidScriptPerfLane::AvidScriptWasmtimeSemantic:
				AvidScriptWasmtimeSemanticAggregate = PerfRunnerMix(
					AvidScriptWasmtimeSemanticAggregate ^ Observation.Checksum);
				AvidScriptWasmtimeSemanticHostCallCount += static_cast<uint64>(
					Observation.HostImportCallCount);
				break;
			case EAvidScriptPerfLane::AvidScriptWasmtimeGeneratedS1:
				AvidScriptWasmtimeNativeDirectAggregate = PerfRunnerMix(
					AvidScriptWasmtimeNativeDirectAggregate ^ Observation.Checksum);
				AvidScriptWasmtimeNativeDirectHostCallCount += static_cast<uint64>(
					Observation.HostImportCallCount);
				break;
			default:
				checkNoEntry();
				break;
			}
		}
	}

	OutResult.bSucceeded = true;
	OutResult.WorkloadCount = WorkloadCount;
	OutResult.NativeChecksum = NativeAggregate;
	OutResult.PuertsReflectionChecksum = ReflectionAggregate;
	OutResult.PuertsStaticChecksum = StaticAggregate;
	OutResult.AvidScriptWasmtimeSemanticChecksum = AvidScriptWasmtimeSemanticAggregate;
	OutResult.AvidScriptWasmtimeNativeDirectChecksum = AvidScriptWasmtimeNativeDirectAggregate;
	OutResult.AvidScriptWasmtimeSemanticHostCallCount = AvidScriptWasmtimeSemanticHostCallCount;
	OutResult.AvidScriptWasmtimeNativeDirectHostCallCount = AvidScriptWasmtimeNativeDirectHostCallCount;
	return true;
}
