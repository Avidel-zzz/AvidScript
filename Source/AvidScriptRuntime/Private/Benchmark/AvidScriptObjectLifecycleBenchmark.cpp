#include "AvidScriptRuntimeBenchmark.h"

#include "Benchmark/AvidScriptBenchmarkStatistics.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptWasmRuntime.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptObjectLifecycleBenchmark, Log, All);

namespace
{
constexpr uint32 AvidScriptLifecycleTransformAddress = 16;
constexpr uint32 AvidScriptLifecycleHandleBaseAddress = 64;
constexpr uint32 AvidScriptLifecycleHandleSize = sizeof(uint32) * 2;

class FAvidScriptLifecycleBenchmarkGuestMemory final : public IAvidScriptVmGuestMemory
{
public:
	explicit FAvidScriptLifecycleBenchmarkGuestMemory(const int32 IterationCount)
	{
		Bytes.SetNumZeroed(
			static_cast<int32>(AvidScriptLifecycleHandleBaseAddress)
			+ FMath::Max(IterationCount, 1) * static_cast<int32>(AvidScriptLifecycleHandleSize));
	}

	bool ReadBytes(const uint32 GuestAddress, TArrayView<uint8> OutBytes, FString& OutError) override
	{
		if (!IsRangeValid(GuestAddress, OutBytes.Num()))
		{
			OutError = TEXT("Lifecycle benchmark guest read is out of bounds.");
			return false;
		}
		FMemory::Memcpy(OutBytes.GetData(), Bytes.GetData() + GuestAddress, OutBytes.Num());
		return true;
	}

	bool WriteBytes(const uint32 GuestAddress, TConstArrayView<uint8> InBytes, FString& OutError) override
	{
		if (!IsRangeValid(GuestAddress, InBytes.Num()))
		{
			OutError = TEXT("Lifecycle benchmark guest write is out of bounds.");
			return false;
		}
		FMemory::Memcpy(Bytes.GetData() + GuestAddress, InBytes.GetData(), InBytes.Num());
		return true;
	}

	void WriteIdentityTransform()
	{
		const float Components[9] = {
			0.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 0.0f,
			1.0f, 1.0f, 1.0f
		};
		FMemory::Memcpy(
			Bytes.GetData() + AvidScriptLifecycleTransformAddress,
			Components,
			sizeof(Components));
	}

	FAvidScriptObjectHandle ReadHandle(const int32 Index) const
	{
		FAvidScriptObjectHandle Handle;
		const uint32 Address = AvidScriptLifecycleHandleBaseAddress
			+ static_cast<uint32>(Index) * AvidScriptLifecycleHandleSize;
		if (IsRangeValid(Address, AvidScriptLifecycleHandleSize))
		{
			FMemory::Memcpy(&Handle.Slot, Bytes.GetData() + Address, sizeof(Handle.Slot));
			FMemory::Memcpy(
				&Handle.Generation,
				Bytes.GetData() + Address + sizeof(Handle.Slot),
				sizeof(Handle.Generation));
		}
		return Handle;
	}

private:
	bool IsRangeValid(const uint32 GuestAddress, const uint64 Size) const
	{
		return GuestAddress <= static_cast<uint64>(Bytes.Num())
			&& Size <= static_cast<uint64>(Bytes.Num()) - GuestAddress;
	}

	TArray<uint8> Bytes;
};

double MeasureLifecyclePerIterationMs(const double StartSeconds, const int32 IterationCount)
{
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	return ElapsedMs / static_cast<double>(FMath::Max(IterationCount, 1));
}

void SetLifecycleBenchmarkFailure(
	FAvidScriptObjectLifecycleBenchmarkResult& OutResult,
	const FString& Category,
	const FString& Details)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript object lifecycle benchmark error | category=%s | details=%s"),
		Category.IsEmpty() ? TEXT("<none>") : *Category,
		Details.IsEmpty() ? TEXT("<none>") : *Details);
	OutResult.Summary = FString::Printf(
		TEXT("object_lifecycle_benchmark_failed | category=%s | message=%s"),
		OutResult.ErrorCategory.IsEmpty() ? TEXT("<none>") : *OutResult.ErrorCategory,
		*OutResult.ErrorMessage);
}

bool CreateLifecycleBenchmarkWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptObjectLifecycleBenchmarkWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyLifecycleBenchmarkWorld(UWorld*& World)
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

AActor* SpawnLifecycleBenchmarkActor(UWorld& World, UClass& ActorClass)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World.SpawnActor<AActor>(&ActorClass, FTransform::Identity, SpawnParameters);
}

void AppendLifecycleU32Leb(TArray<uint8>& Bytes, uint32 Value)
{
	do
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		if (Value != 0)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	} while (Value != 0);
}

void AppendLifecycleI32Leb(TArray<uint8>& Bytes, int32 Value)
{
	bool bMore = true;
	while (bMore)
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		const bool bSignBitSet = (Byte & 0x40) != 0;
		bMore = !((Value == 0 && !bSignBitSet) || (Value == -1 && bSignBitSet));
		if (bMore)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	}
}

void AppendLifecycleString(TArray<uint8>& Bytes, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	AppendLifecycleU32Leb(Bytes, static_cast<uint32>(Utf8.Length()));
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void AppendLifecycleSection(TArray<uint8>& Module, const uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendLifecycleU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendLifecycleI32Const(TArray<uint8>& Body, const int32 Value)
{
	Body.Add(0x41);
	AppendLifecycleI32Leb(Body, Value);
}

void AppendLifecycleI32Load(TArray<uint8>& Body, const int32 Address)
{
	AppendLifecycleI32Const(Body, Address);
	Body.Add(0x28);
	AppendLifecycleU32Leb(Body, 2);
	AppendLifecycleU32Leb(Body, 0);
}

TArray<uint8> BuildLifecycleWasmCrossingProbe(
	const FAvidScriptObjectLifecycleBindingSpec& SpawnSpec,
	const FAvidScriptObjectLifecycleBindingSpec& DestroySpec)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendLifecycleU32Leb(Types, 4);
	Types.Add(0x60);
	AppendLifecycleU32Leb(Types, 3);
	Types.Add(0x7f);
	Types.Add(0x7f);
	Types.Add(0x7f);
	AppendLifecycleU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendLifecycleU32Leb(Types, 2);
	Types.Add(0x7f);
	Types.Add(0x7f);
	AppendLifecycleU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendLifecycleU32Leb(Types, 0);
	AppendLifecycleU32Leb(Types, 0);
	Types.Add(0x60);
	AppendLifecycleU32Leb(Types, 1);
	Types.Add(0x7d);
	AppendLifecycleU32Leb(Types, 0);
	AppendLifecycleSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendLifecycleU32Leb(Imports, 2);
	AppendLifecycleString(Imports, SpawnSpec.ModuleName);
	AppendLifecycleString(Imports, SpawnSpec.ImportName);
	Imports.Add(0x00);
	AppendLifecycleU32Leb(Imports, 0);
	AppendLifecycleString(Imports, DestroySpec.ModuleName);
	AppendLifecycleString(Imports, DestroySpec.ImportName);
	Imports.Add(0x00);
	AppendLifecycleU32Leb(Imports, 1);
	AppendLifecycleSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendLifecycleU32Leb(Functions, 2);
	AppendLifecycleU32Leb(Functions, 2);
	AppendLifecycleU32Leb(Functions, 3);
	AppendLifecycleSection(Module, 3, Functions);

	TArray<uint8> Memory;
	AppendLifecycleU32Leb(Memory, 1);
	Memory.Add(0x00);
	AppendLifecycleU32Leb(Memory, 1);
	AppendLifecycleSection(Module, 5, Memory);

	TArray<uint8> Exports;
	AppendLifecycleU32Leb(Exports, 2);
	AppendLifecycleString(Exports, TEXT("avid_on_begin_play"));
	Exports.Add(0x00);
	AppendLifecycleU32Leb(Exports, 2);
	AppendLifecycleString(Exports, TEXT("avid_on_tick"));
	Exports.Add(0x00);
	AppendLifecycleU32Leb(Exports, 3);
	AppendLifecycleSection(Module, 7, Exports);

	TArray<uint8> BeginBody;
	AppendLifecycleU32Leb(BeginBody, 0);
	AppendLifecycleI32Const(BeginBody, 0);
	AppendLifecycleI32Const(BeginBody, static_cast<int32>(AvidScriptLifecycleTransformAddress));
	AppendLifecycleI32Const(BeginBody, static_cast<int32>(AvidScriptLifecycleHandleBaseAddress));
	BeginBody.Add(0x10);
	AppendLifecycleU32Leb(BeginBody, 0);
	BeginBody.Add(0x1a);
	AppendLifecycleI32Load(BeginBody, static_cast<int32>(AvidScriptLifecycleHandleBaseAddress));
	AppendLifecycleI32Load(
		BeginBody,
		static_cast<int32>(AvidScriptLifecycleHandleBaseAddress + sizeof(uint32)));
	BeginBody.Add(0x10);
	AppendLifecycleU32Leb(BeginBody, 1);
	BeginBody.Add(0x1a);
	BeginBody.Add(0x0b);

	TArray<uint8> TickBody;
	AppendLifecycleU32Leb(TickBody, 0);
	TickBody.Add(0x0b);
	TArray<uint8> Code;
	AppendLifecycleU32Leb(Code, 2);
	AppendLifecycleU32Leb(Code, static_cast<uint32>(BeginBody.Num()));
	Code.Append(BeginBody);
	AppendLifecycleU32Leb(Code, static_cast<uint32>(TickBody.Num()));
	Code.Append(TickBody);
	AppendLifecycleSection(Module, 10, Code);

	const float TransformComponents[9] = {
		0.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f,
		1.0f, 1.0f, 1.0f
	};
	TArray<uint8> Data;
	AppendLifecycleU32Leb(Data, 1);
	Data.Add(0x00);
	AppendLifecycleI32Const(Data, static_cast<int32>(AvidScriptLifecycleTransformAddress));
	Data.Add(0x0b);
	AppendLifecycleU32Leb(Data, sizeof(TransformComponents));
	Data.Append(reinterpret_cast<const uint8*>(TransformComponents), sizeof(TransformComponents));
	AppendLifecycleSection(Module, 11, Data);
	return Module;
}

bool RunLifecycleWasmCrossingProbe(
	const TSharedPtr<const FAvidScriptBindingPackage>& Package,
	UWorld& World,
	FAvidScriptObjectRegistry& Registry,
	int32& OutObservedImports,
	FString& OutError)
{
	const FAvidScriptObjectLifecycleBindingSpec* SpawnSpec = nullptr;
	const FAvidScriptObjectLifecycleBindingSpec* DestroySpec = nullptr;
	for (const FAvidScriptObjectLifecycleBindingSpec& Spec : FAvidScriptObjectLifecycleBindings::GetSpecs())
	{
		if (Spec.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor)
		{
			SpawnSpec = &Spec;
		}
		else if (Spec.Kind == EAvidScriptBindingInvocationKind::ObjectDestroyActor)
		{
			DestroySpec = &Spec;
		}
	}
	if (SpawnSpec == nullptr || DestroySpec == nullptr)
	{
		OutError = TEXT("Lifecycle binding specs do not contain SpawnActor and DestroyActor.");
		return false;
	}

	const TArray<uint8> Module = BuildLifecycleWasmCrossingProbe(*SpawnSpec, *DestroySpec);
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.World = &World;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult SmokeResult;
	if (!Runtime.LoadModule(
			Module.GetData(),
			Module.Num(),
			TEXT("benchmark_lifecycle_crossing_probe"),
			Package,
			SmokeResult)
		|| !Runtime.BeginPlay(SmokeResult))
	{
		OutError = SmokeResult.ErrorMessage;
		return false;
	}
	OutObservedImports = SmokeResult.HostImportCallCount;
	if (OutObservedImports != 2 || Registry.GetLiveHandleCount() != 0)
	{
		OutError = FString::Printf(
			TEXT("Lifecycle WAMR probe mismatch | imports=%d | live_handles=%d"),
			OutObservedImports,
			Registry.GetLiveHandleCount());
		return false;
	}
	Runtime.Unload();
	return true;
}

bool MakeLifecycleBenchmarkPackage(
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FAvidScriptBindingPackageLoadResult& OutLoadResult)
{
	FAvidScriptBindingPackageModel Model;
	Model.SchemaVersion = 5;
	Model.GeneratorVersion = TEXT("49.4.benchmark");
	Model.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Model.Source = TEXT("ue_reflection");
	Model.PackageName = TEXT("avidscript.benchmark.object_lifecycle");

	FAvidScriptBindingClassReferenceModel ActorReference;
	ActorReference.Ordinal = 0;
	ActorReference.ScriptName = TEXT("ActorClass");
	ActorReference.ClassPath = TEXT("/Script/Engine.Actor");
	ActorReference.BaseClassPath = TEXT("/Script/Engine.Actor");
	ActorReference.LoadPolicy = TEXT("EditorLoad");
	ActorReference.StableId = FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
		ActorReference.ClassPath,
		ActorReference.BaseClassPath,
		ActorReference.LoadPolicy);
	Model.ClassReferences.Add(ActorReference);
	Model.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Model);
	Model.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Model);

	FString DescriptorJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DescriptorJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), Model.SchemaVersion);
	Writer->WriteValue(TEXT("generator_version"), Model.GeneratorVersion);
	Writer->WriteValue(TEXT("engine_version"), Model.EngineVersion);
	Writer->WriteValue(TEXT("source"), Model.Source);
	Writer->WriteValue(TEXT("package_name"), Model.PackageName);
	Writer->WriteValue(TEXT("package_hash"), Model.PackageHash);
	Writer->WriteValue(TEXT("selection_hash"), Model.SelectionHash);
	Writer->WriteArrayStart(TEXT("types"));
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("class_references"));
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stable_id"), ActorReference.StableId);
	Writer->WriteValue(TEXT("ordinal"), ActorReference.Ordinal);
	Writer->WriteValue(TEXT("script_name"), ActorReference.ScriptName);
	Writer->WriteValue(TEXT("class_path"), ActorReference.ClassPath);
	Writer->WriteValue(TEXT("base_class_path"), ActorReference.BaseClassPath);
	Writer->WriteValue(TEXT("load_policy"), ActorReference.LoadPolicy);
	Writer->WriteObjectEnd();
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("bindings"));
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	if (!Writer->Close())
	{
		return false;
	}
	return FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, OutPackage, OutLoadResult);
}

uint32 FindLifecycleBenchmarkOrdinal(
	const FAvidScriptBindingPackage& Package,
	const EAvidScriptBindingInvocationKind Kind)
{
	for (const FAvidScriptObjectLifecycleBindingSpec& Spec : FAvidScriptObjectLifecycleBindings::GetSpecs())
	{
		if (Spec.Kind != Kind)
		{
			continue;
		}
		const FAvidScriptVmDynamicImport* Import = Package.GetVmPackage().Imports.FindByPredicate(
			[&Spec](const FAvidScriptVmDynamicImport& Candidate)
			{
				return Candidate.StableId == Spec.StableId;
			});
		return Import != nullptr ? Import->Ordinal : MAX_uint32;
	}
	return MAX_uint32;
}

bool DispatchLifecycleBenchmarkCall(
	const FAvidScriptBindingPackage& Package,
	const uint32 Ordinal,
	const TConstArrayView<uint64> Arguments,
	IAvidScriptVmGuestMemory* GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	TArray<uint8>& Scratch,
	FAvidScriptDynamicHostCallResult& OutCallResult)
{
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Ordinal;
	Call.Arguments = Arguments;
	Call.GuestMemory = GuestMemory;
	return Package.Dispatch(Call, Context, Scratch, OutCallResult);
}
} // namespace

bool FAvidScriptRuntimeBenchmark::RunObjectLifecycleBenchmark(
	const FAvidScriptObjectLifecycleBenchmarkOptions& Options,
	FAvidScriptObjectLifecycleBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptObjectLifecycleBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);
	OutResult.IterationsPerSample = FMath::Max(Options.IterationsPerSample, 1);

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!MakeLifecycleBenchmarkPackage(Package, LoadResult) || !Package.IsValid())
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			LoadResult.ErrorCategory.IsEmpty() ? FString(TEXT("package_load_failed")) : LoadResult.ErrorCategory,
			LoadResult.ErrorDetails);
		return false;
	}

	UClass* ActorClass = nullptr;
	UClass* ActorBaseClass = nullptr;
	if (!Package->TryResolveClassReference(0, ActorClass, ActorBaseClass)
		|| ActorClass == nullptr
		|| ActorBaseClass == nullptr)
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			TEXT("class_plan_missing"),
			TEXT("The warmed lifecycle package did not expose Actor class ordinal zero."));
		return false;
	}

	const uint32 SpawnOrdinal = FindLifecycleBenchmarkOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::ObjectSpawnActor);
	const uint32 DestroyOrdinal = FindLifecycleBenchmarkOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::ObjectDestroyActor);
	if (SpawnOrdinal == MAX_uint32 || DestroyOrdinal == MAX_uint32)
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			TEXT("lifecycle_import_missing"),
			TEXT("The warmed package did not publish SpawnActor and DestroyActor imports."));
		return false;
	}

	UWorld* World = nullptr;
	if (!CreateLifecycleBenchmarkWorld(World))
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			TEXT("world_create_failed"),
			TEXT("The benchmark requires an initialized editor or commandlet world context."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyLifecycleBenchmarkWorld(World);
	};

	FAvidScriptObjectRegistry Registry;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.World = World;
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> Scratch;
	Scratch.SetNumZeroed(Package->GetRequiredScratchSize());
	FAvidScriptLifecycleBenchmarkGuestMemory GuestMemory(OutResult.IterationsPerSample);
	GuestMemory.WriteIdentityTransform();
	const FAvidScriptBindingPackageInstrumentation InstrumentationBeforeWarmLoop =
		Package->GetInstrumentation();
	OutResult.BindingPackageClassLoadsDuringLoad = static_cast<int32>(
		FMath::Min<uint64>(InstrumentationBeforeWarmLoop.ClassLoadCount, MAX_int32));
	OutResult.BindingPackageReflectedNameLookupsDuringLoad = static_cast<int32>(
		FMath::Min<uint64>(InstrumentationBeforeWarmLoop.ReflectedNameLookupCount, MAX_int32));
	FString WasmProbeError;
	if (!RunLifecycleWasmCrossingProbe(
			Package,
			*World,
			Registry,
			OutResult.WasmLifecycleImportsObserved,
			WasmProbeError))
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			TEXT("wasm_crossing_probe_failed"),
			WasmProbeError);
		return false;
	}
	OutResult.SpawnImportsPerIteration = 1;
	OutResult.DestroyImportsPerIteration = 1;

	AActor* ResolveActor = SpawnLifecycleBenchmarkActor(*World, *ActorClass);
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ResolveHandle = Registry.RegisterObject(ResolveActor, RegisterResult, false);
	if (ResolveActor == nullptr || !RegisterResult.bSucceeded || !ResolveHandle.IsValid())
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			TEXT("resolve_fixture_failed"),
			TEXT("Failed to create the warm registry resolve fixture."));
		return false;
	}

	TArray<double> NativeSpawnSamples;
	TArray<double> BindingSpawnSamples;
	TArray<double> NativeDestroySamples;
	TArray<double> BindingDestroySamples;
	TArray<double> ClassResolveSamples;
	TArray<double> RegistryResolveSamples;
	for (TArray<double>* Samples : {
		&NativeSpawnSamples,
		&BindingSpawnSamples,
		&NativeDestroySamples,
		&BindingDestroySamples,
		&ClassResolveSamples,
		&RegistryResolveSamples })
	{
		Samples->Reserve(OutResult.SampleCount);
	}

	FAvidScriptDynamicHostCallResult CallResult;
	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		const bool bRecord = RunIndex >= OutResult.WarmupCount;

		const double ClassResolveStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
		{
			UClass* ResolvedClass = nullptr;
			UClass* ResolvedBaseClass = nullptr;
			if (!Package->TryResolveClassReference(0, ResolvedClass, ResolvedBaseClass)
				|| ResolvedClass != ActorClass
				|| ResolvedBaseClass != ActorBaseClass)
			{
				SetLifecycleBenchmarkFailure(
					OutResult,
					TEXT("class_plan_resolve_failed"),
					TEXT("A warmed class ordinal failed to resolve during the timed loop."));
				return false;
			}
		}
		const double ClassResolveMs = MeasureLifecyclePerIterationMs(
			ClassResolveStart,
			OutResult.IterationsPerSample);

		const double RegistryResolveStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
		{
			FAvidScriptObjectHandleResult ResolveResult;
			if (Registry.ResolveObject<AActor>(ResolveHandle, ResolveResult, false) != ResolveActor
				|| !ResolveResult.bSucceeded
				|| !ResolveResult.ObjectPath.IsEmpty())
			{
				SetLifecycleBenchmarkFailure(
					OutResult,
					TEXT("registry_resolve_failed"),
					TEXT("A warmed Actor handle failed zero-diagnostic resolution during the timed loop."));
				return false;
			}
		}
		const double RegistryResolveMs = MeasureLifecyclePerIterationMs(
			RegistryResolveStart,
			OutResult.IterationsPerSample);

		auto MeasureNativeSpawn = [&]() -> bool
		{
			TArray<AActor*> Actors;
			Actors.SetNumUninitialized(OutResult.IterationsPerSample);
			const double StartSeconds = FPlatformTime::Seconds();
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				Actors[Index] = SpawnLifecycleBenchmarkActor(*World, *ActorClass);
				if (Actors[Index] == nullptr)
				{
					SetLifecycleBenchmarkFailure(
						OutResult,
						TEXT("native_spawn_failed"),
						TEXT("Native UWorld::SpawnActor failed during the timed loop."));
					return false;
				}
			}
			const double SampleMs = MeasureLifecyclePerIterationMs(
				StartSeconds,
				OutResult.IterationsPerSample);
			for (AActor* Actor : Actors)
			{
				if (!Actor->Destroy())
				{
					SetLifecycleBenchmarkFailure(
						OutResult,
						TEXT("native_spawn_cleanup_failed"),
						TEXT("Native SpawnActor fixture cleanup failed."));
					return false;
				}
			}
			if (bRecord)
			{
				NativeSpawnSamples.Add(SampleMs);
			}
			return true;
		};

		auto MeasureBindingSpawn = [&]() -> bool
		{
			const double StartSeconds = FPlatformTime::Seconds();
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				const uint64 Arguments[] = {
					0,
					AvidScriptLifecycleTransformAddress,
					AvidScriptLifecycleHandleBaseAddress
						+ static_cast<uint64>(Index) * AvidScriptLifecycleHandleSize
				};
				if (!DispatchLifecycleBenchmarkCall(
						*Package,
						SpawnOrdinal,
						Arguments,
						&GuestMemory,
						Context,
						Scratch,
						CallResult))
				{
					SetLifecycleBenchmarkFailure(
						OutResult,
						TEXT("binding_spawn_failed"),
						CallResult.Details);
					return false;
				}
			}
			const double SampleMs = MeasureLifecyclePerIterationMs(
				StartSeconds,
				OutResult.IterationsPerSample);
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				const FAvidScriptObjectHandle Handle = GuestMemory.ReadHandle(Index);
				const uint64 Arguments[] = { Handle.Slot, Handle.Generation };
				if (!DispatchLifecycleBenchmarkCall(
						*Package,
						DestroyOrdinal,
						Arguments,
						nullptr,
						Context,
						Scratch,
						CallResult))
				{
					SetLifecycleBenchmarkFailure(
						OutResult,
						TEXT("binding_spawn_cleanup_failed"),
						CallResult.Details);
					return false;
				}
			}
			if (bRecord)
			{
				BindingSpawnSamples.Add(SampleMs);
			}
			return true;
		};

		auto MeasureNativeDestroy = [&]() -> bool
		{
			TArray<AActor*> Actors;
			Actors.SetNumUninitialized(OutResult.IterationsPerSample);
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				Actors[Index] = SpawnLifecycleBenchmarkActor(*World, *ActorClass);
				if (Actors[Index] == nullptr)
				{
					SetLifecycleBenchmarkFailure(
						OutResult,
						TEXT("native_destroy_setup_failed"),
						TEXT("Failed to create a Native DestroyActor fixture."));
					return false;
				}
			}
			const double StartSeconds = FPlatformTime::Seconds();
			for (AActor* Actor : Actors)
			{
				if (!Actor->Destroy())
				{
					SetLifecycleBenchmarkFailure(
						OutResult,
						TEXT("native_destroy_failed"),
						TEXT("Native AActor::Destroy failed during the timed loop."));
					return false;
				}
			}
			const double SampleMs = MeasureLifecyclePerIterationMs(
				StartSeconds,
				OutResult.IterationsPerSample);
			if (bRecord)
			{
				NativeDestroySamples.Add(SampleMs);
			}
			return true;
		};

		auto MeasureBindingDestroy = [&]() -> bool
		{
			TArray<FAvidScriptObjectHandle> Handles;
			Handles.SetNumUninitialized(OutResult.IterationsPerSample);
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				AActor* Actor = SpawnLifecycleBenchmarkActor(*World, *ActorClass);
				FAvidScriptObjectHandleResult FixtureRegisterResult;
				Handles[Index] = Registry.RegisterObject(Actor, FixtureRegisterResult, false);
				if (Actor == nullptr || !FixtureRegisterResult.bSucceeded || !Handles[Index].IsValid())
				{
					SetLifecycleBenchmarkFailure(
						OutResult,
						TEXT("binding_destroy_setup_failed"),
						TEXT("Failed to create a Binding DestroyActor fixture."));
					return false;
				}
			}
			const double StartSeconds = FPlatformTime::Seconds();
			for (const FAvidScriptObjectHandle& Handle : Handles)
			{
				const uint64 Arguments[] = { Handle.Slot, Handle.Generation };
				if (!DispatchLifecycleBenchmarkCall(
						*Package,
						DestroyOrdinal,
						Arguments,
						nullptr,
						Context,
						Scratch,
						CallResult))
				{
					SetLifecycleBenchmarkFailure(
						OutResult,
						TEXT("binding_destroy_failed"),
						CallResult.Details);
					return false;
				}
			}
			const double SampleMs = MeasureLifecyclePerIterationMs(
				StartSeconds,
				OutResult.IterationsPerSample);
			if (bRecord)
			{
				BindingDestroySamples.Add(SampleMs);
			}
			return true;
		};

		const bool bNativeFirst = (RunIndex & 1) == 0;
		if (bNativeFirst)
		{
			if (!MeasureNativeSpawn() || !MeasureBindingSpawn()
				|| !MeasureNativeDestroy() || !MeasureBindingDestroy())
			{
				return false;
			}
		}
		else if (!MeasureBindingSpawn() || !MeasureNativeSpawn()
			|| !MeasureBindingDestroy() || !MeasureNativeDestroy())
		{
			return false;
		}

		if (Registry.GetLiveHandleCount() != 1)
		{
			SetLifecycleBenchmarkFailure(
				OutResult,
				TEXT("registry_leak_detected"),
				TEXT("A benchmark run did not return to the single resolve fixture handle."));
			return false;
		}
		if (bRecord)
		{
			ClassResolveSamples.Add(ClassResolveMs);
			RegistryResolveSamples.Add(RegistryResolveMs);
		}
	}

	FAvidScriptObjectHandleResult ReleaseResult;
	if (!ResolveActor->Destroy())
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			TEXT("resolve_fixture_destroy_failed"),
			TEXT("Native cleanup could not destroy the resolve fixture actor."));
		return false;
	}
	if (!Registry.ReleaseHandle(ResolveHandle, ReleaseResult, false) || Registry.GetLiveHandleCount() != 0)
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			TEXT("resolve_fixture_cleanup_failed"),
			ReleaseResult.ErrorMessage);
		return false;
	}

	OutResult.NativeSpawnActor = CalculateAvidScriptBenchmarkStats(MoveTemp(NativeSpawnSamples));
	OutResult.BindingSpawnActor = CalculateAvidScriptBenchmarkStats(MoveTemp(BindingSpawnSamples));
	OutResult.NativeDestroyActor = CalculateAvidScriptBenchmarkStats(MoveTemp(NativeDestroySamples));
	OutResult.BindingDestroyActor = CalculateAvidScriptBenchmarkStats(MoveTemp(BindingDestroySamples));
	OutResult.ClassOrdinalResolve = CalculateAvidScriptBenchmarkStats(MoveTemp(ClassResolveSamples));
	OutResult.RegistryResolveSpawnedActor = CalculateAvidScriptBenchmarkStats(MoveTemp(RegistryResolveSamples));
	const FAvidScriptBindingPackageInstrumentation InstrumentationAfterWarmLoop =
		Package->GetInstrumentation();
	const uint64 WarmClassLoads = InstrumentationAfterWarmLoop.ClassLoadCount
		- InstrumentationBeforeWarmLoop.ClassLoadCount;
	const uint64 WarmNameLookups = InstrumentationAfterWarmLoop.ReflectedNameLookupCount
		- InstrumentationBeforeWarmLoop.ReflectedNameLookupCount;
	if (WarmClassLoads > MAX_int32 || WarmNameLookups > MAX_int32)
	{
		SetLifecycleBenchmarkFailure(
			OutResult,
			TEXT("instrumentation_overflow"),
			TEXT("Binding package instrumentation exceeded the benchmark result width."));
		return false;
	}
	OutResult.BindingPackageClassLoadsDuringWarmLoop = static_cast<int32>(WarmClassLoads);
	OutResult.BindingPackageReflectedNameLookupsDuringWarmLoop = static_cast<int32>(WarmNameLookups);
	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("object_lifecycle_benchmark | warmup=%d | samples=%d | iterations=%d | native_spawn_p50_ms=%.6f | native_spawn_p95_ms=%.6f | binding_spawn_p50_ms=%.6f | binding_spawn_p95_ms=%.6f | native_destroy_p50_ms=%.6f | native_destroy_p95_ms=%.6f | binding_destroy_p50_ms=%.6f | binding_destroy_p95_ms=%.6f | class_resolve_p50_ms=%.6f | registry_resolve_p50_ms=%.6f | binding_package_class_loads=%d | binding_package_reflected_name_lookups=%d | warm_binding_package_class_loads=%d | warm_binding_package_reflected_name_lookups=%d | wasm_imports_observed=%d | spawn_imports=%d | destroy_imports=%d"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.IterationsPerSample,
		OutResult.NativeSpawnActor.P50Ms,
		OutResult.NativeSpawnActor.P95Ms,
		OutResult.BindingSpawnActor.P50Ms,
		OutResult.BindingSpawnActor.P95Ms,
		OutResult.NativeDestroyActor.P50Ms,
		OutResult.NativeDestroyActor.P95Ms,
		OutResult.BindingDestroyActor.P50Ms,
		OutResult.BindingDestroyActor.P95Ms,
		OutResult.ClassOrdinalResolve.P50Ms,
		OutResult.RegistryResolveSpawnedActor.P50Ms,
		OutResult.BindingPackageClassLoadsDuringLoad,
		OutResult.BindingPackageReflectedNameLookupsDuringLoad,
		OutResult.BindingPackageClassLoadsDuringWarmLoop,
		OutResult.BindingPackageReflectedNameLookupsDuringWarmLoop,
		OutResult.WasmLifecycleImportsObserved,
		OutResult.SpawnImportsPerIteration,
		OutResult.DestroyImportsPerIteration);
	UE_LOG(LogAvidScriptObjectLifecycleBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	return true;
}
