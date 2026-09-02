#include "AvidScriptWasmtimeCompilerProfile.h"

#if PLATFORM_WINDOWS && PLATFORM_CPU_X86_FAMILY
#include <intrin.h>
#endif

namespace
{
constexpr uint32 WasmtimeCompilerProfileSchemaVersion = 3;

FAvidScriptWasmtimeCompilerProfile MakeWin64CompilerProfile()
{
	FAvidScriptWasmtimeCompilerProfile Profile;
	Profile.Id = TEXT("cranelift-speed-x86_64-v3-contained-v3");
	Profile.TargetTriple = TEXT("x86_64-pc-windows-msvc");
	Profile.CpuProfile = TEXT("x86-64-v3");
	Profile.EngineProfile.SchemaVersion =
		WasmtimeCompilerProfileSchemaVersion;
	Profile.EngineProfile.Strategy =
		AVIDSCRIPT_WASMTIME_ENGINE_STRATEGY_CRANELIFT;
	Profile.EngineProfile.Optimization =
		AVIDSCRIPT_WASMTIME_ENGINE_OPT_SPEED;
	Profile.EngineProfile.RegisterAllocator =
		AVIDSCRIPT_WASMTIME_ENGINE_REGALLOC_BACKTRACKING;
	Profile.EngineProfile.Inlining =
		AVIDSCRIPT_WASMTIME_ENGINE_INLINING_ALL;
	Profile.EngineProfile.TargetProfile =
		AVIDSCRIPT_WASMTIME_ENGINE_TARGET_X86_64_WINDOWS;
	Profile.EngineProfile.CpuProfile =
		AVIDSCRIPT_WASMTIME_ENGINE_CPU_X86_64_V3;
	Profile.EngineProfile.Wasm32MemoryReservationBytes =
		UINT64_C(1) << 32;
	Profile.EngineProfile.MaxWasmStackBytes = UINT64_C(2) << 20;
	Profile.EngineProfile.bMemoryMayMove = false;
	Profile.EngineProfile.bSpectreMitigation = true;
	Profile.EngineProfile.bNanCanonicalization = false;
	Profile.EngineProfile.bParallelCompilation = true;
	Profile.EngineProfile.bWasmGc = true;
	Profile.EngineProfile.bConsumeFuel = true;
	Profile.EngineProfile.bEpochInterruption = true;
	return Profile;
}

FAvidScriptWasmtimeCompilerProfile MakeAndroidCompilerProfile()
{
	FAvidScriptWasmtimeCompilerProfile Profile = MakeWin64CompilerProfile();
	Profile.Id = TEXT("cranelift-speed-arm64-v8a-contained-v1");
	Profile.TargetTriple = TEXT("aarch64-linux-android");
	Profile.CpuProfile = TEXT("arm64-v8a");
	Profile.EngineProfile.TargetProfile =
		AVIDSCRIPT_WASMTIME_ENGINE_TARGET_AARCH64_ANDROID;
	Profile.EngineProfile.CpuProfile =
		AVIDSCRIPT_WASMTIME_ENGINE_CPU_ARM64_V8A;
	return Profile;
}

#if PLATFORM_WINDOWS && PLATFORM_CPU_X86_FAMILY
bool HasBit(const int32 Value, const uint32 Bit)
{
	return (static_cast<uint32>(Value) & (UINT32_C(1) << Bit)) != 0;
}
#endif
} // namespace

const FAvidScriptWasmtimeCompilerProfile&
GetAvidScriptWasmtimeCompilerProfile()
{
	static const FAvidScriptWasmtimeCompilerProfile Profile =
		MakeWin64CompilerProfile();
	return Profile;
}

const FAvidScriptWasmtimeCompilerProfile*
FindAvidScriptWasmtimeCompilerProfile(const FString& TargetTriple)
{
	static const FAvidScriptWasmtimeCompilerProfile AndroidProfile =
		MakeAndroidCompilerProfile();
	if (TargetTriple.IsEmpty()
		|| TargetTriple == TEXT("x86_64-pc-windows-msvc"))
	{
		return &GetAvidScriptWasmtimeCompilerProfile();
	}
	if (TargetTriple == AndroidProfile.TargetTriple)
	{
		return &AndroidProfile;
	}
	return nullptr;
}

bool ValidateAvidScriptWasmtimeCompilerCpuProfile(FString& OutError)
{
	OutError.Reset();
#if !PLATFORM_WINDOWS || !PLATFORM_CPU_X86_FAMILY
	OutError = TEXT("The x86-64-v3 Wasmtime compiler profile requires Win64 on x86-64.");
	return false;
#else
	int32 BasicRoot[4] = {};
	__cpuid(BasicRoot, 0);
	if (BasicRoot[0] < 7)
	{
		OutError = TEXT("The CPU does not expose the CPUID leaves required by x86-64-v3.");
		return false;
	}

	int32 BasicFeatures[4] = {};
	__cpuidex(BasicFeatures, 1, 0);
	int32 ExtendedFeatures[4] = {};
	__cpuidex(ExtendedFeatures, 7, 0);
	int32 ExtendedRoot[4] = {};
	__cpuid(ExtendedRoot, static_cast<int32>(UINT32_C(0x80000000)));
	int32 ExtendedCpuFeatures[4] = {};
	if (static_cast<uint32>(ExtendedRoot[0]) >= UINT32_C(0x80000001))
	{
		__cpuid(
			ExtendedCpuFeatures,
			static_cast<int32>(UINT32_C(0x80000001)));
	}

	TArray<FString, TInlineAllocator<16>> Missing;
	auto Require = [&Missing](const bool bAvailable, const TCHAR* Name)
	{
		if (!bAvailable)
		{
			Missing.Add(Name);
		}
	};
	Require(HasBit(BasicFeatures[2], 0), TEXT("sse3"));
	Require(HasBit(BasicFeatures[2], 9), TEXT("ssse3"));
	Require(HasBit(BasicFeatures[2], 13), TEXT("cmpxchg16b"));
	Require(HasBit(BasicFeatures[2], 19), TEXT("sse4.1"));
	Require(HasBit(BasicFeatures[2], 20), TEXT("sse4.2"));
	Require(HasBit(BasicFeatures[2], 22), TEXT("movbe"));
	Require(HasBit(BasicFeatures[2], 23), TEXT("popcnt"));
	Require(HasBit(BasicFeatures[2], 12), TEXT("fma"));
	Require(HasBit(BasicFeatures[2], 27), TEXT("osxsave"));
	Require(HasBit(BasicFeatures[2], 28), TEXT("avx"));
	Require(HasBit(BasicFeatures[2], 29), TEXT("f16c"));
	Require(HasBit(ExtendedFeatures[1], 3), TEXT("bmi1"));
	Require(HasBit(ExtendedFeatures[1], 5), TEXT("avx2"));
	Require(HasBit(ExtendedFeatures[1], 8), TEXT("bmi2"));
	Require(HasBit(ExtendedCpuFeatures[2], 5), TEXT("lzcnt"));
	if (HasBit(BasicFeatures[2], 27))
	{
		const uint64 Xcr0 = _xgetbv(0);
		Require((Xcr0 & UINT64_C(0x6)) == UINT64_C(0x6), TEXT("os-avx-state"));
	}

	if (!Missing.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("The CPU does not satisfy x86-64-v3; missing: %s"),
			*FString::Join(Missing, TEXT(",")));
		return false;
	}
	return true;
#endif
}

FString BuildAvidScriptWasmtimeCompilerIdentity(
	const FString& RuntimeVersion,
	const FString& RuntimeArtifactSha256,
	const FAvidScriptWasmtimeCompilerProfile& CompilerProfile)
{
	return FString::Printf(
		TEXT("wasmtime-v%s+avidscript.1;strategy=cranelift;")
		TEXT("opt=speed;regalloc=backtracking;inlining=all;")
		TEXT("profile=%s;target=%s;cpu=%s;")
		TEXT("wasm32_memory=4g_fixed;memory_may_move=0;")
		TEXT("max_wasm_stack=2m;fuel=on;epoch_interruption=on;")
		TEXT("spectre=on;nan_canonicalization=off;parallel_compilation=on;")
		TEXT("wasm_gc=on;gc_collector=drc;")
		TEXT("runtime_profile=fastest-runtime;runtime_artifact_sha256=%s"),
		*RuntimeVersion,
		*CompilerProfile.Id,
		*CompilerProfile.TargetTriple,
		*CompilerProfile.CpuProfile,
		*RuntimeArtifactSha256);
}
