#include "AvidScriptPerfRunner.h"

#include "AvidScriptPerfFixture.h"
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
	constexpr uint32 MixMultiplier = 1664525u;
	constexpr uint32 MixIncrement = 1013904223u;

	uint32 Mix(const uint32 Value)
	{
		return Value * MixMultiplier + MixIncrement;
	}

	class FAvidScriptPerfModuleLoader final : public puerts::IJSModuleLoader
	{
	public:
		explicit FAvidScriptPerfModuleLoader(FString InRoot)
			: Root(MoveTemp(InRoot))
		{
			FPaths::NormalizeDirectoryName(Root);
		}

		virtual bool Search(
			const FString& RequiredDir,
			const FString& RequiredModule,
			FString& Path,
			FString& AbsolutePath) override
		{
			FString Candidate = RequiredModule;
			if (FPaths::GetExtension(Candidate).IsEmpty())
			{
				Candidate += TEXT(".js");
			}
			if (FPaths::IsRelative(Candidate))
			{
				const FString Base = RequiredDir.IsEmpty() ? Root : RequiredDir;
				Candidate = FPaths::Combine(Base, Candidate);
			}
			Candidate = FPaths::ConvertRelativePathToFull(Candidate);
			FPaths::NormalizeFilename(Candidate);

			FString NormalizedRoot = FPaths::ConvertRelativePathToFull(Root);
			FPaths::NormalizeFilename(NormalizedRoot);
			if (!Candidate.StartsWith(NormalizedRoot + TEXT("/"), ESearchCase::IgnoreCase) ||
				!FPaths::FileExists(Candidate))
			{
				return false;
			}
			Path = Candidate;
			AbsolutePath = Candidate;
			return true;
		}

		virtual bool Load(const FString& Path, TArray<uint8>& Content) override
		{
			return FFileHelper::LoadFileToArray(Content, *Path);
		}

		virtual FString& GetScriptRoot() override
		{
			return Root;
		}

	private:
		FString Root;
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
			if (!Plugin.IsValid())
			{
				OutError = TEXT("AvidScriptPerfHarness plugin is not mounted");
				return false;
			}

			const FString ScriptRoot = FPaths::Combine(Plugin->GetContentDir(), TEXT("JavaScript"));
			if (!FPaths::DirectoryExists(ScriptRoot))
			{
				OutError = FString::Printf(TEXT("Puerts workload root is missing: %s"), *ScriptRoot);
				return false;
			}

			Fixture = &SharedFixture;
			LaneId = InLaneId;
			Environment = MakeShared<puerts::FJsEnv>(
				std::make_shared<FAvidScriptPerfModuleLoader>(ScriptRoot),
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
				Accumulator = Mix(Accumulator ^ static_cast<uint32>(Index));
				break;
			case EAvidScriptPerfWorkload::ScalarNoOp:
				Accumulator = Mix(static_cast<uint32>(Fixture.NativeNoOp(static_cast<int32>(Accumulator))) ^
					static_cast<uint32>(Index));
				break;
			case EAvidScriptPerfWorkload::ScalarAddInt32:
				Accumulator = Mix(static_cast<uint32>(Fixture.NativeAddInt32(
					static_cast<int32>(Accumulator),
					Index)));
				break;
			case EAvidScriptPerfWorkload::PropertyGetSet:
				Fixture.ScalarValue = static_cast<int32>(Accumulator ^ static_cast<uint32>(Index));
				Accumulator = Mix(static_cast<uint32>(Fixture.ScalarValue));
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
				Accumulator = Mix(Accumulator ^ Packed);
				break;
			}
			case EAvidScriptPerfWorkload::ObjectRoundtrip:
				Accumulator = Mix(Accumulator ^
					(Fixture.NativeObjectRoundtrip(&Fixture) == &Fixture ? static_cast<uint32>(Index) : 0xffffffffu));
				break;
			case EAvidScriptPerfWorkload::BatchScalar:
				Accumulator = Mix(static_cast<uint32>(Fixture.NativeBatchAdd(
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
}

bool FAvidScriptPerfRunner::RunPuertsCorrectnessSmoke(
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

	constexpr int32 WorkloadCount = static_cast<int32>(EAvidScriptPerfWorkload::BatchScalar) + 1;
	uint32 NativeAggregate = 0;
	uint32 ReflectionAggregate = 0;
	uint32 StaticAggregate = 0;

	for (int32 WorkloadIndex = 0; WorkloadIndex < WorkloadCount; ++WorkloadIndex)
	{
		const EAvidScriptPerfWorkload Workload = static_cast<EAvidScriptPerfWorkload>(WorkloadIndex);
		const uint32 WorkloadSeed = static_cast<uint32>(Seed) ^ (0x9e3779b9u * static_cast<uint32>(WorkloadIndex + 1));
		const uint64 ExpectedCallCount = GetExpectedOperationCallCount(
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
		if (ReflectionChecksum != NativeChecksum || StaticChecksum != NativeChecksum)
		{
			OutResult.Error = FString::Printf(
				TEXT("checksum mismatch workload=%d native=%u reflection=%u static=%u"),
				WorkloadIndex,
				NativeChecksum,
				ReflectionChecksum,
				StaticChecksum);
			return false;
		}
		if (ReflectionFinalScalar != NativeFinalScalar || StaticFinalScalar != NativeFinalScalar)
		{
			OutResult.Error = FString::Printf(
				TEXT("final fixture state mismatch workload=%d native=%d reflection=%d static=%d"),
				WorkloadIndex,
				NativeFinalScalar,
				ReflectionFinalScalar,
				StaticFinalScalar);
			return false;
		}
		if (NativeCallCount != ExpectedCallCount ||
			ReflectionCallCount != ExpectedCallCount ||
			StaticCallCount != ExpectedCallCount)
		{
			OutResult.Error = FString::Printf(
				TEXT("operation count mismatch workload=%d expected=%llu native=%llu reflection=%llu static=%llu"),
				WorkloadIndex,
				ExpectedCallCount,
				NativeCallCount,
				ReflectionCallCount,
				StaticCallCount);
			return false;
		}

		NativeAggregate = Mix(NativeAggregate ^ NativeChecksum);
		ReflectionAggregate = Mix(ReflectionAggregate ^ ReflectionChecksum);
		StaticAggregate = Mix(StaticAggregate ^ StaticChecksum);
	}

	const int32 CallbackSeed = Seed ^ 0x5a5a5a5a;
	const uint32 ExpectedCallback = Mix(static_cast<uint32>(CallbackSeed));
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
	return true;
}
