#include "AvidScriptPerfRunner.h"

#include "AvidScriptPerfFixture.h"
#include "Interfaces/IPluginManager.h"
#include "JSLogger.h"
#include "JSModuleLoader.h"
#include "JsEnv.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

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
		TStrongObjectPtr<UAvidScriptPerfFixture> Fixture;
		TSharedPtr<puerts::FJsEnv> Environment;

		bool Initialize(const FString& ModuleName, FString& OutError)
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

			Fixture = TStrongObjectPtr<UAvidScriptPerfFixture>(NewObject<UAvidScriptPerfFixture>());
			Environment = MakeShared<puerts::FJsEnv>(
				std::make_shared<FAvidScriptPerfModuleLoader>(ScriptRoot),
				std::make_shared<puerts::FDefaultLogger>(),
				-1);
			Environment->Start(
				ModuleName,
				{ TPair<FString, UObject*>(TEXT("Fixture"), Fixture.Get()) });
			if (!Fixture->HasPuertsCallbacks())
			{
				OutError = FString::Printf(TEXT("Puerts module did not register callbacks: %s"), *ModuleName);
				return false;
			}
			return true;
		}
	};

	uint32 RunNativeWorkload(
		UAvidScriptPerfFixture& Fixture,
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
				Fixture.NativeSetScalar(static_cast<int32>(Accumulator ^ static_cast<uint32>(Index)));
				Accumulator = Mix(static_cast<uint32>(Fixture.NativeGetScalar()));
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

	FString Error;
	FPuertsLane Reflection;
	if (!Reflection.Initialize(TEXT("reflection.js"), Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}
	FPuertsLane Static;
	if (!Static.Initialize(TEXT("static.js"), Error))
	{
		OutResult.Error = MoveTemp(Error);
		return false;
	}

	TStrongObjectPtr<UAvidScriptPerfFixture> NativeFixture(NewObject<UAvidScriptPerfFixture>());
	constexpr int32 WorkloadCount = static_cast<int32>(EAvidScriptPerfWorkload::BatchScalar) + 1;
	uint32 NativeAggregate = 0;
	uint32 ReflectionAggregate = 0;
	uint32 StaticAggregate = 0;

	for (int32 WorkloadIndex = 0; WorkloadIndex < WorkloadCount; ++WorkloadIndex)
	{
		const EAvidScriptPerfWorkload Workload = static_cast<EAvidScriptPerfWorkload>(WorkloadIndex);
		const uint32 WorkloadSeed = static_cast<uint32>(Seed) ^ (0x9e3779b9u * static_cast<uint32>(WorkloadIndex + 1));
		NativeFixture->ScalarValue = 0;
		Reflection.Fixture->ScalarValue = 0;
		Static.Fixture->ScalarValue = 0;

		const uint32 NativeChecksum = RunNativeWorkload(
			*NativeFixture,
			Workload,
			IterationsPerWorkload,
			WorkloadSeed);
		const uint32 ReflectionChecksum = static_cast<uint32>(Reflection.Fixture->RunPuertsWorkload(
			WorkloadIndex,
			IterationsPerWorkload,
			static_cast<int32>(WorkloadSeed)));
		const uint32 StaticChecksum = static_cast<uint32>(Static.Fixture->RunPuertsWorkload(
			WorkloadIndex,
			IterationsPerWorkload,
			static_cast<int32>(WorkloadSeed)));
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

		NativeAggregate = Mix(NativeAggregate ^ NativeChecksum);
		ReflectionAggregate = Mix(ReflectionAggregate ^ ReflectionChecksum);
		StaticAggregate = Mix(StaticAggregate ^ StaticChecksum);
	}

	const int32 CallbackSeed = Seed ^ 0x5a5a5a5a;
	const uint32 ExpectedCallback = Mix(static_cast<uint32>(CallbackSeed));
	const uint32 ReflectionCallback =
		static_cast<uint32>(Reflection.Fixture->RunPuertsEmptyCallback(CallbackSeed));
	const uint32 StaticCallback =
		static_cast<uint32>(Static.Fixture->RunPuertsEmptyCallback(CallbackSeed));
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
