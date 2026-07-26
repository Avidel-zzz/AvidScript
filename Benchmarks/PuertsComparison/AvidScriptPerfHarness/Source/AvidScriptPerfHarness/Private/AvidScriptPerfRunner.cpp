#include "AvidScriptPerfRunner.h"

#include "AvidScriptPerfFixture.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmReloadTypes.h"
#include "AvidScriptWasmRuntime.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "JSLogger.h"
#include "JSModuleLoader.h"
#include "JsEnv.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
	constexpr uint32 PerfRunnerMixMultiplier = 1664525u;
	constexpr uint32 PerfRunnerMixIncrement = 1013904223u;
	constexpr uint32 PerfRunnerExactSeedMask = 0x007fffffu;
	constexpr int32 PerfRunnerWorkloadShift = 24;
	constexpr int32 PerfRunnerIterationMask = 0x00ffffff;

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
	};

	struct FAvidScriptLane
	{
		FAvidScriptObjectRegistry Registry;
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmReloadManifest Manifest;
		TArray<uint8> Bytecode;
		const FAvidScriptWasmStateSlot* ResultSlot = nullptr;
		int32 LastHostImportCallCount = 0;

		bool Initialize(
			AAvidScriptPerfFixture& SharedFixture,
			FString& OutError)
		{
			const FString ManifestPath = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("AvidScriptCSharpGuest/Profiles/profile_phase53_perf/")
				TEXT("profile_phase53_perf.avidscript.json"));
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
			Runtime.SetHostContext(HostContext);

			FAvidScriptWasmSmokeResult SmokeResult;
			if (!Runtime.LoadModule(
					Bytecode.GetData(),
					Bytecode.Num(),
					Manifest.ModuleId,
					Manifest.BindingPackage,
					Manifest.DebugMap,
					SmokeResult)
				|| !Runtime.ValidateRequiredExports(
					Manifest.RequiredExports,
					SmokeResult)
				|| !Runtime.BeginPlay(SmokeResult))
			{
				OutError = FString::Printf(
					TEXT("AvidScript benchmark runtime initialization failed: %s"),
					*SmokeResult.ErrorMessage);
				return false;
			}
			LastHostImportCallCount = SmokeResult.HostImportCallCount;
			return true;
		}

		bool RunWorkload(
			const EAvidScriptPerfWorkload Workload,
			const int32 Iterations,
			const uint32 Seed,
			uint32& OutChecksum,
			int32& OutHostImportCallCount,
			FString& OutError)
		{
			const int32 WorkloadId = static_cast<int32>(Workload);
			if (WorkloadId < 0 ||
				WorkloadId > 0x7f ||
				Iterations <= 0 ||
				Iterations > PerfRunnerIterationMask ||
				Seed > PerfRunnerExactSeedMask)
			{
				OutError = TEXT("AvidScript benchmark event packing arguments are out of range");
				return false;
			}

			const int32 PackedWorkload =
				(WorkloadId << PerfRunnerWorkloadShift) |
				(Iterations & PerfRunnerIterationMask);
			FAvidScriptWasmSmokeResult SmokeResult;
			if (!Runtime.DispatchEvent(
				PackedWorkload,
				static_cast<float>(Seed),
				SmokeResult))
			{
				OutError = FString::Printf(
					TEXT("AvidScript benchmark workload dispatch failed: %s"),
					*SmokeResult.ErrorMessage);
				return false;
			}

			int32 Checksum = 0;
			FString ReadError;
			if (!Runtime.ReadStateBytes(
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
				SmokeResult.HostImportCallCount - LastHostImportCallCount;
			LastHostImportCallCount = SmokeResult.HostImportCallCount;
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
			return static_cast<uint64>(Iterations);
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
			return Iterations * 2;
		case EAvidScriptPerfWorkload::ScalarNoOp:
		case EAvidScriptPerfWorkload::ScalarAddInt32:
		case EAvidScriptPerfWorkload::VectorValue:
		case EAvidScriptPerfWorkload::ObjectRoundtrip:
		case EAvidScriptPerfWorkload::BatchScalar:
			return Iterations;
		default:
			return 0;
		}
	}
}

bool FAvidScriptPerfRunner::RunFourLaneCorrectnessSmoke(
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
	if (!Reflection.Initialize(
		TEXT("reflection.js"),
		AAvidScriptPerfFixture::ReflectionLaneId,
		*Fixture,
		Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}
	FPuertsLane Static;
	if (!Static.Initialize(
		TEXT("static.js"),
		AAvidScriptPerfFixture::StaticLaneId,
		*Fixture,
		Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}
	FAvidScriptLane AvidScript;
	if (!AvidScript.Initialize(*Fixture, Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}

	constexpr int32 WorkloadCount = static_cast<int32>(EAvidScriptPerfWorkload::BatchScalar) + 1;
	uint32 NativeAggregate = 0;
	uint32 ReflectionAggregate = 0;
	uint32 StaticAggregate = 0;
	uint32 AvidScriptAggregate = 0;
	uint64 AvidScriptHostCallCount = 0;

	for (int32 WorkloadIndex = 0; WorkloadIndex < WorkloadCount; ++WorkloadIndex)
	{
		const EAvidScriptPerfWorkload Workload = static_cast<EAvidScriptPerfWorkload>(WorkloadIndex);
		const uint32 WorkloadSeed = MakePerfRunnerWorkloadSeed(Seed, WorkloadIndex);
		const uint64 ExpectedCallCount = GetExpectedOperationCallCount(
			Workload,
			IterationsPerWorkload);
		const int32 ExpectedAvidScriptHostCallCount =
			GetExpectedAvidScriptHostCallCount(
				Workload,
				IterationsPerWorkload);

		Fixture->ScalarValue = 0;
		Fixture->ResetOperationCounts();
		const uint32 NativeChecksum = RunNativeWorkload(
			*Fixture,
			Workload,
			IterationsPerWorkload,
			WorkloadSeed);
		const int32 NativeFinalScalar = Fixture->ScalarValue;
		const uint64 NativeCallCount = Fixture->GetOperationCallCount(WorkloadIndex);

		Fixture->ScalarValue = 0;
		Fixture->ResetOperationCounts();
		const uint32 ReflectionChecksum = static_cast<uint32>(Fixture->RunPuertsWorkload(
			Reflection.LaneId,
			WorkloadIndex,
			IterationsPerWorkload,
			static_cast<int32>(WorkloadSeed)));
		const int32 ReflectionFinalScalar = Fixture->ScalarValue;
		const uint64 ReflectionCallCount = Fixture->GetOperationCallCount(WorkloadIndex);

		Fixture->ScalarValue = 0;
		Fixture->ResetOperationCounts();
		const uint32 StaticChecksum = static_cast<uint32>(Fixture->RunPuertsWorkload(
			Static.LaneId,
			WorkloadIndex,
			IterationsPerWorkload,
			static_cast<int32>(WorkloadSeed)));
		const int32 StaticFinalScalar = Fixture->ScalarValue;
		const uint64 StaticCallCount = Fixture->GetOperationCallCount(WorkloadIndex);

		Fixture->ScalarValue = 0;
		Fixture->ResetOperationCounts();
		uint32 AvidScriptChecksum = 0;
		int32 AvidScriptWorkloadHostCallCount = 0;
		if (!AvidScript.RunWorkload(
			Workload,
			IterationsPerWorkload,
			WorkloadSeed,
			AvidScriptChecksum,
			AvidScriptWorkloadHostCallCount,
			Error))
		{
			OutResult.Error = MoveTemp(Error);
			return false;
		}
		const int32 AvidScriptFinalScalar = Fixture->ScalarValue;
		const uint64 AvidScriptCallCount =
			Fixture->GetOperationCallCount(WorkloadIndex);
		if (ReflectionChecksum != NativeChecksum ||
			StaticChecksum != NativeChecksum ||
			AvidScriptChecksum != NativeChecksum)
		{
			OutResult.Error = FString::Printf(
				TEXT("checksum mismatch workload=%d native=%u reflection=%u static=%u avidscript=%u"),
				WorkloadIndex,
				NativeChecksum,
				ReflectionChecksum,
				StaticChecksum,
				AvidScriptChecksum);
			return false;
		}
		if (ReflectionFinalScalar != NativeFinalScalar ||
			StaticFinalScalar != NativeFinalScalar ||
			AvidScriptFinalScalar != NativeFinalScalar)
		{
			OutResult.Error = FString::Printf(
				TEXT("final fixture state mismatch workload=%d native=%d reflection=%d static=%d avidscript=%d"),
				WorkloadIndex,
				NativeFinalScalar,
				ReflectionFinalScalar,
				StaticFinalScalar,
				AvidScriptFinalScalar);
			return false;
		}
		if (NativeCallCount != ExpectedCallCount ||
			ReflectionCallCount != ExpectedCallCount ||
			StaticCallCount != ExpectedCallCount ||
			AvidScriptCallCount != ExpectedCallCount)
		{
			OutResult.Error = FString::Printf(
				TEXT("operation count mismatch workload=%d expected=%llu native=%llu reflection=%llu static=%llu avidscript=%llu"),
				WorkloadIndex,
				ExpectedCallCount,
				NativeCallCount,
				ReflectionCallCount,
				StaticCallCount,
				AvidScriptCallCount);
			return false;
		}
		if (AvidScriptWorkloadHostCallCount != ExpectedAvidScriptHostCallCount)
		{
			OutResult.Error = FString::Printf(
				TEXT("AvidScript host call mismatch workload=%d expected=%d actual=%d"),
				WorkloadIndex,
				ExpectedAvidScriptHostCallCount,
				AvidScriptWorkloadHostCallCount);
			return false;
		}

		NativeAggregate = PerfRunnerMix(NativeAggregate ^ NativeChecksum);
		ReflectionAggregate = PerfRunnerMix(ReflectionAggregate ^ ReflectionChecksum);
		StaticAggregate = PerfRunnerMix(StaticAggregate ^ StaticChecksum);
		AvidScriptAggregate = PerfRunnerMix(AvidScriptAggregate ^ AvidScriptChecksum);
		AvidScriptHostCallCount +=
			static_cast<uint64>(AvidScriptWorkloadHostCallCount);
	}

	const int32 CallbackSeed = Seed ^ 0x5a5a5a5a;
	const uint32 ExpectedCallback = PerfRunnerMix(static_cast<uint32>(CallbackSeed));
	const uint32 ReflectionCallback =
		static_cast<uint32>(Fixture->RunPuertsEmptyCallback(Reflection.LaneId, CallbackSeed));
	const uint32 StaticCallback =
		static_cast<uint32>(Fixture->RunPuertsEmptyCallback(Static.LaneId, CallbackSeed));
	if (ReflectionCallback != ExpectedCallback || StaticCallback != ExpectedCallback)
	{
		OutResult.Error = FString::Printf(
			TEXT("callback checksum mismatch expected=%u reflection=%u static=%u"),
			ExpectedCallback,
			ReflectionCallback,
			StaticCallback);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.WorkloadCount = WorkloadCount;
	OutResult.NativeChecksum = NativeAggregate;
	OutResult.PuertsReflectionChecksum = ReflectionAggregate;
	OutResult.PuertsStaticChecksum = StaticAggregate;
	OutResult.AvidScriptChecksum = AvidScriptAggregate;
	OutResult.AvidScriptHostCallCount = AvidScriptHostCallCount;
	return true;
}
