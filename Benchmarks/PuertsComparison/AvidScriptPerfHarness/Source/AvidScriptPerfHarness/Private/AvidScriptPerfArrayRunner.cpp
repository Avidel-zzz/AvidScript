#include "AvidScriptPerfArrayRunner.h"

#include "AvidScriptPerfFixture.h"
#include "AvidScriptWasmRuntime.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "JSLogger.h"
#include "JSModuleLoader.h"
#include "JsEnv.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
	constexpr uint32 ArrayMixMultiplier = 1664525u;
	constexpr uint32 ArrayMixIncrement = 1013904223u;
	constexpr uint32 FnvOffset = 2166136261u;
	constexpr uint32 FnvPrime = 16777619u;
	constexpr int32 ArrayTokenAddress = 0;
	constexpr int32 LogicalCallsAddress = 4;
	constexpr int32 FullHashAddress = 64;
	constexpr int32 ArrayElementBytes = sizeof(int32);
	const int32 RequiredSizes[] = { 1, 4, 16, 64, 256, 1024 };
	const FString PuertsLane(TEXT("puerts_v8_reflection_tarray"));
	const FString ElementLane(TEXT("avidscript_wasmtime_element"));
	const FString BulkLane(TEXT("avidscript_wasmtime_bulk"));
	const FString CompilerRegionLane(TEXT("avidscript_wasmtime_compiler_region"));

	uint32 Phase57ArrayMix(const uint32 Value)
	{
		return Value * ArrayMixMultiplier + ArrayMixIncrement;
	}

	uint32 HashValues(const TArray<int32>& Values)
	{
		uint32 Hash = FnvOffset;
		for (const int32 Value : Values)
		{
			Hash = (Hash ^ static_cast<uint32>(Value)) * FnvPrime;
		}
		return Hash;
	}

	FString FormatHash(const uint32 Hash)
	{
		return FString::Printf(TEXT("fnv1a32:%08x"), Hash);
	}

	TArray<int32> MakeExpectedValues(
		const int32 Size,
		const int32 LogicalCalls,
		const int32 Seed)
	{
		TArray<int32> Values;
		Values.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Values[Index] = static_cast<int32>(
				Phase57ArrayMix(static_cast<uint32>(Seed ^ Index)));
		}
		for (int32 Call = 0; Call < LogicalCalls; ++Call)
		{
			for (int32 Index = 0; Index < Size; ++Index)
			{
				Values[Index] = static_cast<int32>(Phase57ArrayMix(
					static_cast<uint32>(Values[Index] ^ Index)));
			}
		}
		return Values;
	}

	bool GetPhase57ArrayFileSha256(
		const FString& Path,
		FString& OutSha256,
		FString& OutError)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			OutError = FString::Printf(TEXT("Phase57Array artifact is unreadable: %s"), *Path);
			return false;
		}
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		if (SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest) == nullptr)
		{
			OutError = FString::Printf(TEXT("Phase57Array SHA-256 failed: %s"), *Path);
			return false;
		}
		OutSha256.Reset(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			OutSha256 += FString::Printf(TEXT("%02x"), Byte);
		}
		return true;
	}

	struct FArrayRequest
	{
		FString ProfileId;
		FString ProfileSha256;
		FString MeasurementLevel;
		int32 ProcessRun = 0;
		int32 WarmupSamples = 0;
		int32 TimedSamples = 0;
		int32 Seed = 0;
		TMap<int32, int32> LogicalCallsBySize;
		TArray<FString> LaneOrder;
		FString PuertsScriptSha256;
		FString PuertsRuntimeSha256;
		FString AvidScriptWasmPath;
		FString AvidScriptWasmSha256;
		FString AvidScriptCompilerWasmPath;
		FString AvidScriptCompilerWasmSha256;
		FString AvidScriptCompilerSourceSha256;
		FString AvidScriptCompilerReferenceSha256;
		FString AvidScriptCompilerGuestIrSha256;
		FString AvidScriptCompilerInspectionSha256;
	};

	bool ReadRequiredString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Name,
		FString& OutValue,
		FString& OutError)
	{
		if (!Object->TryGetStringField(Name, OutValue) || OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Phase57Array request is missing %s"), Name);
			return false;
		}
		return true;
	}

	bool ParseRequest(
		const FString& RequestPath,
		FArrayRequest& OutRequest,
		FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *RequestPath))
		{
			OutError = FString::Printf(TEXT("Phase57Array request is unreadable: %s"), *RequestPath);
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = TEXT("Phase57Array request is not valid JSON");
			return false;
		}
		FString Contract;
		if (!ReadRequiredString(Root, TEXT("contract"), Contract, OutError) ||
			Contract != TEXT("phase57_array_request.v2") ||
			!ReadRequiredString(Root, TEXT("profile_id"), OutRequest.ProfileId, OutError) ||
			!ReadRequiredString(Root, TEXT("profile_sha256"), OutRequest.ProfileSha256, OutError) ||
			!ReadRequiredString(Root, TEXT("measurement_level"), OutRequest.MeasurementLevel, OutError) ||
			!ReadRequiredString(Root, TEXT("puerts_script_sha256"), OutRequest.PuertsScriptSha256, OutError) ||
			!ReadRequiredString(Root, TEXT("puerts_runtime_sha256"), OutRequest.PuertsRuntimeSha256, OutError) ||
			!ReadRequiredString(Root, TEXT("avidscript_wasm_path"), OutRequest.AvidScriptWasmPath, OutError) ||
			!ReadRequiredString(Root, TEXT("avidscript_wasm_sha256"), OutRequest.AvidScriptWasmSha256, OutError) ||
			!ReadRequiredString(Root, TEXT("avidscript_compiler_wasm_path"), OutRequest.AvidScriptCompilerWasmPath, OutError) ||
			!ReadRequiredString(Root, TEXT("avidscript_compiler_wasm_sha256"), OutRequest.AvidScriptCompilerWasmSha256, OutError) ||
			!ReadRequiredString(Root, TEXT("avidscript_compiler_source_sha256"), OutRequest.AvidScriptCompilerSourceSha256, OutError) ||
			!ReadRequiredString(Root, TEXT("avidscript_compiler_reference_sha256"), OutRequest.AvidScriptCompilerReferenceSha256, OutError) ||
			!ReadRequiredString(Root, TEXT("avidscript_compiler_guest_ir_sha256"), OutRequest.AvidScriptCompilerGuestIrSha256, OutError) ||
			!ReadRequiredString(Root, TEXT("avidscript_compiler_inspection_sha256"), OutRequest.AvidScriptCompilerInspectionSha256, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Phase57Array request contract mismatch");
			}
			return false;
		}
		if ((OutRequest.MeasurementLevel != TEXT("diagnostic") &&
			 OutRequest.MeasurementLevel != TEXT("formal")) ||
			!Root->TryGetNumberField(TEXT("process_run"), OutRequest.ProcessRun) ||
			!Root->TryGetNumberField(TEXT("warmup_samples"), OutRequest.WarmupSamples) ||
			!Root->TryGetNumberField(TEXT("timed_samples"), OutRequest.TimedSamples) ||
			!Root->TryGetNumberField(TEXT("seed"), OutRequest.Seed) ||
			OutRequest.ProcessRun < 1 || OutRequest.WarmupSamples < 1 ||
			OutRequest.TimedSamples < 3)
		{
			OutError = TEXT("Phase57Array request numeric fields are invalid");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Sizes = nullptr;
		if (!Root->TryGetArrayField(TEXT("sizes"), Sizes) ||
			Sizes->Num() != UE_ARRAY_COUNT(RequiredSizes))
		{
			OutError = TEXT("Phase57Array request must use six frozen sizes");
			return false;
		}
		for (int32 Index = 0; Index < Sizes->Num(); ++Index)
		{
			if (static_cast<int32>((*Sizes)[Index]->AsNumber()) != RequiredSizes[Index])
			{
				OutError = TEXT("Phase57Array request size order differs from the frozen headline");
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* Calls = nullptr;
		if (!Root->TryGetObjectField(TEXT("logical_calls_by_size"), Calls) ||
			Calls == nullptr || !Calls->IsValid())
		{
			OutError = TEXT("Phase57Array request is missing logical_calls_by_size");
			return false;
		}
		for (const int32 Size : RequiredSizes)
		{
			int32 LogicalCalls = 0;
			if (!(*Calls)->TryGetNumberField(FString::FromInt(Size), LogicalCalls) || LogicalCalls < 1)
			{
				OutError = FString::Printf(TEXT("Phase57Array logical call count is invalid for N=%d"), Size);
				return false;
			}
			OutRequest.LogicalCallsBySize.Add(Size, LogicalCalls);
		}

		const TArray<TSharedPtr<FJsonValue>>* LaneOrder = nullptr;
		if (!Root->TryGetArrayField(TEXT("lane_order"), LaneOrder) || LaneOrder->Num() != 4)
		{
			OutError = TEXT("Phase57Array request must contain four lanes");
			return false;
		}
		TSet<FString> SeenLanes;
		for (const TSharedPtr<FJsonValue>& LaneValue : *LaneOrder)
		{
			const FString Lane = LaneValue->AsString();
			if ((Lane != PuertsLane && Lane != ElementLane && Lane != BulkLane &&
				 Lane != CompilerRegionLane) || SeenLanes.Contains(Lane))
			{
				OutError = TEXT("Phase57Array lane_order is invalid");
				return false;
			}
			SeenLanes.Add(Lane);
			OutRequest.LaneOrder.Add(Lane);
		}
		return true;
	}

	class FArrayModuleLoader final : public puerts::IJSModuleLoader
	{
	public:
		FArrayModuleLoader(FString InWorkloadRoot, FString InRuntimeRoot)
			: WorkloadRoot(FPaths::ConvertRelativePathToFull(InWorkloadRoot))
			, RuntimeRoot(FPaths::ConvertRelativePathToFull(InRuntimeRoot))
		{
			FPaths::NormalizeDirectoryName(WorkloadRoot);
			FPaths::NormalizeDirectoryName(RuntimeRoot);
		}

		virtual bool Search(
			const FString& RequiredDir,
			const FString& RequiredModule,
			FString& Path,
			FString& AbsolutePath) override
		{
			TArray<FString> Candidates;
			if (!RequiredDir.IsEmpty())
			{
				Candidates.Add(FPaths::Combine(RequiredDir, RequiredModule));
			}
			Candidates.Add(FPaths::Combine(WorkloadRoot, RequiredModule));
			Candidates.Add(FPaths::Combine(RuntimeRoot, RequiredModule));
			for (FString Candidate : Candidates)
			{
				if (FPaths::GetExtension(Candidate).IsEmpty())
				{
					Candidate += TEXT(".js");
				}
				Candidate = FPaths::ConvertRelativePathToFull(Candidate);
				FPaths::NormalizeFilename(Candidate);
				const bool bAllowed = Candidate.StartsWith(WorkloadRoot + TEXT("/"), ESearchCase::IgnoreCase) ||
					Candidate.StartsWith(RuntimeRoot + TEXT("/"), ESearchCase::IgnoreCase);
				if (bAllowed && FPaths::FileExists(Candidate))
				{
					Path = Candidate;
					AbsolutePath = Candidate;
					return true;
				}
			}
			return false;
		}

		virtual bool Load(const FString& Path, TArray<uint8>& Content) override
		{
			return FFileHelper::LoadFileToArray(Content, *Path);
		}

		virtual FString& GetScriptRoot() override { return WorkloadRoot; }

	private:
		FString WorkloadRoot;
		FString RuntimeRoot;
	};

	struct FSample
	{
		FString Lane;
		int32 Size = 0;
		int32 SampleIndex = 0;
		int32 LogicalCalls = 0;
		uint64 Elements = 0;
		uint64 Bytes = 0;
		uint64 HostTransferBytes = 0;
		uint64 HostCrossings = 0;
		double ElapsedNs = 0.0;
		FString FullHash;
	};

	bool PublishValues(
		FAvidScriptWasmRuntimeInstance& Runtime,
		const TArray<int32>& Values,
		uint32& OutToken,
		FString& OutError)
	{
		FAvidScriptArrayValueReservation Reservation;
		FAvidScriptArrayValueHeap& Heap = Runtime.GetArrayValueHeapForTesting();
		if (!Heap.Reserve(Reservation, OutError))
		{
			return false;
		}
		if (!Heap.PublishReserved(
				Reservation,
				TEXT("System.Int32"),
				Values.Num(),
				sizeof(int32),
				alignof(int32),
				MakeArrayView(
					reinterpret_cast<const uint8*>(Values.GetData()),
					Values.Num() * sizeof(int32)),
				OutToken,
				OutError))
		{
			Heap.ReleaseReservation(Reservation);
			return false;
		}
		return true;
	}

	bool RunAvidScriptSample(
		FAvidScriptWasmRuntimeInstance& Runtime,
		const FString& Lane,
		const int32 Size,
		const int32 LogicalCalls,
		const int32 Seed,
		const int32 SampleIndex,
		FSample& OutSample,
		FString& OutError)
	{
		const TArray<int32> InitialValues = MakeExpectedValues(Size, 0, Seed);
		const TArray<int32> ExpectedValues = MakeExpectedValues(Size, LogicalCalls, Seed);
		const uint32 ExpectedHash = HashValues(ExpectedValues);
		uint32 Token = 0;
		if (!PublishValues(Runtime, InitialValues, Token, OutError))
		{
			return false;
		}
		auto ReleaseToken = [&Runtime, &Token]()
		{
			if (Token != 0)
			{
				Runtime.HandleValueReleaseImport(static_cast<int32>(Token));
				Token = 0;
			}
		};

		if (Lane != CompilerRegionLane)
		{
			const int32 Header[] = { static_cast<int32>(Token), LogicalCalls };
			if (!Runtime.WriteStateBytes(
					ArrayTokenAddress,
					MakeArrayView(
						reinterpret_cast<const uint8*>(Header),
						sizeof(Header)),
					OutError))
			{
				ReleaseToken();
				return false;
			}
		}

		FAvidScriptWasmSmokeResult Failure;
		const int32 HostImportsBefore = Runtime.GetHostImportCallCountForTesting();
		const double StartSeconds = FPlatformTime::Seconds();
		const bool bDispatched = Lane == CompilerRegionLane
			? Runtime.InvokeI32PairExportHotForTesting(
				TEXT("phase57_array_run"),
				static_cast<int32>(Token),
				LogicalCalls,
				Failure)
			: Runtime.DispatchEventHot(
				Lane == ElementLane ? 0 : 1,
				static_cast<float>(Size),
				Failure);
		const double ElapsedNs = (FPlatformTime::Seconds() - StartSeconds) * 1.0e9;
		const int32 ObservedHostImports =
			Runtime.GetHostImportCallCountForTesting() - HostImportsBefore;
		if (!bDispatched)
		{
			OutError = FString::Printf(
				TEXT("Phase57Array AvidScript guest dispatch failed: %s"),
				*Failure.ErrorMessage);
			ReleaseToken();
			return false;
		}

		uint32 GuestHash = 0;
		if (Lane != CompilerRegionLane && !Runtime.ReadStateBytes(
			FullHashAddress,
			MakeArrayView(reinterpret_cast<uint8*>(&GuestHash), sizeof(GuestHash)),
			OutError))
		{
			ReleaseToken();
			return false;
		}
		TArray<int32> ActualValues;
		ActualValues.SetNumUninitialized(Size);
		if (!Runtime.GetArrayValueHeapForTesting().ReadRange(
				Token,
				0,
				Size,
				MakeArrayView(
					reinterpret_cast<uint8*>(ActualValues.GetData()),
					ActualValues.Num() * sizeof(int32)),
				OutError))
		{
			ReleaseToken();
			return false;
		}
		if (Lane == CompilerRegionLane)
		{
			GuestHash = HashValues(ActualValues);
		}
		ReleaseToken();
		if (ActualValues != ExpectedValues || GuestHash != ExpectedHash)
		{
			OutError = FString::Printf(
				TEXT("Phase57Array correctness mismatch lane=%s N=%d expected=%s actual=%s"),
				*Lane,
				Size,
				*FormatHash(ExpectedHash),
				*FormatHash(GuestHash));
			return false;
		}

		OutSample.Lane = Lane;
		OutSample.Size = Size;
		OutSample.SampleIndex = SampleIndex;
		OutSample.LogicalCalls = LogicalCalls;
		OutSample.Elements = static_cast<uint64>(LogicalCalls) * Size * 2u;
		OutSample.Bytes = OutSample.Elements * ArrayElementBytes;
		OutSample.HostTransferBytes = Lane == CompilerRegionLane
			? static_cast<uint64>(Size) * 2u * ArrayElementBytes
			: OutSample.Bytes;
		const uint64 ExpectedHostCrossings = Lane == ElementLane
			? static_cast<uint64>(LogicalCalls) * Size * 2u
			: Lane == CompilerRegionLane ? 4u : static_cast<uint64>(LogicalCalls) * 2u;
		if (ObservedHostImports < 0 ||
			static_cast<uint64>(ObservedHostImports) != ExpectedHostCrossings)
		{
			OutError = FString::Printf(
				TEXT("Phase57Array host crossing mismatch lane=%s N=%d expected=%llu actual=%d"),
				*Lane,
				Size,
				ExpectedHostCrossings,
				ObservedHostImports);
			return false;
		}
		OutSample.HostCrossings = ExpectedHostCrossings;
		OutSample.ElapsedNs = FMath::Max(ElapsedNs, 1.0);
		OutSample.FullHash = FormatHash(GuestHash);
		return true;
	}

	bool WriteProcessResult(
		const FString& ResultPath,
		const FArrayRequest& Request,
		const TArray<FSample>& Samples,
		FString& OutError)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("contract"), TEXT("phase57_array_process_result.v2"));
		Root->SetStringField(TEXT("profile_id"), Request.ProfileId);
		Root->SetStringField(TEXT("profile_sha256"), Request.ProfileSha256);
		Root->SetStringField(TEXT("measurement_level"), Request.MeasurementLevel);
		Root->SetNumberField(TEXT("process_run"), Request.ProcessRun);
		TArray<TSharedPtr<FJsonValue>> Sizes;
		for (const int32 Size : RequiredSizes)
		{
			Sizes.Add(MakeShared<FJsonValueNumber>(Size));
		}
		Root->SetArrayField(TEXT("sizes"), MoveTemp(Sizes));

		TArray<TSharedPtr<FJsonValue>> JsonSamples;
		JsonSamples.Reserve(Samples.Num());
		for (const FSample& Sample : Samples)
		{
			TSharedRef<FJsonObject> JsonSample = MakeShared<FJsonObject>();
			JsonSample->SetStringField(TEXT("lane"), Sample.Lane);
			JsonSample->SetNumberField(TEXT("size"), Sample.Size);
			JsonSample->SetNumberField(TEXT("sample_index"), Sample.SampleIndex);
			JsonSample->SetNumberField(TEXT("logical_calls"), Sample.LogicalCalls);
			JsonSample->SetNumberField(TEXT("elements"), static_cast<double>(Sample.Elements));
			JsonSample->SetNumberField(TEXT("bytes"), static_cast<double>(Sample.Bytes));
			JsonSample->SetNumberField(
				TEXT("host_transfer_bytes"),
				static_cast<double>(Sample.HostTransferBytes));
			JsonSample->SetNumberField(TEXT("host_crossings"), static_cast<double>(Sample.HostCrossings));
			JsonSample->SetNumberField(TEXT("elapsed_ns"), Sample.ElapsedNs);
			JsonSample->SetNumberField(
				TEXT("ns_per_logical_call"),
				Sample.ElapsedNs / Sample.LogicalCalls);
			JsonSample->SetNumberField(
				TEXT("ns_per_element"),
				Sample.ElapsedNs / Sample.Elements);
			JsonSample->SetStringField(TEXT("full_hash"), Sample.FullHash);
			JsonSamples.Add(MakeShared<FJsonValueObject>(JsonSample));
		}
		Root->SetArrayField(TEXT("samples"), MoveTemp(JsonSamples));

		TSharedRef<FJsonObject> Provenance = MakeShared<FJsonObject>();
		Provenance->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Provenance->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());
		Provenance->SetStringField(TEXT("puerts_script_sha256"), Request.PuertsScriptSha256);
		Provenance->SetStringField(TEXT("puerts_runtime_sha256"), Request.PuertsRuntimeSha256);
		Provenance->SetStringField(TEXT("avidscript_wasm_sha256"), Request.AvidScriptWasmSha256);
		Provenance->SetStringField(TEXT("avidscript_compiler_wasm_sha256"), Request.AvidScriptCompilerWasmSha256);
		Provenance->SetStringField(TEXT("avidscript_compiler_source_sha256"), Request.AvidScriptCompilerSourceSha256);
		Provenance->SetStringField(TEXT("avidscript_compiler_reference_sha256"), Request.AvidScriptCompilerReferenceSha256);
		Provenance->SetStringField(TEXT("avidscript_compiler_guest_ir_sha256"), Request.AvidScriptCompilerGuestIrSha256);
		Provenance->SetStringField(TEXT("avidscript_compiler_inspection_sha256"), Request.AvidScriptCompilerInspectionSha256);
		Provenance->SetStringField(TEXT("avidscript_backend"), TEXT("wasmtime"));
		Provenance->SetStringField(TEXT("avidscript_execution_mode"), TEXT("jit"));
		Root->SetObjectField(TEXT("provenance"), Provenance);

		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			OutError = TEXT("Phase57Array result serialization failed");
			return false;
		}
		const FString TempPath = ResultPath + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Text, *TempPath) ||
			!IFileManager::Get().Move(*ResultPath, *TempPath, true, true))
		{
			OutError = FString::Printf(TEXT("Phase57Array result publish failed: %s"), *ResultPath);
			return false;
		}
		return true;
	}
}

bool FAvidScriptPerfArrayRunner::RunFromFiles(
	const FString& RequestPath,
	const FString& ResultPath,
	FString& OutError)
{
#if !WITH_DEV_AUTOMATION_TESTS
	OutError = TEXT("Phase57Array runner requires WITH_DEV_AUTOMATION_TESTS for array capability setup");
	return false;
#else
	FArrayRequest Request;
	if (!ParseRequest(RequestPath, Request, OutError))
	{
		return false;
	}

	const TSharedPtr<IPlugin> HarnessPlugin =
		IPluginManager::Get().FindPlugin(TEXT("AvidScriptPerfHarness"));
	const TSharedPtr<IPlugin> PuertsPlugin =
		IPluginManager::Get().FindPlugin(TEXT("Puerts"));
	if (!HarnessPlugin.IsValid() || !PuertsPlugin.IsValid())
	{
		OutError = TEXT("Phase57Array requires mounted AvidScriptPerfHarness and Puerts plugins");
		return false;
	}
	const FString ScriptRoot = FPaths::Combine(HarnessPlugin->GetContentDir(), TEXT("JavaScript"));
	const FString ScriptPath = FPaths::Combine(ScriptRoot, TEXT("phase57_array_reflection.js"));
	const FString PuertsRuntimePath = FModuleManager::Get().GetModuleFilename(TEXT("JsEnv"));
	FString ActualScriptSha256;
	FString ActualPuertsRuntimeSha256;
	FString ActualWasmSha256;
	FString ActualCompilerWasmSha256;
	if (!GetPhase57ArrayFileSha256(ScriptPath, ActualScriptSha256, OutError) ||
		!GetPhase57ArrayFileSha256(PuertsRuntimePath, ActualPuertsRuntimeSha256, OutError) ||
		!GetPhase57ArrayFileSha256(Request.AvidScriptWasmPath, ActualWasmSha256, OutError) ||
		!GetPhase57ArrayFileSha256(Request.AvidScriptCompilerWasmPath, ActualCompilerWasmSha256, OutError) ||
		ActualScriptSha256 != Request.PuertsScriptSha256 ||
		ActualPuertsRuntimeSha256 != Request.PuertsRuntimeSha256 ||
		ActualWasmSha256 != Request.AvidScriptWasmSha256 ||
		ActualCompilerWasmSha256 != Request.AvidScriptCompilerWasmSha256)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Phase57Array provenance digest mismatch");
		}
		return false;
	}

	UWorld* World = nullptr;
	if (GEngine == nullptr ||
		(World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptPhase57ArrayWorld"))) == nullptr)
	{
		OutError = TEXT("Phase57Array could not create its isolated benchmark world");
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	auto DestroyWorld = [&World]()
	{
		if (World != nullptr)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			World = nullptr;
		}
	};
	AAvidScriptPerfFixture* Fixture = World->SpawnActor<AAvidScriptPerfFixture>();
	if (Fixture == nullptr)
	{
		OutError = TEXT("Phase57Array fixture spawn failed");
		DestroyWorld();
		return false;
	}

	const FString RuntimeRoot = FPaths::Combine(PuertsPlugin->GetContentDir(), TEXT("JavaScript"));
	TSharedPtr<puerts::FJsEnv> PuertsEnvironment = MakeShared<puerts::FJsEnv>(
		std::make_shared<FArrayModuleLoader>(ScriptRoot, RuntimeRoot),
		std::make_shared<puerts::FDefaultLogger>(),
		-1);
	PuertsEnvironment->Start(
		TEXT("phase57_array_reflection.js"),
		{ TPair<FString, UObject*>(TEXT("Fixture"), Fixture) });
	if (!Fixture->HasPuertsArrayCallbacks())
	{
		OutError = TEXT("Phase57Array Puerts module did not register TArray callbacks");
		PuertsEnvironment.Reset();
		DestroyWorld();
		return false;
	}

	TArray<uint8> WasmBytes;
	TArray<uint8> CompilerWasmBytes;
	if (!FFileHelper::LoadFileToArray(WasmBytes, *Request.AvidScriptWasmPath))
	{
		OutError = TEXT("Phase57Array Wasm kernel could not be loaded");
		PuertsEnvironment.Reset();
		DestroyWorld();
		return false;
	}
	if (!FFileHelper::LoadFileToArray(CompilerWasmBytes, *Request.AvidScriptCompilerWasmPath))
	{
		OutError = TEXT("Phase57Array compiler-managed Wasm could not be loaded");
		PuertsEnvironment.Reset();
		DestroyWorld();
		return false;
	}
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Selection.bAllowFallback = false;
	FAvidScriptWasmRuntimeInstance Runtime(Selection);
	FAvidScriptWasmRuntimeInstance CompilerRuntime(Selection);
	FAvidScriptWasmSmokeResult RuntimeResult;
	if (!Runtime.LoadModule(
			WasmBytes.GetData(),
			WasmBytes.Num(),
			TEXT("phase57_array_kernel"),
			RuntimeResult) ||
		!Runtime.BeginPlay(RuntimeResult) ||
		!CompilerRuntime.LoadModule(
			CompilerWasmBytes.GetData(),
			CompilerWasmBytes.Num(),
			TEXT("phase57_array_compiler_region"),
			RuntimeResult) ||
		!CompilerRuntime.BeginPlay(RuntimeResult))
	{
		OutError = FString::Printf(
			TEXT("Phase57Array AvidScript Wasmtime initialization failed: %s"),
			*RuntimeResult.ErrorMessage);
		PuertsEnvironment.Reset();
		DestroyWorld();
		return false;
	}

	TArray<FSample> Samples;
	for (const FString& Lane : Request.LaneOrder)
	{
		for (const int32 Size : RequiredSizes)
		{
			const int32 LogicalCalls = Request.LogicalCallsBySize.FindChecked(Size);
			for (int32 SampleIndex = -Request.WarmupSamples;
				 SampleIndex < Request.TimedSamples;
				 ++SampleIndex)
			{
				FSample Sample;
				if (Lane == PuertsLane)
				{
					const double StartSeconds = FPlatformTime::Seconds();
					Fixture->RunPuertsArrayWorkload(Size, LogicalCalls, Request.Seed);
					const double ElapsedNs = (FPlatformTime::Seconds() - StartSeconds) * 1.0e9;
					const FString FullHash = Fixture->GetPuertsArrayFullHash();
					const FString ExpectedHash = FormatHash(HashValues(
						MakeExpectedValues(Size, LogicalCalls, Request.Seed)));
					if (FullHash != ExpectedHash)
					{
						OutError = FString::Printf(
							TEXT("Phase57Array Puerts correctness mismatch N=%d expected=%s actual=%s"),
							Size,
							*ExpectedHash,
							*FullHash);
						Runtime.Unload();
						CompilerRuntime.Unload();
						PuertsEnvironment.Reset();
						DestroyWorld();
						return false;
					}
					Sample.Lane = Lane;
					Sample.Size = Size;
					Sample.SampleIndex = SampleIndex;
					Sample.LogicalCalls = LogicalCalls;
					Sample.Elements = static_cast<uint64>(LogicalCalls) * Size * 2u;
					Sample.Bytes = Sample.Elements * ArrayElementBytes;
					Sample.HostTransferBytes = Sample.Bytes;
					Sample.HostCrossings = LogicalCalls;
					Sample.ElapsedNs = FMath::Max(ElapsedNs, 1.0);
					Sample.FullHash = FullHash;
				}
				else if (!RunAvidScriptSample(
					Lane == CompilerRegionLane ? CompilerRuntime : Runtime,
					Lane,
					Size,
					LogicalCalls,
					Request.Seed,
					SampleIndex,
					Sample,
					OutError))
				{
					Runtime.Unload();
					CompilerRuntime.Unload();
					PuertsEnvironment.Reset();
					DestroyWorld();
					return false;
				}
				if (SampleIndex >= 0)
				{
					Samples.Add(MoveTemp(Sample));
				}
			}
		}
	}

	FAvidScriptWasmSmokeResult EndResult;
	Runtime.EndPlay(EndResult);
	CompilerRuntime.EndPlay(EndResult);
	Runtime.Unload();
	CompilerRuntime.Unload();
	PuertsEnvironment.Reset();
	DestroyWorld();
	return WriteProcessResult(ResultPath, Request, Samples, OutError);
#endif
}
