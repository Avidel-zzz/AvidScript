#include "AvidScriptControlledRuntimeRunner.h"

#include "AvidScriptPerfFixture.h"
#include "AvidScriptVmBackend.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "JSLogger.h"
#include "JSModuleLoader.h"
#include "JsEnv.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
#include <v8.h>
THIRD_PARTY_INCLUDES_END

namespace
{
	constexpr int32 ControlledRuntimeSchemaVersion = 1;
	constexpr uint32 ControlledRuntimeMixConstant = 0x6d2b79f5u;
	constexpr uint32 ControlledRuntimeMultiplier = 1664525u;
	constexpr uint32 ControlledRuntimeIncrement = 1013904223u;
	constexpr int32 ControlledRuntimeLaneCount = 4;
	constexpr int32 ControlledRuntimeMaximumWarmups = 100;
	constexpr int32 ControlledRuntimeMaximumTimedSamples = 1000;

	enum class EControlledRuntimeLane : uint8
	{
		PuertsV8WasmJit,
		AvidScriptWasmtimeCraneliftJit,
		AvidScriptWamrInterpreter,
		NativeCppReference
	};

	const TCHAR* GetLaneId(const EControlledRuntimeLane Lane)
	{
		switch (Lane)
		{
		case EControlledRuntimeLane::PuertsV8WasmJit:
			return TEXT("puerts_v8_wasm_jit");
		case EControlledRuntimeLane::AvidScriptWasmtimeCraneliftJit:
			return TEXT("avidscript_wasmtime_cranelift_jit");
		case EControlledRuntimeLane::AvidScriptWamrInterpreter:
			return TEXT("avidscript_wamr_interpreter");
		case EControlledRuntimeLane::NativeCppReference:
			return TEXT("native_cpp_reference");
		default:
			return TEXT("invalid");
		}
	}

	const EControlledRuntimeLane ControlledRuntimeLanes[] = {
		EControlledRuntimeLane::PuertsV8WasmJit,
		EControlledRuntimeLane::AvidScriptWasmtimeCraneliftJit,
		EControlledRuntimeLane::AvidScriptWamrInterpreter,
		EControlledRuntimeLane::NativeCppReference
	};

	uint32 RotateLeft13(const uint32 Value)
	{
		return (Value << 13u) | (Value >> 19u);
	}

	uint32 RunControlledRuntimeOracle(
		const int32 Iterations,
		const int32 Seed)
	{
		uint32 Value = static_cast<uint32>(Seed) ^ ControlledRuntimeMixConstant;
		for (uint32 Index = 0; Index < static_cast<uint32>(Iterations); ++Index)
		{
			Value = RotateLeft13(Value);
			Value = Value * ControlledRuntimeMultiplier + ControlledRuntimeIncrement;
			Value ^= static_cast<uint32>(Seed) + Index;
		}
		return Value;
	}

	bool ComputeSha256(
		const TConstArrayView<uint8> Bytes,
		FString& OutDigest)
	{
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		if (SHA256(
				Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest) == nullptr)
		{
			return false;
		}
		OutDigest.Reset(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			OutDigest += FString::Printf(TEXT("%02x"), Byte);
		}
		return true;
	}

	bool ReadJsonObject(
		const FString& Path,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("unable to read JSON file: %s"), *Path);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("invalid JSON object: %s"), *Path);
			return false;
		}
		return true;
	}

	bool GetRequiredString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name,
		FString& OutValue,
		FString& OutError)
	{
		if (!Object.IsValid() ||
			!Object->TryGetStringField(Name, OutValue) ||
			OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("required string is missing: %s"), Name);
			return false;
		}
		return true;
	}

	bool GetRequiredInteger(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name,
		int32& OutValue,
		FString& OutError)
	{
		double Number = 0.0;
		if (!Object.IsValid() ||
			!Object->TryGetNumberField(Name, Number) ||
			!FMath::IsFinite(Number) ||
			FMath::FloorToDouble(Number) != Number ||
			Number < static_cast<double>(MIN_int32) ||
			Number > static_cast<double>(MAX_int32))
		{
			OutError = FString::Printf(TEXT("required exact int32 is invalid: %s"), Name);
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	class FControlledRuntimeModuleLoader final : public puerts::IJSModuleLoader
	{
	public:
		FControlledRuntimeModuleLoader(
			FString InWorkloadRoot,
			FString InRuntimeRoot)
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
				if (!IsUnderAllowedRoot(Candidate) ||
					!FPaths::FileExists(Candidate))
				{
					continue;
				}
				Path = Candidate;
				AbsolutePath = Candidate;
				return true;
			}
			return false;
		}

		virtual bool Load(
			const FString& Path,
			TArray<uint8>& Content) override
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
					ESearchCase::IgnoreCase) ||
				Candidate.StartsWith(
					RuntimeRoot + TEXT("/"),
					ESearchCase::IgnoreCase);
		}

		FString WorkloadRoot;
		FString RuntimeRoot;
	};

	struct FControlledPuertsLane
	{
		TSharedPtr<puerts::FJsEnv> Environment;
		AAvidScriptPerfFixture* Fixture = nullptr;
		FString RuntimeVersion;

		bool Initialize(
			AAvidScriptPerfFixture& InFixture,
			const TConstArrayView<uint8> WasmBytes,
			FString& OutError)
		{
			const TSharedPtr<IPlugin> HarnessPlugin =
				IPluginManager::Get().FindPlugin(TEXT("AvidScriptPerfHarness"));
			const TSharedPtr<IPlugin> PuertsPlugin =
				IPluginManager::Get().FindPlugin(TEXT("Puerts"));
			if (!HarnessPlugin.IsValid() || !PuertsPlugin.IsValid())
			{
				OutError = TEXT("AvidScriptPerfHarness or Puerts plugin is not mounted");
				return false;
			}
			const FString ScriptRoot = FPaths::Combine(
				HarnessPlugin->GetContentDir(),
				TEXT("JavaScript"));
			const FString RuntimeRoot = FPaths::Combine(
				PuertsPlugin->GetContentDir(),
				TEXT("JavaScript"));
			if (!FPaths::DirectoryExists(ScriptRoot) ||
				!FPaths::DirectoryExists(RuntimeRoot))
			{
				OutError = TEXT("Puerts controlled runtime script roots are missing");
				return false;
			}

			Fixture = &InFixture;
			Fixture->SetControlledWasmBytes(WasmBytes);
			Environment = MakeShared<puerts::FJsEnv>(
				std::make_shared<FControlledRuntimeModuleLoader>(
					ScriptRoot,
					RuntimeRoot),
				std::make_shared<puerts::FDefaultLogger>(),
				-1);
			Environment->Start(
				TEXT("controlled_wasm.js"),
				{ TPair<FString, UObject*>(TEXT("Fixture"), Fixture) });
			if (!Fixture->HasControlledWasmRunner() ||
				!Fixture->ControlledRunnerUsesWebAssembly())
			{
				OutError = TEXT(
					"Puerts controlled lane did not register a WebAssembly.Module/Instance runner");
				return false;
			}
			RuntimeVersion = UTF8_TO_TCHAR(v8::V8::GetVersion());
			if (RuntimeVersion.IsEmpty())
			{
				OutError = TEXT("V8 runtime version is unavailable");
				return false;
			}
			return true;
		}

		bool Run(
			const int32 Iterations,
			const int32 Seed,
			int32& OutResult,
			FString& OutError) const
		{
			if (Fixture == nullptr || !Fixture->HasControlledWasmRunner())
			{
				OutError = TEXT("Puerts controlled lane is not initialized");
				return false;
			}
			OutResult = Fixture->RunControlledWasm(Iterations, Seed);
			return true;
		}
	};

	struct FControlledVmLane
	{
		TUniquePtr<IAvidScriptVmBackend> Backend;
		FAvidScriptVmExportHandle RunExport;
		FAvidScriptVmBackendInfo BackendInfo;
		EAvidScriptVmBackendKind ExpectedKind = EAvidScriptVmBackendKind::Wamr;
		EAvidScriptVmExecutionMode ExpectedMode =
			EAvidScriptVmExecutionMode::Interpreter;

		bool Initialize(
			const EAvidScriptVmBackendKind Kind,
			const EAvidScriptVmExecutionMode Mode,
			const TConstArrayView<uint8> WasmBytes,
			const FString& WasmSha256,
			FString& OutError)
		{
			ExpectedKind = Kind;
			ExpectedMode = Mode;
			FAvidScriptVmBackendSelection Selection;
			Selection.BackendKind = Kind;
			Selection.ExecutionMode = Mode;
			Selection.ArtifactFormat =
				EAvidScriptVmArtifactFormat::WasmBytecode;
			Selection.bAllowFallback = false;
			FAvidScriptVmError VmError;
			Backend = CreateAvidScriptVmBackend(Selection, VmError);
			if (!Backend.IsValid())
			{
				OutError = FString::Printf(
					TEXT("backend creation failed: %s %s"),
					*VmError.Category,
					*VmError.Details);
				return false;
			}
			FAvidScriptVmLoadConfig LoadConfig;
			if (!Backend->Load(
					WasmBytes,
					WasmSha256,
					LoadConfig,
					VmError))
			{
				OutError = FString::Printf(
					TEXT("backend load failed: %s %s"),
					*VmError.Category,
					*VmError.Details);
				return false;
			}
			if (!Backend->ResolveExport(TEXT("run"), RunExport, VmError))
			{
				OutError = FString::Printf(
					TEXT("run export resolve failed: %s %s"),
					*VmError.Category,
					*VmError.Details);
				return false;
			}
			BackendInfo = Backend->GetBackendInfo();
			if (BackendInfo.Kind != ExpectedKind ||
				BackendInfo.ExecutionMode != ExpectedMode ||
				BackendInfo.ArtifactFormat !=
					EAvidScriptVmArtifactFormat::WasmBytecode ||
				Backend->GetExportLookupCount() != 1)
			{
				OutError = TEXT(
					"backend identity, mode, artifact, or export-cache contract mismatch");
				return false;
			}
			return true;
		}

		bool Run(
			const int32 Iterations,
			const int32 Seed,
			int32& OutResult,
			FString& OutError)
		{
			FAvidScriptVmCallFrame Frame;
			Frame.Cells[0] = static_cast<uint32>(Iterations);
			Frame.Cells[1] = static_cast<uint32>(Seed);
			Frame.CellCount = 2;
			FAvidScriptVmCallResult Result;
			FAvidScriptVmError VmError;
			if (!Backend->Call(RunExport, Frame, VmError, &Result))
			{
				OutError = FString::Printf(
					TEXT("backend run failed: %s %s"),
					*VmError.Category,
					*VmError.Details);
				return false;
			}
			if (Result.CellCount != 1)
			{
				OutError = FString::Printf(
					TEXT("run export returned %u cells instead of one i32"),
					Result.CellCount);
				return false;
			}
			OutResult = static_cast<int32>(Result.Cells[0]);
			return true;
		}
	};

	struct FControlledRuntimeEnvironment
	{
		UWorld* World = nullptr;
		AAvidScriptPerfFixture* Fixture = nullptr;
		FControlledPuertsLane Puerts;
		FControlledVmLane Wasmtime;
		FControlledVmLane Wamr;

		~FControlledRuntimeEnvironment()
		{
			if (World != nullptr)
			{
				if (GEngine != nullptr)
				{
					GEngine->DestroyWorldContext(World);
				}
				World->DestroyWorld(false);
				World = nullptr;
			}
		}

		bool Initialize(
			const TConstArrayView<uint8> WasmBytes,
			const FString& WasmSha256,
			FString& OutError)
		{
			if (GEngine == nullptr)
			{
				OutError = TEXT("GEngine is unavailable");
				return false;
			}
			World = UWorld::CreateWorld(
				EWorldType::Game,
				false,
				TEXT("AvidScriptControlledRuntimeWorld"));
			if (World == nullptr)
			{
				OutError = TEXT("unable to create controlled runtime world");
				return false;
			}
			FWorldContext& WorldContext =
				GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			Fixture = World->SpawnActor<AAvidScriptPerfFixture>();
			if (Fixture == nullptr)
			{
				OutError = TEXT("unable to spawn controlled runtime fixture");
				return false;
			}
			if (!Puerts.Initialize(*Fixture, WasmBytes, OutError))
			{
				return false;
			}
			if (!Wasmtime.Initialize(
					EAvidScriptVmBackendKind::Wasmtime,
					EAvidScriptVmExecutionMode::Jit,
					WasmBytes,
					WasmSha256,
					OutError))
			{
				return false;
			}
			if (!Wamr.Initialize(
					EAvidScriptVmBackendKind::Wamr,
					EAvidScriptVmExecutionMode::Interpreter,
					WasmBytes,
					WasmSha256,
					OutError))
			{
				return false;
			}
			return true;
		}

		bool Run(
			const EControlledRuntimeLane Lane,
			const int32 Iterations,
			const int32 Seed,
			int32& OutResult,
			FString& OutError)
		{
			switch (Lane)
			{
			case EControlledRuntimeLane::PuertsV8WasmJit:
				return Puerts.Run(Iterations, Seed, OutResult, OutError);
			case EControlledRuntimeLane::AvidScriptWasmtimeCraneliftJit:
				return Wasmtime.Run(Iterations, Seed, OutResult, OutError);
			case EControlledRuntimeLane::AvidScriptWamrInterpreter:
				return Wamr.Run(Iterations, Seed, OutResult, OutError);
			case EControlledRuntimeLane::NativeCppReference:
				OutResult = static_cast<int32>(
					RunControlledRuntimeOracle(Iterations, Seed));
				return true;
			default:
				OutError = TEXT("invalid controlled runtime lane");
				return false;
			}
		}
	};

	struct FControlledRequest
	{
		FString Mode;
		int32 ProcessRun = -1;
		int32 WarmupSamples = 0;
		int32 TimedSamples = 0;
		int32 MinimumIterations = 0;
		int32 MaximumIterations = 0;
		int32 Seed = 0;
		double MinimumSampleMilliseconds = 0.0;
		FString KernelWasmPath;
		FString KernelWasmSha256;
		FString PuertsCommit;
		FString PuertsBackendSha256;
		FString TargetTriple;
		TMap<FString, int32> Iterations;
	};

	bool ParseControlledRequest(
		const TSharedPtr<FJsonObject>& Object,
		FControlledRequest& OutRequest,
		FString& OutError)
	{
		int32 SchemaVersion = 0;
		FString BenchmarkKind;
		if (!GetRequiredInteger(
				Object,
				TEXT("schema_version"),
				SchemaVersion,
				OutError) ||
			SchemaVersion != ControlledRuntimeSchemaVersion ||
			!GetRequiredString(
				Object,
				TEXT("benchmark_kind"),
				BenchmarkKind,
				OutError) ||
			BenchmarkKind != TEXT("identical_wasm_kernel") ||
			!GetRequiredString(Object, TEXT("mode"), OutRequest.Mode, OutError) ||
			!GetRequiredInteger(
				Object,
				TEXT("process_run"),
				OutRequest.ProcessRun,
				OutError) ||
			!GetRequiredInteger(
				Object,
				TEXT("warmup_samples"),
				OutRequest.WarmupSamples,
				OutError) ||
			!GetRequiredInteger(
				Object,
				TEXT("timed_samples"),
				OutRequest.TimedSamples,
				OutError) ||
			!GetRequiredInteger(
				Object,
				TEXT("minimum_iterations"),
				OutRequest.MinimumIterations,
				OutError) ||
			!GetRequiredInteger(
				Object,
				TEXT("maximum_iterations"),
				OutRequest.MaximumIterations,
				OutError) ||
			!GetRequiredInteger(Object, TEXT("seed"), OutRequest.Seed, OutError) ||
			!GetRequiredString(
				Object,
				TEXT("kernel_wasm_path"),
				OutRequest.KernelWasmPath,
				OutError) ||
			!GetRequiredString(
				Object,
				TEXT("kernel_wasm_sha256"),
				OutRequest.KernelWasmSha256,
				OutError) ||
			!GetRequiredString(
				Object,
				TEXT("puerts_commit"),
				OutRequest.PuertsCommit,
				OutError) ||
			!GetRequiredString(
				Object,
				TEXT("puerts_backend_sha256"),
				OutRequest.PuertsBackendSha256,
				OutError) ||
			!GetRequiredString(
				Object,
				TEXT("target_triple"),
				OutRequest.TargetTriple,
				OutError))
		{
			return false;
		}
		if (!Object->TryGetNumberField(
				TEXT("minimum_sample_milliseconds"),
				OutRequest.MinimumSampleMilliseconds) ||
			!FMath::IsFinite(OutRequest.MinimumSampleMilliseconds) ||
			OutRequest.MinimumSampleMilliseconds <= 0.0)
		{
			OutError = TEXT("minimum_sample_milliseconds must be positive");
			return false;
		}
		if ((OutRequest.Mode != TEXT("calibration") &&
			 OutRequest.Mode != TEXT("timed")) ||
			OutRequest.WarmupSamples < 1 ||
			OutRequest.WarmupSamples > ControlledRuntimeMaximumWarmups ||
			OutRequest.TimedSamples < 0 ||
			OutRequest.TimedSamples > ControlledRuntimeMaximumTimedSamples ||
			OutRequest.MinimumIterations < 1 ||
			OutRequest.MaximumIterations < OutRequest.MinimumIterations ||
			OutRequest.KernelWasmSha256.Len() != 64 ||
			OutRequest.PuertsCommit.Len() != 40 ||
			OutRequest.PuertsBackendSha256.Len() != 64)
		{
			OutError = TEXT("controlled runtime request bounds or identities are invalid");
			return false;
		}
		if (OutRequest.Mode == TEXT("calibration"))
		{
			return OutRequest.ProcessRun == -1 &&
				OutRequest.TimedSamples == 0;
		}
		if (OutRequest.ProcessRun < 0 || OutRequest.TimedSamples < 1)
		{
			OutError = TEXT("timed request requires process_run >= 0 and timed_samples > 0");
			return false;
		}
		const TSharedPtr<FJsonObject>* IterationsObject = nullptr;
		if (!Object->TryGetObjectField(TEXT("iterations"), IterationsObject) ||
			IterationsObject == nullptr ||
			!IterationsObject->IsValid())
		{
			OutError = TEXT("timed request is missing iterations");
			return false;
		}
		for (const EControlledRuntimeLane Lane : ControlledRuntimeLanes)
		{
			int32 LaneIterations = 0;
			if (!GetRequiredInteger(
					*IterationsObject,
					GetLaneId(Lane),
					LaneIterations,
					OutError) ||
				LaneIterations < OutRequest.MinimumIterations ||
				LaneIterations > OutRequest.MaximumIterations)
			{
				return false;
			}
			OutRequest.Iterations.Add(GetLaneId(Lane), LaneIterations);
		}
		return true;
	}

	TSharedRef<FJsonObject> MakeLaneIdentity(
		const EControlledRuntimeLane Lane,
		const FControlledRuntimeEnvironment& Environment,
		const FControlledRequest& Request)
	{
		TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
		Identity->SetStringField(TEXT("lane_id"), GetLaneId(Lane));
		Identity->SetStringField(
			TEXT("wasm_workload_kind"),
			TEXT("identical_wasm_kernel"));
		Identity->SetStringField(
			TEXT("source_wasm_sha256"),
			Request.KernelWasmSha256);
		Identity->SetStringField(
			TEXT("execution_artifact_sha256"),
			Request.KernelWasmSha256);
		Identity->SetStringField(
			TEXT("target_triple"),
			Request.TargetTriple);
		Identity->SetBoolField(TEXT("fallback_used"), false);
		switch (Lane)
		{
		case EControlledRuntimeLane::PuertsV8WasmJit:
			Identity->SetStringField(
				TEXT("runtime_id"),
				TEXT("v8.webassembly.tiered_jit"));
			Identity->SetStringField(
				TEXT("runtime_version"),
				Environment.Puerts.RuntimeVersion);
			Identity->SetStringField(
				TEXT("adapter_id"),
				TEXT("puerts_webassembly_module_instance"));
			Identity->SetStringField(
				TEXT("execution_tier"),
				TEXT("tiered_jit"));
			Identity->SetStringField(
				TEXT("execution_artifact_format"),
				TEXT("wasm_bytecode"));
			Identity->SetStringField(
				TEXT("compiler_identity"),
				TEXT("v8.webassembly.tiered_jit"));
			Identity->SetStringField(TEXT("compiler_flags"), TEXT("host_default"));
			Identity->SetStringField(
				TEXT("runtime_build_identity"),
				Request.PuertsCommit);
			Identity->SetStringField(
				TEXT("runtime_artifact_sha256"),
				Request.PuertsBackendSha256);
			break;
		case EControlledRuntimeLane::AvidScriptWasmtimeCraneliftJit:
			Identity->SetStringField(
				TEXT("runtime_id"),
				TEXT("wasmtime.cranelift.jit"));
			Identity->SetStringField(
				TEXT("runtime_version"),
				Environment.Wasmtime.BackendInfo.RuntimeVersion);
			Identity->SetStringField(
				TEXT("adapter_id"),
				TEXT("avidscript_vm_backend"));
			Identity->SetStringField(TEXT("execution_tier"), TEXT("jit"));
			Identity->SetStringField(
				TEXT("execution_artifact_format"),
				TEXT("wasm_bytecode"));
			Identity->SetStringField(
				TEXT("compiler_identity"),
				TEXT("cranelift"));
			Identity->SetStringField(TEXT("compiler_flags"), TEXT("host_default"));
			Identity->SetStringField(
				TEXT("runtime_build_identity"),
				Environment.Wasmtime.BackendInfo.RuntimeBuildIdentity);
			Identity->SetStringField(
				TEXT("runtime_artifact_sha256"),
				Environment.Wasmtime.BackendInfo.RuntimeArtifactSha256);
			break;
		case EControlledRuntimeLane::AvidScriptWamrInterpreter:
			Identity->SetStringField(TEXT("runtime_id"), TEXT("wamr.interpreter"));
			Identity->SetStringField(
				TEXT("runtime_version"),
				Environment.Wamr.BackendInfo.RuntimeVersion);
			Identity->SetStringField(
				TEXT("adapter_id"),
				TEXT("avidscript_vm_backend"));
			Identity->SetStringField(
				TEXT("execution_tier"),
				TEXT("interpreter"));
			Identity->SetStringField(
				TEXT("execution_artifact_format"),
				TEXT("wasm_bytecode"));
			Identity->SetStringField(
				TEXT("compiler_identity"),
				TEXT("not_applicable"));
			Identity->SetStringField(
				TEXT("compiler_flags"),
				TEXT("fast_interp"));
			Identity->SetStringField(
				TEXT("runtime_build_identity"),
				Environment.Wamr.BackendInfo.RuntimeBuildIdentity);
			Identity->SetStringField(
				TEXT("runtime_artifact_sha256"),
				Environment.Wamr.BackendInfo.RuntimeArtifactSha256);
			break;
		case EControlledRuntimeLane::NativeCppReference:
			Identity->SetStringField(
				TEXT("runtime_id"),
				TEXT("unreal_engine_native"));
			Identity->SetStringField(TEXT("runtime_version"), FEngineVersion::Current().ToString());
			Identity->SetStringField(
				TEXT("adapter_id"),
				TEXT("native_cpp_oracle"));
			Identity->SetStringField(TEXT("execution_tier"), TEXT("native"));
			Identity->SetStringField(
				TEXT("execution_artifact_format"),
				TEXT("pe_coff"));
			Identity->SetStringField(
				TEXT("compiler_identity"),
				TEXT("ue58_msvc"));
			Identity->SetStringField(
				TEXT("compiler_flags"),
				TEXT("Development Editor"));
			Identity->SetStringField(
				TEXT("runtime_build_identity"),
				TEXT("unreal_editor_process"));
			Identity->SetStringField(
				TEXT("runtime_artifact_sha256"),
				TEXT("not_applicable"));
			break;
		}
		return Identity;
	}

	bool RunLaneAndValidate(
		FControlledRuntimeEnvironment& Environment,
		const EControlledRuntimeLane Lane,
		const int32 Iterations,
		const int32 Seed,
		int32& OutActual,
		uint64& OutDurationNanoseconds,
		FString& OutError)
	{
		const uint64 StartCycles = FPlatformTime::Cycles64();
		const bool bRan = Environment.Run(
			Lane,
			Iterations,
			Seed,
			OutActual,
			OutError);
		const uint64 EndCycles = FPlatformTime::Cycles64();
		OutDurationNanoseconds = static_cast<uint64>(
			static_cast<double>(EndCycles - StartCycles) *
			FPlatformTime::GetSecondsPerCycle64() *
			1000000000.0);
		if (!bRan)
		{
			return false;
		}
		const int32 Expected = static_cast<int32>(
			RunControlledRuntimeOracle(Iterations, Seed));
		if (OutActual != Expected)
		{
			OutError = FString::Printf(
				TEXT("oracle mismatch lane=%s iterations=%d seed=%d actual=%d expected=%d"),
				GetLaneId(Lane),
				Iterations,
				Seed,
				OutActual,
				Expected);
			return false;
		}
		return true;
	}

	bool SaveJson(
		const FString& Path,
		const TSharedRef<FJsonObject>& Object,
		FString& OutError)
	{
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		if (!FJsonSerializer::Serialize(Object, Writer))
		{
			OutError = TEXT("unable to serialize controlled runtime result");
			return false;
		}
		if (!FFileHelper::SaveStringToFile(
				Json,
				*Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(
				TEXT("unable to publish controlled runtime result: %s"),
				*Path);
			return false;
		}
		return true;
	}
}

bool FAvidScriptControlledRuntimeRunner::RunFromFiles(
	const FString& RequestPath,
	const FString& ResultPath,
	FString& OutError)
{
	TSharedPtr<FJsonObject> RequestJson;
	if (!ReadJsonObject(RequestPath, RequestJson, OutError))
	{
		return false;
	}
	FControlledRequest Request;
	if (!ParseControlledRequest(RequestJson, Request, OutError))
	{
		return false;
	}
	TArray<uint8> WasmBytes;
	if (!FFileHelper::LoadFileToArray(WasmBytes, *Request.KernelWasmPath))
	{
		OutError = TEXT("unable to load tracked controlled runtime WASM");
		return false;
	}
	FString ActualWasmSha256;
	if (!ComputeSha256(WasmBytes, ActualWasmSha256) ||
		ActualWasmSha256 != Request.KernelWasmSha256)
	{
		OutError = FString::Printf(
			TEXT("controlled runtime WASM digest mismatch expected=%s actual=%s"),
			*Request.KernelWasmSha256,
			*ActualWasmSha256);
		return false;
	}

	FControlledRuntimeEnvironment Environment;
	if (!Environment.Initialize(WasmBytes, ActualWasmSha256, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("schema_version"), ControlledRuntimeSchemaVersion);
	Result->SetStringField(TEXT("benchmark_kind"), TEXT("identical_wasm_kernel"));
	Result->SetStringField(TEXT("mode"), Request.Mode);
	Result->SetNumberField(TEXT("process_run"), Request.ProcessRun);
	Result->SetNumberField(
		TEXT("pid"),
		static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	Result->SetStringField(TEXT("kernel_wasm_sha256"), ActualWasmSha256);
	Result->SetStringField(
		TEXT("timing_boundary"),
		TEXT("single_cached_export_call"));
	Result->SetBoolField(TEXT("compile_in_timed_region"), false);
	Result->SetBoolField(TEXT("instantiate_in_timed_region"), false);
	Result->SetBoolField(TEXT("export_lookup_in_timed_region"), false);
	Result->SetBoolField(TEXT("fallback_used"), false);

	TArray<TSharedPtr<FJsonValue>> LaneIdentities;
	for (const EControlledRuntimeLane Lane : ControlledRuntimeLanes)
	{
		LaneIdentities.Add(
			MakeShared<FJsonValueObject>(
				MakeLaneIdentity(Lane, Environment, Request)));
	}
	Result->SetArrayField(TEXT("lane_identities"), LaneIdentities);

	TSharedRef<FJsonObject> Calibration = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Samples;
	if (Request.Mode == TEXT("calibration"))
	{
		for (const EControlledRuntimeLane Lane : ControlledRuntimeLanes)
		{
			int32 Iterations = Request.MinimumIterations;
			uint64 DurationNanoseconds = 0;
			int32 Actual = 0;
			for (int32 Warmup = 0;
				Warmup < Request.WarmupSamples;
				++Warmup)
			{
				if (!RunLaneAndValidate(
						Environment,
						Lane,
						Iterations,
						Request.Seed + Warmup,
						Actual,
						DurationNanoseconds,
						OutError))
				{
					return false;
				}
			}
			while (true)
			{
				if (!RunLaneAndValidate(
						Environment,
						Lane,
						Iterations,
						Request.Seed + 1009,
						Actual,
						DurationNanoseconds,
						OutError))
				{
					return false;
				}
				if (static_cast<double>(DurationNanoseconds) >=
						Request.MinimumSampleMilliseconds * 1000000.0 ||
					Iterations == Request.MaximumIterations)
				{
					break;
				}
				const int64 Doubled = static_cast<int64>(Iterations) * 2;
				Iterations = static_cast<int32>(
					FMath::Min<int64>(Doubled, Request.MaximumIterations));
			}
			if (static_cast<double>(DurationNanoseconds) <
				Request.MinimumSampleMilliseconds * 1000000.0)
			{
				OutError = FString::Printf(
					TEXT("calibration did not reach minimum duration lane=%s iterations=%d"),
					GetLaneId(Lane),
					Iterations);
				return false;
			}
			TSharedRef<FJsonObject> LaneCalibration = MakeShared<FJsonObject>();
			LaneCalibration->SetNumberField(TEXT("iterations"), Iterations);
			LaneCalibration->SetNumberField(
				TEXT("duration_ns"),
				static_cast<double>(DurationNanoseconds));
			Calibration->SetObjectField(GetLaneId(Lane), LaneCalibration);
		}
	}
	else
	{
		for (const EControlledRuntimeLane Lane : ControlledRuntimeLanes)
		{
			const int32 Iterations = Request.Iterations.FindChecked(GetLaneId(Lane));
			for (int32 Warmup = 0;
				Warmup < Request.WarmupSamples;
				++Warmup)
			{
				int32 WarmupActual = 0;
				uint64 WarmupDuration = 0;
				const int32 WarmupSeed =
					Request.Seed + Request.ProcessRun * 1009 - Warmup - 1;
				if (!RunLaneAndValidate(
						Environment,
						Lane,
						Iterations,
						WarmupSeed,
						WarmupActual,
						WarmupDuration,
						OutError))
				{
					return false;
				}
			}
			for (int32 SampleIndex = 0;
				SampleIndex < Request.TimedSamples;
				++SampleIndex)
			{
				const int32 SampleSeed =
					Request.Seed + Request.ProcessRun * 1009 + SampleIndex * 17;
				int32 Actual = 0;
				uint64 DurationNanoseconds = 0;
				if (!RunLaneAndValidate(
						Environment,
						Lane,
						Iterations,
						SampleSeed,
						Actual,
						DurationNanoseconds,
						OutError))
				{
					return false;
				}
				const int32 Expected = static_cast<int32>(
					RunControlledRuntimeOracle(Iterations, SampleSeed));
				TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
				Sample->SetStringField(TEXT("lane_id"), GetLaneId(Lane));
				Sample->SetNumberField(TEXT("sample_index"), SampleIndex);
				Sample->SetNumberField(TEXT("iterations"), Iterations);
				Sample->SetNumberField(TEXT("seed"), SampleSeed);
				Sample->SetNumberField(
					TEXT("duration_ns"),
					static_cast<double>(DurationNanoseconds));
				Sample->SetNumberField(
					TEXT("ns_per_iteration"),
					static_cast<double>(DurationNanoseconds) /
						static_cast<double>(Iterations));
				Sample->SetNumberField(TEXT("result"), Actual);
				Sample->SetNumberField(TEXT("expected"), Expected);
				Sample->SetBoolField(TEXT("correct"), Actual == Expected);
				Sample->SetNumberField(TEXT("host_crossing_count"), 1);
				Samples.Add(MakeShared<FJsonValueObject>(Sample));
			}
		}
	}
	Result->SetObjectField(TEXT("calibration"), Calibration);
	Result->SetArrayField(TEXT("samples"), Samples);
	Result->SetNumberField(TEXT("correctness_failures"), 0);
	return SaveJson(ResultPath, Result, OutError);
}

bool FAvidScriptControlledRuntimeRunner::RunCorrectnessSmoke(
	const int32 Iterations,
	const int32 Seed,
	FAvidScriptControlledRuntimeSmokeResult& OutResult)
{
	OutResult = FAvidScriptControlledRuntimeSmokeResult();
	if (Iterations <= 0)
	{
		OutResult.Error = TEXT("Iterations must be positive");
		return false;
	}
	const TSharedPtr<IPlugin> AvidScriptPlugin =
		IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (!AvidScriptPlugin.IsValid())
	{
		OutResult.Error = TEXT("AvidScript plugin is not mounted");
		return false;
	}
	const FString WasmPath = FPaths::Combine(
		AvidScriptPlugin->GetBaseDir(),
		TEXT("Benchmarks/PuertsComparison/ControlledRuntime/Kernel/")
		TEXT("controlled_runtime_kernel.wasm"));
	TArray<uint8> WasmBytes;
	if (!FFileHelper::LoadFileToArray(WasmBytes, *WasmPath) ||
		!ComputeSha256(WasmBytes, OutResult.KernelWasmSha256))
	{
		OutResult.Error = TEXT("unable to load or hash controlled runtime kernel");
		return false;
	}
	FControlledRuntimeEnvironment Environment;
	if (!Environment.Initialize(
			WasmBytes,
			OutResult.KernelWasmSha256,
			OutResult.Error))
	{
		return false;
	}
	OutResult.Expected = static_cast<int32>(
		RunControlledRuntimeOracle(Iterations, Seed));
	FString Error;
	if (!Environment.Run(
			EControlledRuntimeLane::NativeCppReference,
			Iterations,
			Seed,
			OutResult.NativeResult,
			Error) ||
		!Environment.Run(
			EControlledRuntimeLane::PuertsV8WasmJit,
			Iterations,
			Seed,
			OutResult.PuertsV8Result,
			Error) ||
		!Environment.Run(
			EControlledRuntimeLane::AvidScriptWamrInterpreter,
			Iterations,
			Seed,
			OutResult.WamrResult,
			Error) ||
		!Environment.Run(
			EControlledRuntimeLane::AvidScriptWasmtimeCraneliftJit,
			Iterations,
			Seed,
			OutResult.WasmtimeResult,
			Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}
	OutResult.bPuertsExecutedWebAssembly =
		Environment.Fixture->ControlledRunnerUsesWebAssembly();
	OutResult.bSucceeded =
		OutResult.bPuertsExecutedWebAssembly &&
		OutResult.NativeResult == OutResult.Expected &&
		OutResult.PuertsV8Result == OutResult.Expected &&
		OutResult.WamrResult == OutResult.Expected &&
		OutResult.WasmtimeResult == OutResult.Expected;
	if (!OutResult.bSucceeded)
	{
		OutResult.Error = TEXT("controlled runtime smoke results differ");
	}
	return OutResult.bSucceeded;
}
