#include "AvidScriptWasmtimeApi.h"

#include <wasmtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using FClock = std::chrono::steady_clock;

struct FOptions
{
    std::filesystem::path KernelPath;
    int Samples = 30;
    int WarmupSamples = 5;
    double MinimumSampleMilliseconds = 5.0;
    int32_t Seed = 1397313;
    uint32_t MinimumIterations = 1000;
    uint32_t MaximumIterations = 100000000;
};

struct FProfileDefinition
{
    const char* Id;
    bool bEpochInterruption;
    bool bSpectreMitigation;
};

struct FProfileResult
{
    FProfileDefinition Definition;
    std::vector<double> NanosecondsPerIteration;
    int32_t Checksum = 0;
};

std::string DescribeFailure(AvidScriptWasmtimeFailure* Failure)
{
    if (Failure == nullptr)
    {
        return "unknown Wasmtime failure";
    }

    size_t MessageSize = 0;
    const char* Message = avidscript_wasmtime_failure_message(
        Failure,
        &MessageSize);
    const std::string Result = Message != nullptr
        ? std::string(Message, MessageSize)
        : "Wasmtime failure without a message";
    avidscript_wasmtime_failure_delete(Failure);
    return Result;
}

std::string EscapeJson(const std::string& Value)
{
    std::ostringstream Result;
    for (const unsigned char Character : Value)
    {
        switch (Character)
        {
        case '\\': Result << "\\\\"; break;
        case '"': Result << "\\\""; break;
        case '\b': Result << "\\b"; break;
        case '\f': Result << "\\f"; break;
        case '\n': Result << "\\n"; break;
        case '\r': Result << "\\r"; break;
        case '\t': Result << "\\t"; break;
        default:
            if (Character < 0x20)
            {
                Result << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(Character)
                    << std::dec << std::setfill(' ');
            }
            else
            {
                Result << Character;
            }
        }
    }
    return Result.str();
}

uint32_t ParseUInt32(const char* Value, const char* Name)
{
    char* End = nullptr;
    const unsigned long long Parsed = std::strtoull(Value, &End, 10);
    if (End == Value || *End != '\0'
        || Parsed > std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error(std::string("invalid ") + Name);
    }
    return static_cast<uint32_t>(Parsed);
}

int ParsePositiveInt(const char* Value, const char* Name)
{
    const uint32_t Parsed = ParseUInt32(Value, Name);
    if (Parsed == 0 || Parsed > static_cast<uint32_t>(std::numeric_limits<int>::max()))
    {
        throw std::runtime_error(std::string(Name) + " must be positive");
    }
    return static_cast<int>(Parsed);
}

FOptions ParseOptions(const int ArgumentCount, char** Arguments)
{
    FOptions Options;
    for (int Index = 1; Index < ArgumentCount; ++Index)
    {
        const std::string Argument = Arguments[Index];
        if (Index + 1 >= ArgumentCount)
        {
            throw std::runtime_error("missing value for " + Argument);
        }
        const char* Value = Arguments[++Index];
        if (Argument == "--kernel")
        {
            Options.KernelPath = std::filesystem::path(Value);
        }
        else if (Argument == "--samples")
        {
            Options.Samples = ParsePositiveInt(Value, "samples");
        }
        else if (Argument == "--warmup")
        {
            Options.WarmupSamples = ParsePositiveInt(Value, "warmup");
        }
        else if (Argument == "--minimum-sample-ms")
        {
            Options.MinimumSampleMilliseconds = std::strtod(Value, nullptr);
            if (!(Options.MinimumSampleMilliseconds > 0.0))
            {
                throw std::runtime_error("minimum-sample-ms must be positive");
            }
        }
        else if (Argument == "--seed")
        {
            Options.Seed = static_cast<int32_t>(ParseUInt32(Value, "seed"));
        }
        else if (Argument == "--minimum-iterations")
        {
            Options.MinimumIterations = ParseUInt32(Value, "minimum-iterations");
        }
        else if (Argument == "--maximum-iterations")
        {
            Options.MaximumIterations = ParseUInt32(Value, "maximum-iterations");
        }
        else
        {
            throw std::runtime_error("unknown argument: " + Argument);
        }
    }

    if (Options.KernelPath.empty())
    {
        throw std::runtime_error("--kernel is required");
    }
    if (Options.MinimumIterations == 0
        || Options.MinimumIterations > Options.MaximumIterations)
    {
        throw std::runtime_error("iteration bounds are invalid");
    }
    return Options;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& Path)
{
    std::ifstream Stream(Path, std::ios::binary | std::ios::ate);
    if (!Stream)
    {
        throw std::runtime_error("unable to open kernel file");
    }
    const std::streamsize Size = Stream.tellg();
    if (Size <= 0)
    {
        throw std::runtime_error("kernel file is empty");
    }
    Stream.seekg(0, std::ios::beg);
    std::vector<uint8_t> Bytes(static_cast<size_t>(Size));
    if (!Stream.read(reinterpret_cast<char*>(Bytes.data()), Size))
    {
        throw std::runtime_error("unable to read kernel file");
    }
    return Bytes;
}

class FCompiledProfile
{
public:
    FCompiledProfile(
        const FProfileDefinition& InDefinition,
        const std::vector<uint8_t>& WasmBytes)
        : Definition(InDefinition)
    {
        AvidScriptWasmtimeEngineProfile Profile = {};
        Profile.SchemaVersion = 3;
        Profile.Strategy = AVIDSCRIPT_WASMTIME_ENGINE_STRATEGY_CRANELIFT;
        Profile.Optimization = AVIDSCRIPT_WASMTIME_ENGINE_OPT_SPEED;
        Profile.RegisterAllocator = AVIDSCRIPT_WASMTIME_ENGINE_REGALLOC_BACKTRACKING;
        Profile.Inlining = AVIDSCRIPT_WASMTIME_ENGINE_INLINING_ALL;
        Profile.TargetProfile = AVIDSCRIPT_WASMTIME_ENGINE_TARGET_X86_64_WINDOWS;
        Profile.CpuProfile = AVIDSCRIPT_WASMTIME_ENGINE_CPU_X86_64_V3;
        Profile.Wasm32MemoryReservationBytes = UINT64_C(1) << 32;
        Profile.MaxWasmStackBytes = UINT64_C(2) << 20;
        Profile.bMemoryMayMove = false;
        Profile.bSpectreMitigation = Definition.bSpectreMitigation;
        Profile.bNanCanonicalization = false;
        Profile.bParallelCompilation = true;
        Profile.bWasmGc = true;
        Profile.bConsumeFuel = false;
        Profile.bEpochInterruption = Definition.bEpochInterruption;
        Profile.CompilerInliningSetter =
            reinterpret_cast<AvidScriptWasmtimeCompilerInliningSetter>(
                &avidscript_wasmtime_config_compiler_inlining_set);
        Profile.ModulePrecompiler =
            reinterpret_cast<AvidScriptWasmtimeModulePrecompiler>(
                &avidscript_wasmtime_engine_precompile_module);

        Engine = avidscript_wasmtime_engine_new_with_profile(&Profile);
        if (Engine == nullptr)
        {
            throw std::runtime_error("unable to create Wasmtime engine");
        }

        AvidScriptWasmtimeFailure* Failure = avidscript_wasmtime_module_new(
            Engine,
            WasmBytes.data(),
            WasmBytes.size(),
            &Module);
        if (Failure != nullptr || Module == nullptr)
        {
            throw std::runtime_error(DescribeFailure(Failure));
        }

        Store = avidscript_wasmtime_store_new(Engine);
        Linker = avidscript_wasmtime_linker_new(Engine);
        if (Store == nullptr || Linker == nullptr)
        {
            throw std::runtime_error("unable to create Wasmtime store or linker");
        }
        if (!avidscript_wasmtime_store_set_limits(Store, UINT64_C(64) << 20))
        {
            throw std::runtime_error("unable to set linear memory limit");
        }
        if (Definition.bEpochInterruption
            && !avidscript_wasmtime_store_set_epoch_deadline(
                Store,
                std::numeric_limits<uint64_t>::max() / 2))
        {
            throw std::runtime_error("unable to set epoch deadline");
        }

        Failure = avidscript_wasmtime_linker_instantiate(
            Linker,
            Store,
            Module,
            &Instance);
        if (Failure != nullptr || Instance == nullptr)
        {
            throw std::runtime_error(DescribeFailure(Failure));
        }

        uint32_t ParameterCells = 0;
        uint32_t ResultCells = 0;
        const int ResolveStatus = avidscript_wasmtime_instance_resolve_event_export(
            Store,
            Instance,
            "run",
            3,
            &Function,
            &ParameterCells,
            &ResultCells);
        if (ResolveStatus != 0 || Function == nullptr
            || ParameterCells != 2 || ResultCells != 1)
        {
            throw std::runtime_error("kernel run export has an unexpected signature");
        }
    }

    ~FCompiledProfile()
    {
        avidscript_wasmtime_function_delete(Function);
        avidscript_wasmtime_instance_delete(Instance);
        avidscript_wasmtime_linker_delete(Linker);
        avidscript_wasmtime_store_delete(Store);
        avidscript_wasmtime_module_delete(Module);
        avidscript_wasmtime_engine_delete(Engine);
    }

    FCompiledProfile(const FCompiledProfile&) = delete;
    FCompiledProfile& operator=(const FCompiledProfile&) = delete;

    int32_t Run(const uint32_t Iterations, const int32_t Seed)
    {
        int32_t Result = 0;
        AvidScriptWasmtimeFailure* Failure = nullptr;
        const AvidScriptWasmtimeCallStatus Status =
            avidscript_wasmtime_function_call_i32_i32_to_i32_prepared_unchecked(
                Function,
                static_cast<int32_t>(Iterations),
                Seed,
                &Result,
                &Failure);
        if (Status != AVIDSCRIPT_WASMTIME_CALL_SUCCESS)
        {
            throw std::runtime_error(DescribeFailure(Failure));
        }
        return Result;
    }

    const FProfileDefinition Definition;

private:
    AvidScriptWasmtimeEngine* Engine = nullptr;
    AvidScriptWasmtimeModule* Module = nullptr;
    AvidScriptWasmtimeStore* Store = nullptr;
    AvidScriptWasmtimeLinker* Linker = nullptr;
    AvidScriptWasmtimeInstance* Instance = nullptr;
    AvidScriptWasmtimeFunction* Function = nullptr;
};

double Measure(
    FCompiledProfile& Profile,
    const uint32_t Iterations,
    const int32_t Seed,
    int32_t& OutChecksum)
{
    const FClock::time_point Start = FClock::now();
    OutChecksum = Profile.Run(Iterations, Seed);
    const FClock::time_point End = FClock::now();
    return std::chrono::duration<double, std::nano>(End - Start).count()
        / static_cast<double>(Iterations);
}

uint32_t CalibrateIterations(
    FCompiledProfile& Baseline,
    const FOptions& Options)
{
    uint32_t Iterations = Options.MinimumIterations;
    while (true)
    {
        int32_t Checksum = 0;
        const double NanosecondsPerIteration = Measure(
            Baseline,
            Iterations,
            Options.Seed,
            Checksum);
        const double TotalMilliseconds = NanosecondsPerIteration
            * static_cast<double>(Iterations) / 1000000.0;
        if (TotalMilliseconds >= Options.MinimumSampleMilliseconds
            || Iterations >= Options.MaximumIterations)
        {
            return Iterations;
        }
        Iterations = Iterations > Options.MaximumIterations / 2
            ? Options.MaximumIterations
            : Iterations * 2;
    }
}

double Percentile(std::vector<double> Values, const double Quantile)
{
    std::sort(Values.begin(), Values.end());
    const size_t Index = static_cast<size_t>(std::ceil(
        Quantile * static_cast<double>(Values.size()))) - 1;
    return Values[std::min(Index, Values.size() - 1)];
}

void WriteResult(
    const FOptions& Options,
    const uint32_t Iterations,
    const std::vector<FProfileResult>& Results)
{
    const double BaselineP50 = Percentile(
        Results.front().NanosecondsPerIteration,
        0.50);
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"evidence_class\": \"diagnostic_attribution\",\n"
        << "  \"kernel_path\": \""
        << EscapeJson(Options.KernelPath.generic_string()) << "\",\n"
        << "  \"iterations\": " << Iterations << ",\n"
        << "  \"warmup_samples\": " << Options.WarmupSamples << ",\n"
        << "  \"timed_samples\": " << Options.Samples << ",\n"
        << "  \"seed\": " << static_cast<uint32_t>(Options.Seed) << ",\n"
        << "  \"profiles\": [\n";

    for (size_t Index = 0; Index < Results.size(); ++Index)
    {
        const FProfileResult& Result = Results[Index];
        const double P50 = Percentile(Result.NanosecondsPerIteration, 0.50);
        const double P95 = Percentile(Result.NanosecondsPerIteration, 0.95);
        std::cout << "    {\"id\": \"" << Result.Definition.Id
            << "\", \"epoch_interruption\": "
            << (Result.Definition.bEpochInterruption ? "true" : "false")
            << ", \"spectre_mitigation\": "
            << (Result.Definition.bSpectreMitigation ? "true" : "false")
            << ", \"checksum\": " << static_cast<uint32_t>(Result.Checksum)
            << ", \"p50_ns_per_iteration\": " << P50
            << ", \"p95_ns_per_iteration\": " << P95
            << ", \"p50_ratio_vs_baseline\": " << (P50 / BaselineP50)
            << "}" << (Index + 1 == Results.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
}
} // namespace

int main(const int ArgumentCount, char** Arguments)
{
    try
    {
        const FOptions Options = ParseOptions(ArgumentCount, Arguments);
        const std::vector<uint8_t> WasmBytes = ReadFile(Options.KernelPath);
        const std::vector<FProfileDefinition> Definitions = {
            {"production_baseline", true, true},
            {"epoch_off", false, true},
            {"spectre_off", true, false},
            {"epoch_and_spectre_off", false, false}};

        std::vector<std::unique_ptr<FCompiledProfile>> Profiles;
        for (const FProfileDefinition& Definition : Definitions)
        {
            Profiles.push_back(std::make_unique<FCompiledProfile>(
                Definition,
                WasmBytes));
        }

        const uint32_t Iterations = CalibrateIterations(*Profiles.front(), Options);
        std::vector<FProfileResult> Results;
        for (const FProfileDefinition& Definition : Definitions)
        {
            Results.push_back({Definition, {}, 0});
            Results.back().NanosecondsPerIteration.reserve(Options.Samples);
        }

        for (int Warmup = 0; Warmup < Options.WarmupSamples; ++Warmup)
        {
            for (size_t Offset = 0; Offset < Profiles.size(); ++Offset)
            {
                const size_t Index = (static_cast<size_t>(Warmup) + Offset)
                    % Profiles.size();
                Results[Index].Checksum = Profiles[Index]->Run(
                    Iterations,
                    Options.Seed);
            }
        }

        for (int Sample = 0; Sample < Options.Samples; ++Sample)
        {
            for (size_t Offset = 0; Offset < Profiles.size(); ++Offset)
            {
                const size_t Index = (static_cast<size_t>(Sample) + Offset)
                    % Profiles.size();
                int32_t Checksum = 0;
                Results[Index].NanosecondsPerIteration.push_back(Measure(
                    *Profiles[Index],
                    Iterations,
                    Options.Seed,
                    Checksum));
                Results[Index].Checksum = Checksum;
            }
        }

        for (size_t Index = 1; Index < Results.size(); ++Index)
        {
            if (Results[Index].Checksum != Results.front().Checksum)
            {
                throw std::runtime_error("profile checksum mismatch");
            }
        }

        WriteResult(Options, Iterations, Results);
        return EXIT_SUCCESS;
    }
    catch (const std::exception& Error)
    {
        std::cerr << "Native profile probe failed: " << Error.what() << '\n';
        return EXIT_FAILURE;
    }
}
