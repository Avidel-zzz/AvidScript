#include "AvidScriptPerfCostRunner.h"

#include "AvidScriptVmBackend.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
	constexpr int32 CostSchemaVersion = 2;
	constexpr int32 EmptyOrdinal = 0;
	constexpr int32 PairOrdinal = 1;

	struct FCostRequest
	{
		FString AttemptId;
		FString ProfileSha256;
		FString CandidateCommit;
		FString CandidateTreeSha;
		FString EngineExecutableSha256;
		FString KernelPath;
		FString KernelSha256;
		int32 ProcessRun = 0;
		int32 WarmupSamples = 0;
		int32 TimedSamples = 0;
		int32 Iterations = 0;
		int32 Seed = 0;
	};

	bool GetFileSha256(
		const FString& Path,
		FString& OutSha256,
		FString& OutError)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			OutError = FString::Printf(TEXT("unable to read identity artifact: %s"), *Path);
			return false;
		}
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		if (SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest) == nullptr)
		{
			OutError = FString::Printf(TEXT("unable to hash identity artifact: %s"), *Path);
			return false;
		}
		OutSha256.Reset(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			OutSha256 += FString::Printf(TEXT("%02x"), Byte);
		}
		return true;
	}

	bool ReadRequest(
		const FString& Path,
		FCostRequest& OutRequest,
		FString& OutRequestSha256,
		TArray<uint8>& OutKernelBytes,
		FString& OutError)
	{
		FString Text;
		TSharedPtr<FJsonObject> Json;
		if (!FFileHelper::LoadFileToString(Text, *Path)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json)
			|| !Json.IsValid())
		{
			OutError = TEXT("cost request is not a readable JSON object");
			return false;
		}
		if (!GetFileSha256(Path, OutRequestSha256, OutError))
		{
			return false;
		}

		auto ReadString = [&Json, &OutError](const TCHAR* Name, FString& OutValue)
		{
			if (!Json->TryGetStringField(Name, OutValue) || OutValue.IsEmpty())
			{
				OutError = FString::Printf(TEXT("required cost request string is missing: %s"), Name);
				return false;
			}
			return true;
		};
		auto ReadInt = [&Json, &OutError](const TCHAR* Name, int32& OutValue)
		{
			double Number = 0.0;
			if (!Json->TryGetNumberField(Name, Number)
				|| !FMath::IsFinite(Number)
				|| FMath::FloorToDouble(Number) != Number
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				OutError = FString::Printf(TEXT("required cost request integer is invalid: %s"), Name);
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		};

		double SchemaVersion = 0.0;
		bool CandidateClean = false;
		if (!Json->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
			|| SchemaVersion != CostSchemaVersion
			|| !Json->TryGetBoolField(TEXT("candidate_clean"), CandidateClean)
			|| !CandidateClean
			|| !ReadString(TEXT("attempt_id"), OutRequest.AttemptId)
			|| !ReadString(TEXT("profile_sha256"), OutRequest.ProfileSha256)
			|| !ReadString(TEXT("candidate_commit"), OutRequest.CandidateCommit)
			|| !ReadString(TEXT("candidate_tree_sha"), OutRequest.CandidateTreeSha)
			|| !ReadString(TEXT("engine_executable_sha256"), OutRequest.EngineExecutableSha256)
			|| !ReadString(TEXT("kernel_wasm_path"), OutRequest.KernelPath)
			|| !ReadString(TEXT("kernel_wasm_sha256"), OutRequest.KernelSha256)
			|| !ReadInt(TEXT("process_run"), OutRequest.ProcessRun)
			|| !ReadInt(TEXT("warmup_samples"), OutRequest.WarmupSamples)
			|| !ReadInt(TEXT("timed_samples"), OutRequest.TimedSamples)
			|| !ReadInt(TEXT("iterations"), OutRequest.Iterations)
			|| !ReadInt(TEXT("seed"), OutRequest.Seed))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("cost request schema or clean candidate contract is invalid");
			}
			return false;
		}
		if (OutRequest.ProcessRun < 0
			|| OutRequest.WarmupSamples < 1
			|| OutRequest.WarmupSamples > 100
			|| OutRequest.TimedSamples < 1
			|| OutRequest.TimedSamples > 1000
			|| OutRequest.Iterations < 1
			|| OutRequest.Iterations > 100000000)
		{
			OutError = TEXT("cost request dimensions are outside safe limits");
			return false;
		}

		OutRequest.KernelPath = FPaths::ConvertRelativePathToFull(OutRequest.KernelPath);
		if (!FFileHelper::LoadFileToArray(OutKernelBytes, *OutRequest.KernelPath))
		{
			OutError = TEXT("physical cost kernel could not be read");
			return false;
		}
		FString ActualKernelSha256;
		FString ActualEditorSha256;
		if (!GetFileSha256(OutRequest.KernelPath, ActualKernelSha256, OutError)
			|| !ActualKernelSha256.Equals(
				OutRequest.KernelSha256,
				ESearchCase::CaseSensitive))
		{
			OutError = TEXT("physical cost kernel identity differs from request");
			return false;
		}
		if (!GetFileSha256(
				FPlatformProcess::ExecutablePath(),
				ActualEditorSha256,
				OutError)
			|| !ActualEditorSha256.Equals(
				OutRequest.EngineExecutableSha256,
				ESearchCase::CaseSensitive))
		{
			OutError = TEXT("physical cost Editor identity differs from request");
			return false;
		}
		return true;
	}

	FAvidScriptVmBindingPackage MakeBindingPackage()
	{
		FAvidScriptVmBindingPackage Package;
		Package.PackageName = TEXT("avidscript.phase54.physical_cost");
		Package.PackageHash = FString::ChrN(64, TEXT('c'));
		for (int32 Ordinal = 0; Ordinal < 2; ++Ordinal)
		{
			FAvidScriptVmDynamicImport Import;
			Import.StableId = Ordinal == EmptyOrdinal
				? FString::ChrN(64, TEXT('d'))
				: FString::ChrN(64, TEXT('e'));
			Import.Ordinal = static_cast<uint32>(Ordinal);
			Import.ModuleName = TEXT("avidscript");
			Import.ImportName = Ordinal == EmptyOrdinal
				? TEXT("typed_empty_i32")
				: TEXT("i32_pair");
			Import.Signature = Ordinal == EmptyOrdinal ? TEXT("()i") : TEXT("(ii)i");
			Package.Imports.Add(MoveTemp(Import));
		}
		return Package;
	}

	TArray<FAvidScriptVmTypedHostImport> MakeTypedImports()
	{
		TArray<FAvidScriptVmTypedHostImport> Imports;
		for (int32 Ordinal = 0; Ordinal < 2; ++Ordinal)
		{
			FAvidScriptVmTypedHostImport& Import = Imports.Emplace_GetRef();
			Import.StableId = Ordinal == EmptyOrdinal
				? FString::ChrN(64, TEXT('d'))
				: FString::ChrN(64, TEXT('e'));
			Import.BindingOrdinal = static_cast<uint32>(Ordinal);
			Import.ModuleName = TEXT("avidscript");
			Import.ImportName = Ordinal == EmptyOrdinal
				? TEXT("typed_empty_i32")
				: TEXT("i32_pair");
			Import.Signature = Ordinal == EmptyOrdinal ? TEXT("()i") : TEXT("(ii)i");
			Import.Shape = Ordinal == EmptyOrdinal
				? EAvidScriptVmTypedHostShape::EmptyI32
				: EAvidScriptVmTypedHostShape::I32PairToI32;
		}
		return Imports;
	}

	class FCostTypedDispatcher final : public IAvidScriptVmTypedHostDispatcher
	{
	public:
		EAvidScriptVmTypedHostStatus DispatchEmptyI32(uint32 BindingOrdinal, int32& OutValue) override
		{
			if (BindingOrdinal != EmptyOrdinal)
			{
				return EAvidScriptVmTypedHostStatus::Rejected;
			}
			OutValue = 0;
			return EAvidScriptVmTypedHostStatus::Succeeded;
		}

		EAvidScriptVmTypedHostStatus DispatchI32PairToI32(
			uint32 BindingOrdinal,
			int32 Left,
			int32 Right,
			int32& OutValue) override
		{
			if (BindingOrdinal != PairOrdinal)
			{
				return EAvidScriptVmTypedHostStatus::Rejected;
			}
			OutValue = static_cast<int32>(
				static_cast<uint32>(Left) + static_cast<uint32>(Right));
			return EAvidScriptVmTypedHostStatus::Succeeded;
		}

		EAvidScriptVmTypedHostStatus DispatchSelfI32PairToI32(uint32, int32, int32, int32, int32, int32&) override { return EAvidScriptVmTypedHostStatus::Rejected; }
		EAvidScriptVmTypedHostStatus DispatchSelfPropertyI32GetSet(uint32, int32, int32, int32, int32&) override { return EAvidScriptVmTypedHostStatus::Rejected; }
		EAvidScriptVmTypedHostStatus DispatchSelfVectorValue(uint32, int32, int32, int32, int32&) override { return EAvidScriptVmTypedHostStatus::Rejected; }
		EAvidScriptVmTypedHostStatus DispatchStableObjectRoundtrip(uint32, int32, int32, int32, int32, int32, int32&) override { return EAvidScriptVmTypedHostStatus::Rejected; }
		EAvidScriptVmTypedHostStatus DispatchCommandBufferSubmit(uint32, int32, int32, int32&) override { return EAvidScriptVmTypedHostStatus::Rejected; }
	};

	class FCostGenericDispatcher final : public IAvidScriptHostDispatcher
	{
	public:
		bool DispatchHostCall(const FAvidScriptHostCall&, FAvidScriptHostCallResult& OutResult) override
		{
			OutResult = FAvidScriptHostCallResult();
			return false;
		}

		bool DispatchDynamicHostCall(
			const FAvidScriptDynamicHostCall& Call,
			FAvidScriptDynamicHostCallResult& OutResult) override
		{
			OutResult = FAvidScriptDynamicHostCallResult();
			if (Call.BindingOrdinal == EmptyOrdinal && Call.Arguments.IsEmpty())
			{
				OutResult.bSucceeded = true;
				OutResult.ReturnValue = 0;
				return true;
			}
			if (Call.BindingOrdinal == PairOrdinal && Call.Arguments.Num() == 2)
			{
				OutResult.bSucceeded = true;
				OutResult.ReturnValue = static_cast<int32>(
					static_cast<uint32>(Call.Arguments[0])
					+ static_cast<uint32>(Call.Arguments[1]));
				return true;
			}
			OutResult.Details = TEXT("physical cost dynamic import contract mismatch");
			return false;
		}
	};

	TUniquePtr<IAvidScriptVmBackend> CreateWasmtimeBackend(FAvidScriptVmError& OutError)
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
		Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
		Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
		Selection.bAllowFallback = false;
		return CreateAvidScriptVmBackend(Selection, OutError);
	}

	uint32 RunCachedOracle(const int32 Iterations, const int32 Seed)
	{
		uint32 Value = static_cast<uint32>(Seed);
		for (uint32 Index = 0; Index < static_cast<uint32>(Iterations); ++Index)
		{
			Value += Index;
		}
		return Value;
	}

	uint32 RunPairOracle(const int32 Iterations, const int32 Seed)
	{
		uint32 Value = static_cast<uint32>(Seed);
		for (uint32 Index = 0; Index < static_cast<uint32>(Iterations); ++Index)
		{
			Value += static_cast<uint32>(Seed) + Index;
		}
		return Value;
	}

	bool CallExport(
		IAvidScriptVmBackend& Backend,
		const FAvidScriptVmExportHandle& Handle,
		int32 Iterations,
		int32 Seed,
		uint32& OutValue,
		FString& OutError)
	{
		FAvidScriptVmCallFrame Frame;
		Frame.Cells[0] = static_cast<uint32>(Iterations);
		Frame.Cells[1] = static_cast<uint32>(Seed);
		Frame.CellCount = 2;
		FAvidScriptVmCallResult Result;
		FAvidScriptVmError VmError;
		if (!Backend.Call(Handle, Frame, VmError, &Result) || Result.CellCount != 1)
		{
			OutError = FString::Printf(
				TEXT("Wasmtime cost export call failed: %s | %s"),
				*VmError.Category,
				*VmError.Details);
			return false;
		}
		OutValue = Result.Cells[0];
		return true;
	}

	bool CallExportRepeated(
		IAvidScriptVmBackend& Backend,
		const FAvidScriptVmExportHandle& Handle,
		const FAvidScriptVmPreparedExportCall* PreparedCall,
		const int32 Iterations,
		const int32 Seed,
		uint32& OutValue,
		FString& OutError)
	{
		uint32 Value = static_cast<uint32>(Seed);
		for (uint32 Index = 0;
			Index < static_cast<uint32>(Iterations);
			++Index)
		{
			FAvidScriptVmCallFrame Frame;
			Frame.Cells[0] = 1;
			Frame.Cells[1] = Value;
			Frame.CellCount = 2;
			FAvidScriptVmCallResult Result;
			FAvidScriptVmError VmError;
			const bool bCalled = PreparedCall != nullptr
				? PreparedCall->Call(Frame, VmError, &Result)
				: Backend.Call(Handle, Frame, VmError, &Result);
			if (!bCalled || Result.CellCount != 1)
			{
				OutError = FString::Printf(
					TEXT("Wasmtime repeated export call failed: %s | %s"),
					*VmError.Category,
					*VmError.Details);
				return false;
			}
			Value = Result.Cells[0] + Index;
		}
		OutValue = Value;
		return true;
	}
}

bool FAvidScriptPerfCostRunner::RunFromFiles(
	const FString& RequestPath,
	const FString& ResultPath,
	FString& OutError)
{
	FCostRequest Request;
	FString RequestSha256;
	TArray<uint8> KernelBytes;
	if (!ReadRequest(
			FPaths::ConvertRelativePathToFull(RequestPath),
			Request,
			RequestSha256,
			KernelBytes,
			OutError))
	{
		return false;
	}

	FAvidScriptVmBindingPackage Package = MakeBindingPackage();
	TArray<FAvidScriptVmTypedHostImport> TypedImports = MakeTypedImports();
	FCostTypedDispatcher TypedDispatcher;
	FCostGenericDispatcher GenericDispatcher;
	FAvidScriptVmError VmError;
	TUniquePtr<IAvidScriptVmBackend> TypedBackend = CreateWasmtimeBackend(VmError);
	TUniquePtr<IAvidScriptVmBackend> GenericBackend = CreateWasmtimeBackend(VmError);
	if (!TypedBackend || !GenericBackend)
	{
		OutError = FString::Printf(TEXT("Wasmtime cost backend unavailable: %s"), *VmError.Details);
		return false;
	}

	FAvidScriptVmLoadConfig TypedConfig;
	TypedConfig.BindingPackage = &Package;
	TypedConfig.TypedHostDispatcher = &TypedDispatcher;
	TypedConfig.TypedHostImports = TypedImports;
	FAvidScriptVmLoadConfig GenericConfig;
	GenericConfig.BindingPackage = &Package;
	GenericConfig.HostDispatcher = &GenericDispatcher;
	if (!TypedBackend->Load(KernelBytes, TEXT("phase54.physical_cost.typed"), TypedConfig, VmError)
		|| !GenericBackend->Load(KernelBytes, TEXT("phase54.physical_cost.generic"), GenericConfig, VmError))
	{
		OutError = FString::Printf(
			TEXT("Wasmtime cost module load failed: %s | %s"),
			*VmError.Category,
			*VmError.Details);
		return false;
	}

	FAvidScriptVmExportHandle CachedExport;
	FAvidScriptVmExportHandle TypedEmptyExport;
	FAvidScriptVmExportHandle TypedPairExport;
	FAvidScriptVmExportHandle GenericEmptyExport;
	if (!TypedBackend->ResolveExport(TEXT("run_cached"), CachedExport, VmError)
		|| !TypedBackend->ResolveExport(TEXT("run_empty"), TypedEmptyExport, VmError)
		|| !TypedBackend->ResolveExport(TEXT("run_i32_pair"), TypedPairExport, VmError)
		|| !GenericBackend->ResolveExport(TEXT("run_empty"), GenericEmptyExport, VmError))
	{
		OutError = FString::Printf(TEXT("Wasmtime cost export resolve failed: %s"), *VmError.Details);
		return false;
	}
	FAvidScriptVmPreparedExportCall PreparedCachedExport;
	if (!TypedBackend->PrepareExportCall(
			CachedExport,
			PreparedCachedExport,
			VmError)
		|| !PreparedCachedExport.IsValid())
	{
		OutError = FString::Printf(
			TEXT("Wasmtime cost prepared export failed: %s | %s"),
			*VmError.Category,
			*VmError.Details);
		return false;
	}

	auto RunStage = [&](const FString& Stage, const int32 Seed, uint32& OutValue)
	{
		if (Stage == TEXT("native_no_op"))
		{
			OutValue = RunCachedOracle(Request.Iterations, Seed);
			return true;
		}
		if (Stage == TEXT("guest_loop_baseline"))
		{
			return CallExport(*TypedBackend, CachedExport, Request.Iterations, Seed, OutValue, OutError);
		}
		if (Stage == TEXT("generic_export"))
		{
			return CallExportRepeated(
				*TypedBackend,
				CachedExport,
				nullptr,
				Request.Iterations,
				Seed,
				OutValue,
				OutError);
		}
		if (Stage == TEXT("prepared_export"))
		{
			return CallExportRepeated(
				*TypedBackend,
				CachedExport,
				&PreparedCachedExport,
				Request.Iterations,
				Seed,
				OutValue,
				OutError);
		}
		if (Stage == TEXT("typed_empty_import"))
		{
			return CallExport(*TypedBackend, TypedEmptyExport, Request.Iterations, Seed, OutValue, OutError);
		}
		if (Stage == TEXT("generic_empty_import"))
		{
			return CallExport(*GenericBackend, GenericEmptyExport, Request.Iterations, Seed, OutValue, OutError);
		}
		if (Stage == TEXT("typed_i32_pair_import"))
		{
			return CallExport(*TypedBackend, TypedPairExport, Request.Iterations, Seed, OutValue, OutError);
		}
		OutError = FString::Printf(TEXT("unsupported physical cost stage: %s"), *Stage);
		return false;
	};

	const TArray<FString> Stages = {
		TEXT("native_no_op"),
		TEXT("guest_loop_baseline"),
		TEXT("generic_export"),
		TEXT("prepared_export"),
		TEXT("typed_empty_import"),
		TEXT("generic_empty_import"),
		TEXT("typed_i32_pair_import")
	};
	for (int32 Warmup = 0; Warmup < Request.WarmupSamples; ++Warmup)
	{
		for (const FString& Stage : Stages)
		{
			uint32 Ignored = 0;
			if (!RunStage(Stage, Request.Seed + Warmup * 17, Ignored))
			{
				return false;
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> Samples;
	for (int32 SampleIndex = 0; SampleIndex < Request.TimedSamples; ++SampleIndex)
	{
		const int32 Seed = Request.Seed + Request.ProcessRun * 1009 + SampleIndex * 17;
		for (int32 StageOffset = 0; StageOffset < Stages.Num(); ++StageOffset)
		{
			const int32 StageIndex = (SampleIndex + Request.ProcessRun + StageOffset) % Stages.Num();
			const FString& Stage = Stages[StageIndex];
			uint32 Value = 0;
			const uint64 BeginCycles = FPlatformTime::Cycles64();
			if (!RunStage(Stage, Seed, Value))
			{
				return false;
			}
			const uint64 ElapsedCycles = FPlatformTime::Cycles64() - BeginCycles;
			const uint32 Expected = Stage == TEXT("typed_i32_pair_import")
				? RunPairOracle(Request.Iterations, Seed)
				: RunCachedOracle(Request.Iterations, Seed);
			if (Value != Expected)
			{
				OutError = FString::Printf(TEXT("physical cost checksum mismatch: %s"), *Stage);
				return false;
			}
			const double DurationNs =
				static_cast<double>(ElapsedCycles)
				* FPlatformTime::GetSecondsPerCycle64()
				* 1.0e9;
			TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
			Sample->SetStringField(TEXT("stage"), Stage);
			Sample->SetNumberField(TEXT("sample_index"), SampleIndex);
			Sample->SetNumberField(TEXT("stage_position"), StageOffset);
			Sample->SetNumberField(TEXT("iterations"), Request.Iterations);
			Sample->SetNumberField(TEXT("seed"), Seed);
			Sample->SetNumberField(TEXT("duration_ns"), DurationNs);
			Sample->SetNumberField(
				TEXT("ns_per_iteration"),
				DurationNs / static_cast<double>(Request.Iterations));
			Sample->SetNumberField(TEXT("checksum"), Value);
			Sample->SetNumberField(TEXT("expected_checksum"), Expected);
			Sample->SetBoolField(TEXT("correct"), true);
			Sample->SetNumberField(
				TEXT("host_import_count"),
				Stage.Contains(TEXT("import"))
					? Request.Iterations
					: 0);
			Sample->SetNumberField(
				TEXT("export_call_count"),
				Stage == TEXT("generic_export")
					|| Stage == TEXT("prepared_export")
					? Request.Iterations
					: Stage == TEXT("native_no_op")
						? 0
						: 1);
			Samples.Add(MakeShared<FJsonValueObject>(Sample));
		}
	}

	const FAvidScriptVmBackendInfo& BackendInfo = TypedBackend->GetBackendInfo();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("schema_version"), CostSchemaVersion);
	Result->SetStringField(TEXT("benchmark_kind"), TEXT("physical_crossing_cost_ladder"));
	Result->SetStringField(TEXT("attempt_id"), Request.AttemptId);
	Result->SetStringField(TEXT("request_sha256"), RequestSha256);
	Result->SetStringField(TEXT("profile_sha256"), Request.ProfileSha256);
	Result->SetStringField(TEXT("candidate_commit"), Request.CandidateCommit);
	Result->SetStringField(TEXT("candidate_tree_sha"), Request.CandidateTreeSha);
	Result->SetBoolField(TEXT("candidate_clean"), true);
	Result->SetStringField(TEXT("engine_executable_sha256"), Request.EngineExecutableSha256);
	Result->SetNumberField(TEXT("process_run"), Request.ProcessRun);
	Result->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Result->SetStringField(TEXT("kernel_wasm_sha256"), Request.KernelSha256);
	Result->SetStringField(TEXT("runtime_id"), BackendInfo.StableBackendId);
	Result->SetStringField(TEXT("runtime_version"), BackendInfo.RuntimeVersion);
	Result->SetStringField(TEXT("runtime_build_identity"), BackendInfo.RuntimeBuildIdentity);
	Result->SetStringField(TEXT("runtime_artifact_sha256"), BackendInfo.RuntimeArtifactSha256);
	Result->SetBoolField(TEXT("fallback_used"), false);
	Result->SetNumberField(TEXT("correctness_failures"), 0);
	Result->SetArrayField(TEXT("samples"), Samples);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Result, Writer)
		|| !FFileHelper::SaveStringToFile(
			Json,
			*FPaths::ConvertRelativePathToFull(ResultPath),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("physical cost result could not be published");
		return false;
	}
	GenericBackend->Unload();
	TypedBackend->Unload();
	return true;
}
