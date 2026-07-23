#include "AvidScriptRuntimeBenchmark.h"

#include "Benchmark/AvidScriptBenchmarkStatistics.h"

#include "AvidScriptActorBinding.h"
#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptHash.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectTypeBinding.h"
#include "AvidScriptWasmRuntime.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptRuntimeBenchmark, Log, All);

namespace
{
FAvidScriptBenchmarkStats CalculateStats(TArray<double> Samples)
{
	return CalculateAvidScriptBenchmarkStats(MoveTemp(Samples));
}

double MeasureElapsedPerIterationMs(double StartSeconds, int32 Iterations)
{
	const int32 SafeIterations = FMath::Max(Iterations, 1);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	return ElapsedMs / static_cast<double>(SafeIterations);
}

bool CreateBenchmarkWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptHostBindingBenchmarkWorld"));
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

AActor* SpawnBenchmarkActor(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	AActor* Actor = World->SpawnActor<AActor>();
	if (Actor == nullptr)
	{
		return nullptr;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass(), TEXT("AvidScriptBenchmarkRoot"));
	if (RootComponent == nullptr)
	{
		return nullptr;
	}

	Actor->SetRootComponent(RootComponent);
	Actor->AddInstanceComponent(RootComponent);
	RootComponent->RegisterComponentWithWorld(World);
	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0), false, nullptr, ETeleportType::TeleportPhysics);
	return Actor;
}

void AppendMetrics(
	const FAvidScriptWasmRuntimeMetrics& Metrics,
	TArray<double>& RuntimeInitSamples,
	TArray<double>& ModuleLoadSamples,
	TArray<double>& ModuleInstantiateSamples,
	TArray<double>& ExecEnvCreateSamples,
	TArray<double>& BeginPlayCallSamples,
	TArray<double>& TickCallSamples,
	TArray<double>& UnloadSamples)
{
	RuntimeInitSamples.Add(Metrics.RuntimeInitMs);
	ModuleLoadSamples.Add(Metrics.ModuleLoadMs);
	ModuleInstantiateSamples.Add(Metrics.ModuleInstantiateMs);
	ExecEnvCreateSamples.Add(Metrics.ExecEnvCreateMs);
	BeginPlayCallSamples.Add(Metrics.BeginPlayCallMs);
	TickCallSamples.Add(Metrics.TickCallMs);
	UnloadSamples.Add(Metrics.UnloadMs);
}

void SetFailureFromSmokeResult(
	const FAvidScriptWasmSmokeResult& SmokeResult,
	FAvidScriptRuntimeBenchmarkResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = SmokeResult.ErrorCategory;
	OutResult.ErrorMessage = SmokeResult.ErrorMessage;
	OutResult.Summary = FString::Printf(
		TEXT("runtime_microbenchmark_failed | category=%s | message=%s"),
		OutResult.ErrorCategory.IsEmpty() ? TEXT("<none>") : *OutResult.ErrorCategory,
		OutResult.ErrorMessage.IsEmpty() ? TEXT("<none>") : *OutResult.ErrorMessage);
}

void SetTimerSchedulerFailure(
	FAvidScriptTimerSchedulerBenchmarkResult& OutResult,
	const FString& ErrorCategory,
	const FString& ErrorMessage)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.Summary = FString::Printf(
		TEXT("timer_scheduler_benchmark_failed | category=%s | message=%s"),
		ErrorCategory.IsEmpty() ? TEXT("<none>") : *ErrorCategory,
		ErrorMessage.IsEmpty() ? TEXT("<none>") : *ErrorMessage);
}

void SetHostBindingFailure(
	FAvidScriptHostBindingBenchmarkResult& OutResult,
	const FString& ErrorCategory,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript host binding benchmark error | category=%s | details=%s | next=%s"),
		ErrorCategory.IsEmpty() ? TEXT("<none>") : *ErrorCategory,
		Details.IsEmpty() ? TEXT("<none>") : *Details,
		NextAction.IsEmpty() ? TEXT("<none>") : *NextAction);
	OutResult.Summary = FString::Printf(
		TEXT("host_binding_benchmark_failed | category=%s | message=%s"),
		OutResult.ErrorCategory.IsEmpty() ? TEXT("<none>") : *OutResult.ErrorCategory,
		*OutResult.ErrorMessage);
}

void AppendBenchmarkU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendBenchmarkI32Leb(TArray<uint8>& Bytes, int32 Value)
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

void AppendBenchmarkString(TArray<uint8>& Bytes, const char* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendBenchmarkU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

void AppendBenchmarkSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendBenchmarkU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendBenchmarkI32Const(TArray<uint8>& Body, uint32 Value)
{
	Body.Add(0x41);
	AppendBenchmarkI32Leb(Body, static_cast<int32>(Value));
}

TArray<uint8> BuildTransformImportBenchmarkModule(
	TConstArrayView<FAvidScriptObjectHandle> Handles,
	bool bBatch)
{
	constexpr uint32 InputAddress = 64;
	constexpr uint32 OutputAddress = 4096;
	const uint32 ImportCount = bBatch ? 1u : 3u;

	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendBenchmarkU32Leb(Types, 3);
	Types.Add(0x60);
	AppendBenchmarkU32Leb(Types, 3);
	Types.Add(0x7f);
	Types.Add(0x7f);
	Types.Add(0x7f);
	AppendBenchmarkU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendBenchmarkU32Leb(Types, 0);
	AppendBenchmarkU32Leb(Types, 0);
	Types.Add(0x60);
	AppendBenchmarkU32Leb(Types, 1);
	Types.Add(0x7d);
	AppendBenchmarkU32Leb(Types, 0);
	AppendBenchmarkSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendBenchmarkU32Leb(Imports, ImportCount);
	const char* ImportNames[] = {
		"actor_get_location",
		"actor_get_rotation",
		"actor_get_scale"
	};
	if (bBatch)
	{
		AppendBenchmarkString(Imports, "avidscript");
		AppendBenchmarkString(Imports, "actor_get_transform_batch");
		Imports.Add(0x00);
		AppendBenchmarkU32Leb(Imports, 0);
	}
	else
	{
		for (const char* ImportName : ImportNames)
		{
			AppendBenchmarkString(Imports, "avidscript");
			AppendBenchmarkString(Imports, ImportName);
			Imports.Add(0x00);
			AppendBenchmarkU32Leb(Imports, 0);
		}
	}
	AppendBenchmarkSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendBenchmarkU32Leb(Functions, 2);
	AppendBenchmarkU32Leb(Functions, 1);
	AppendBenchmarkU32Leb(Functions, 2);
	AppendBenchmarkSection(Module, 3, Functions);

	TArray<uint8> Memory;
	AppendBenchmarkU32Leb(Memory, 1);
	Memory.Add(0x00);
	AppendBenchmarkU32Leb(Memory, 1);
	AppendBenchmarkSection(Module, 5, Memory);

	TArray<uint8> Exports;
	AppendBenchmarkU32Leb(Exports, 2);
	AppendBenchmarkString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendBenchmarkU32Leb(Exports, ImportCount);
	AppendBenchmarkString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	AppendBenchmarkU32Leb(Exports, ImportCount + 1);
	AppendBenchmarkSection(Module, 7, Exports);

	TArray<uint8> BeginBody;
	AppendBenchmarkU32Leb(BeginBody, 0);
	if (bBatch)
	{
		AppendBenchmarkI32Const(BeginBody, InputAddress);
		AppendBenchmarkI32Const(BeginBody, static_cast<uint32>(Handles.Num()));
		AppendBenchmarkI32Const(BeginBody, OutputAddress);
		BeginBody.Add(0x10);
		AppendBenchmarkU32Leb(BeginBody, 0);
		BeginBody.Add(0x1a);
	}
	else
	{
		for (const FAvidScriptObjectHandle& Handle : Handles)
		{
			for (uint32 ImportIndex = 0; ImportIndex < ImportCount; ++ImportIndex)
			{
				AppendBenchmarkI32Const(BeginBody, Handle.Slot);
				AppendBenchmarkI32Const(BeginBody, Handle.Generation);
				AppendBenchmarkI32Const(BeginBody, OutputAddress + ImportIndex * 12u);
				BeginBody.Add(0x10);
				AppendBenchmarkU32Leb(BeginBody, ImportIndex);
				BeginBody.Add(0x1a);
			}
		}
	}
	BeginBody.Add(0x0b);

	TArray<uint8> TickBody;
	AppendBenchmarkU32Leb(TickBody, 0);
	TickBody.Add(0x0b);
	TArray<uint8> Code;
	AppendBenchmarkU32Leb(Code, 2);
	AppendBenchmarkU32Leb(Code, static_cast<uint32>(BeginBody.Num()));
	Code.Append(BeginBody);
	AppendBenchmarkU32Leb(Code, static_cast<uint32>(TickBody.Num()));
	Code.Append(TickBody);
	AppendBenchmarkSection(Module, 10, Code);

	if (bBatch)
	{
		TArray<uint8> HandleBytes;
		HandleBytes.Reserve(Handles.Num() * 2 * sizeof(uint32));
		for (const FAvidScriptObjectHandle& Handle : Handles)
		{
			HandleBytes.Append(reinterpret_cast<const uint8*>(&Handle.Slot), sizeof(uint32));
			HandleBytes.Append(reinterpret_cast<const uint8*>(&Handle.Generation), sizeof(uint32));
		}
		TArray<uint8> Data;
		AppendBenchmarkU32Leb(Data, 1);
		Data.Add(0x00);
		AppendBenchmarkI32Const(Data, InputAddress);
		Data.Add(0x0b);
		AppendBenchmarkU32Leb(Data, static_cast<uint32>(HandleBytes.Num()));
		Data.Append(HandleBytes);
		AppendBenchmarkSection(Module, 11, Data);
	}
	return Module;
}

bool MeasureTransformImportModule(
	TConstArrayView<uint8> Bytecode,
	const FString& ModuleId,
	FAvidScriptObjectRegistry& Registry,
	int32 ExpectedImportCount,
	double& OutCallMs,
	FString& OutError)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult SmokeResult;
	if (!Runtime.LoadModule(Bytecode.GetData(), Bytecode.Num(), ModuleId, SmokeResult) ||
		!Runtime.BeginPlay(SmokeResult))
	{
		OutError = SmokeResult.ErrorMessage;
		return false;
	}
	if (SmokeResult.HostImportCallCount != ExpectedImportCount)
	{
		OutError = FString::Printf(
			TEXT("Unexpected WAMR transform import count | expected=%d | actual=%d"),
			ExpectedImportCount,
			SmokeResult.HostImportCallCount);
		return false;
	}
	OutCallMs = SmokeResult.Metrics.BeginPlayCallMs;
	Runtime.Unload();
	return true;
}

void SetTypedObjectBenchmarkFailure(
	FAvidScriptTypedObjectBenchmarkResult& OutResult,
	const FString& Category,
	const FString& Details)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript typed object benchmark error | category=%s | details=%s"),
		Category.IsEmpty() ? TEXT("<none>") : *Category,
		Details.IsEmpty() ? TEXT("<none>") : *Details);
	OutResult.Summary = FString::Printf(
		TEXT("typed_object_benchmark_failed | category=%s | message=%s"),
		*OutResult.ErrorCategory,
		*OutResult.ErrorMessage);
}

FAvidScriptBindingTypeModel MakeTypedObjectBenchmarkType(
	const TCHAR* ClassPath,
	const TCHAR* CppType,
	const int32 ObjectTypeOrdinal,
	const FString& BaseTypeId)
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("object:") + FString(ClassPath);
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(Type.CanonicalType, {});
	Type.Kind = TEXT("object_handle");
	Type.CppType = CppType;
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiTypes = { TEXT("i"), TEXT("i") };
	Type.ObjectTypeOrdinal = ObjectTypeOrdinal;
	Type.ClassPath = ClassPath;
	Type.BaseTypeId = BaseTypeId;
	return Type;
}

FAvidScriptBindingTypeModel MakeTypedObjectBenchmarkStaticOwnerType()
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("object:/Script/Engine.KismetMathLibrary");
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(Type.CanonicalType, {});
	Type.Kind = TEXT("object_handle");
	Type.CppType = TEXT("UKismetMathLibrary");
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiTypes = { TEXT("i"), TEXT("i") };
	return Type;
}

FAvidScriptBindingTypeModel MakeTypedObjectBenchmarkBoolType()
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("scalar:bool");
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(Type.CanonicalType, {});
	Type.Kind = TEXT("scalar");
	Type.CppType = TEXT("bool");
	Type.Size = 4;
	Type.Alignment = 4;
	Type.AbiTypes = { TEXT("i") };
	return Type;
}

FAvidScriptBindingValueModel MakeTypedObjectBenchmarkBoolValue(
	const TCHAR* Name,
	const TCHAR* Direction,
	const FAvidScriptBindingTypeModel& BoolType)
{
	FAvidScriptBindingValueModel Value;
	Value.Name = Name;
	Value.Direction = Direction;
	Value.CanonicalType = BoolType.CanonicalType;
	Value.TypeId = BoolType.StableId;
	Value.Kind = BoolType.Kind;
	Value.CppType = BoolType.CppType;
	Value.AbiTypes = BoolType.AbiTypes;
	return Value;
}

FAvidScriptBindingFunctionModel MakeTypedObjectBenchmarkSentinelBinding(
	const FAvidScriptBindingTypeModel& BoolType)
{
	FAvidScriptBindingFunctionModel Binding;
	Binding.Ordinal = 0;
	Binding.OwnerClass = TEXT("/Script/Engine.KismetMathLibrary");
	Binding.UeMember = TEXT("Not_PreBool");
	Binding.UeFunction = Binding.UeMember;
	Binding.ScriptName = TEXT("Not");
	Binding.DispatchMode = TEXT("cached_process_event");
	Binding.bStatic = true;
	Binding.bConst = false;
	Binding.ReloadEffect = EAvidScriptBindingReloadEffect::None;
	Binding.ReturnValue = MakeTypedObjectBenchmarkBoolValue(TEXT("ReturnValue"), TEXT("return"), BoolType);
	Binding.Parameters.Add(MakeTypedObjectBenchmarkBoolValue(TEXT("A"), TEXT("value"), BoolType));
	Binding.CanonicalIdentity = TEXT("/Script/Engine.KismetMathLibrary::Not_PreBool(scalar:bool;A:value:scalar:bool)");
	Binding.StableId = FAvidScriptHash::Sha256HexUtf8(Binding.CanonicalIdentity);
	Binding.HostImport.Module = TEXT("avidscript");
	Binding.HostImport.Name = TEXT("avid_ue_") + Binding.StableId.Left(16);
	Binding.HostImport.Signature = TEXT("(ii)i");
	return Binding;
}

void WriteTypedObjectBenchmarkType(
	const TSharedRef<TJsonWriter<>>& Writer,
	const FAvidScriptBindingTypeModel& Type)
{
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stable_id"), Type.StableId);
	Writer->WriteValue(TEXT("canonical_type"), Type.CanonicalType);
	Writer->WriteValue(TEXT("kind"), Type.Kind);
	Writer->WriteValue(TEXT("cpp_type"), Type.CppType);
	Writer->WriteValue(TEXT("size"), Type.Size);
	Writer->WriteValue(TEXT("alignment"), Type.Alignment);
	Writer->WriteArrayStart(TEXT("abi_types"));
	for (const FString& AbiType : Type.AbiTypes)
	{
		Writer->WriteValue(AbiType);
	}
	Writer->WriteArrayEnd();
	Writer->WriteValue(TEXT("object_type_ordinal"), Type.ObjectTypeOrdinal);
	Writer->WriteValue(TEXT("class_path"), Type.ClassPath);
	Writer->WriteValue(TEXT("base_type_id"), Type.BaseTypeId);
	Writer->WriteObjectEnd();
}

void WriteTypedObjectBenchmarkValue(
	const TSharedRef<TJsonWriter<>>& Writer,
	const FAvidScriptBindingValueModel& Value)
{
	Writer->WriteValue(TEXT("name"), Value.Name);
	Writer->WriteValue(TEXT("direction"), Value.Direction);
	Writer->WriteValue(TEXT("has_default"), Value.bHasDefault);
	Writer->WriteValue(TEXT("canonical_type"), Value.CanonicalType);
	Writer->WriteValue(TEXT("type_id"), Value.TypeId);
	Writer->WriteValue(TEXT("kind"), Value.Kind);
	Writer->WriteValue(TEXT("cpp_type"), Value.CppType);
	Writer->WriteArrayStart(TEXT("abi_types"));
	for (const FString& AbiType : Value.AbiTypes)
	{
		Writer->WriteValue(AbiType);
	}
	Writer->WriteArrayEnd();
}

bool MakeTypedObjectBenchmarkPackage(
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FAvidScriptBindingPackageLoadResult& OutLoadResult)
{
	FAvidScriptBindingPackageModel Model;
	Model.SchemaVersion = 6;
	Model.GeneratorVersion = TEXT("50.5.benchmark");
	Model.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Model.Source = TEXT("ue_reflection");
	Model.PackageName = TEXT("avidscript.benchmark.typed_object");
	const FAvidScriptBindingTypeModel UObjectType = MakeTypedObjectBenchmarkType(
		TEXT("/Script/CoreUObject.Object"), TEXT("UObject"), 0, FString());
	const FAvidScriptBindingTypeModel ActorComponentType = MakeTypedObjectBenchmarkType(
		TEXT("/Script/Engine.ActorComponent"), TEXT("UActorComponent"), 1, UObjectType.StableId);
	const FAvidScriptBindingTypeModel SceneComponentType = MakeTypedObjectBenchmarkType(
		TEXT("/Script/Engine.SceneComponent"), TEXT("USceneComponent"), 2, ActorComponentType.StableId);
	const FAvidScriptBindingTypeModel StaticOwnerType = MakeTypedObjectBenchmarkStaticOwnerType();
	const FAvidScriptBindingTypeModel BoolType = MakeTypedObjectBenchmarkBoolType();
	Model.Types = { UObjectType, ActorComponentType, SceneComponentType, StaticOwnerType, BoolType };
	const FAvidScriptBindingFunctionModel SentinelBinding = MakeTypedObjectBenchmarkSentinelBinding(BoolType);
	Model.Bindings.Add(SentinelBinding);
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
	Writer->WriteValue(TEXT("self_type_id"), Model.SelfTypeId);
	Writer->WriteArrayStart(TEXT("types"));
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		WriteTypedObjectBenchmarkType(Writer, Type);
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("class_references"));
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("bindings"));
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stable_id"), SentinelBinding.StableId);
	Writer->WriteValue(TEXT("canonical_identity"), SentinelBinding.CanonicalIdentity);
	Writer->WriteValue(TEXT("ordinal"), SentinelBinding.Ordinal);
	Writer->WriteValue(TEXT("owner_class"), SentinelBinding.OwnerClass);
	Writer->WriteValue(TEXT("binding_kind"), SentinelBinding.BindingKind);
	Writer->WriteValue(TEXT("ue_member"), SentinelBinding.UeMember);
	Writer->WriteValue(TEXT("script_name"), SentinelBinding.ScriptName);
	Writer->WriteValue(TEXT("dispatch_mode"), SentinelBinding.DispatchMode);
	Writer->WriteValue(TEXT("is_static"), SentinelBinding.bStatic);
	Writer->WriteValue(TEXT("is_const"), SentinelBinding.bConst);
	Writer->WriteValue(TEXT("reload_effect"), TEXT("none"));
	Writer->WriteObjectStart(TEXT("return"));
	WriteTypedObjectBenchmarkValue(Writer, SentinelBinding.ReturnValue);
	Writer->WriteObjectEnd();
	Writer->WriteArrayStart(TEXT("parameters"));
	for (const FAvidScriptBindingValueModel& Parameter : SentinelBinding.Parameters)
	{
		Writer->WriteObjectStart();
		WriteTypedObjectBenchmarkValue(Writer, Parameter);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectStart(TEXT("host_import"));
	Writer->WriteValue(TEXT("module"), SentinelBinding.HostImport.Module);
	Writer->WriteValue(TEXT("name"), SentinelBinding.HostImport.Name);
	Writer->WriteValue(TEXT("signature"), SentinelBinding.HostImport.Signature);
	Writer->WriteObjectEnd();
	Writer->WriteObjectEnd();
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	if (!Writer->Close())
	{
		return false;
	}
	return FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, OutPackage, OutLoadResult);
}

uint32 FindTypedObjectBenchmarkOrdinal(const FAvidScriptBindingPackage& Package)
{
	for (const FAvidScriptObjectTypeBindingSpec& Spec : FAvidScriptObjectTypeBindings::GetSpecs())
	{
		const FAvidScriptVmDynamicImport* Import = Package.GetVmPackage().Imports.FindByPredicate(
			[&Spec](const FAvidScriptVmDynamicImport& Candidate)
			{
				return Candidate.StableId == Spec.StableId;
			});
		if (Import != nullptr)
		{
			return Import->Ordinal;
		}
	}
	return MAX_uint32;
}

bool DispatchTypedObjectBenchmarkCall(
	const FAvidScriptBindingPackage& Package,
	const uint32 Ordinal,
	const TConstArrayView<uint64> Arguments,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Ordinal;
	Call.Arguments = Arguments;
	TArray<uint8> Scratch;
	return Package.Dispatch(Call, Context, Scratch, OutResult);
}

TArray<uint8> BuildTypedObjectCastBenchmarkModule(
	const FAvidScriptObjectHandle Handle,
	const uint32 TargetObjectTypeOrdinal)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendBenchmarkU32Leb(Types, 3);
	Types.Add(0x60);
	AppendBenchmarkU32Leb(Types, 3);
	Types.Add(0x7f);
	Types.Add(0x7f);
	Types.Add(0x7f);
	AppendBenchmarkU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendBenchmarkU32Leb(Types, 0);
	AppendBenchmarkU32Leb(Types, 0);
	Types.Add(0x60);
	AppendBenchmarkU32Leb(Types, 1);
	Types.Add(0x7d);
	AppendBenchmarkU32Leb(Types, 0);
	AppendBenchmarkSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendBenchmarkU32Leb(Imports, 1);
	AppendBenchmarkString(Imports, "avidscript");
	AppendBenchmarkString(Imports, "avid_object_type_is_a");
	Imports.Add(0x00);
	AppendBenchmarkU32Leb(Imports, 0);
	AppendBenchmarkSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendBenchmarkU32Leb(Functions, 2);
	AppendBenchmarkU32Leb(Functions, 1);
	AppendBenchmarkU32Leb(Functions, 2);
	AppendBenchmarkSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendBenchmarkU32Leb(Exports, 2);
	AppendBenchmarkString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendBenchmarkU32Leb(Exports, 1);
	AppendBenchmarkString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	AppendBenchmarkU32Leb(Exports, 2);
	AppendBenchmarkSection(Module, 7, Exports);

	TArray<uint8> BeginBody;
	AppendBenchmarkU32Leb(BeginBody, 0);
	BeginBody.Add(0x0b);
	TArray<uint8> TickBody;
	AppendBenchmarkU32Leb(TickBody, 0);
	AppendBenchmarkI32Const(TickBody, Handle.Slot);
	AppendBenchmarkI32Const(TickBody, Handle.Generation);
	AppendBenchmarkI32Const(TickBody, TargetObjectTypeOrdinal);
	TickBody.Add(0x10);
	AppendBenchmarkU32Leb(TickBody, 0);
	TickBody.Add(0x1a);
	TickBody.Add(0x0b);
	TArray<uint8> Code;
	AppendBenchmarkU32Leb(Code, 2);
	AppendBenchmarkU32Leb(Code, static_cast<uint32>(BeginBody.Num()));
	Code.Append(BeginBody);
	AppendBenchmarkU32Leb(Code, static_cast<uint32>(TickBody.Num()));
	Code.Append(TickBody);
	AppendBenchmarkSection(Module, 10, Code);
	return Module;
}
} // namespace

bool FAvidScriptRuntimeBenchmark::RunEmbeddedSmokeBenchmark(
	const FAvidScriptRuntimeBenchmarkOptions& Options,
	FAvidScriptRuntimeBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptRuntimeBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);

	TArray<double> RuntimeInitSamples;
	TArray<double> ModuleLoadSamples;
	TArray<double> ModuleInstantiateSamples;
	TArray<double> ExecEnvCreateSamples;
	TArray<double> BeginPlayCallSamples;
	TArray<double> TickCallSamples;
	TArray<double> UnloadSamples;

	RuntimeInitSamples.Reserve(OutResult.SampleCount);
	ModuleLoadSamples.Reserve(OutResult.SampleCount);
	ModuleInstantiateSamples.Reserve(OutResult.SampleCount);
	ExecEnvCreateSamples.Reserve(OutResult.SampleCount);
	BeginPlayCallSamples.Reserve(OutResult.SampleCount);
	TickCallSamples.Reserve(OutResult.SampleCount);
	UnloadSamples.Reserve(OutResult.SampleCount);

	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	const float TickDeltaSeconds = Options.TickDeltaSeconds > 0.0f ? Options.TickDeltaSeconds : 1.0f / 60.0f;

	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult SmokeResult;

		if (!Runtime.LoadEmbeddedSmokeModule(SmokeResult))
		{
			SetFailureFromSmokeResult(SmokeResult, OutResult);
			return false;
		}

		if (!Runtime.BeginPlay(SmokeResult))
		{
			SetFailureFromSmokeResult(SmokeResult, OutResult);
			return false;
		}

		if (!Runtime.Tick(TickDeltaSeconds, SmokeResult))
		{
			SetFailureFromSmokeResult(SmokeResult, OutResult);
			return false;
		}

		Runtime.Unload(SmokeResult);
		if (!SmokeResult.bUnloaded)
		{
			SetFailureFromSmokeResult(SmokeResult, OutResult);
			if (OutResult.ErrorMessage.IsEmpty())
			{
				OutResult.ErrorCategory = TEXT("unload_failed");
				OutResult.ErrorMessage = TEXT("Embedded runtime did not report a completed unload.");
			}
			return false;
		}

		if (RunIndex >= OutResult.WarmupCount)
		{
			AppendMetrics(
				SmokeResult.Metrics,
				RuntimeInitSamples,
				ModuleLoadSamples,
				ModuleInstantiateSamples,
				ExecEnvCreateSamples,
				BeginPlayCallSamples,
				TickCallSamples,
				UnloadSamples);
		}
	}

	OutResult.RuntimeInit = CalculateStats(RuntimeInitSamples);
	OutResult.ModuleLoad = CalculateStats(ModuleLoadSamples);
	OutResult.ModuleInstantiate = CalculateStats(ModuleInstantiateSamples);
	OutResult.ExecEnvCreate = CalculateStats(ExecEnvCreateSamples);
	OutResult.BeginPlayCall = CalculateStats(BeginPlayCallSamples);
	OutResult.TickCall = CalculateStats(TickCallSamples);
	OutResult.Unload = CalculateStats(UnloadSamples);
	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("runtime_microbenchmark | warmup=%d | samples=%d | load_avg_ms=%.4f | instantiate_avg_ms=%.4f | begin_avg_ms=%.4f | tick_avg_ms=%.4f | tick_p95_ms=%.4f | unload_avg_ms=%.4f"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.ModuleLoad.AvgMs,
		OutResult.ModuleInstantiate.AvgMs,
		OutResult.BeginPlayCall.AvgMs,
		OutResult.TickCall.AvgMs,
		OutResult.TickCall.P95Ms,
		OutResult.Unload.AvgMs);

	UE_LOG(LogAvidScriptRuntimeBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	return true;
}

bool FAvidScriptRuntimeBenchmark::RunTimerSchedulerBenchmark(
	const FAvidScriptTimerSchedulerBenchmarkOptions& Options,
	FAvidScriptTimerSchedulerBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptTimerSchedulerBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);
	OutResult.PendingTimerCount = FMath::Clamp(Options.PendingTimerCount, 1, 1023);
	OutResult.IterationsPerSample = FMath::Max(Options.IterationsPerSample, 1);

	TArray<double> IdleTickSamples;
	TArray<double> SetCancelChurnSamples;
	IdleTickSamples.Reserve(OutResult.SampleCount);
	SetCancelChurnSamples.Reserve(OutResult.SampleCount);

	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult SmokeResult;
		if (!Runtime.LoadEmbeddedSmokeModule(SmokeResult) || !Runtime.BeginPlay(SmokeResult))
		{
			SetTimerSchedulerFailure(OutResult, SmokeResult.ErrorCategory, SmokeResult.ErrorMessage);
			return false;
		}

		for (int32 TimerIndex = 0; TimerIndex < OutResult.PendingTimerCount; ++TimerIndex)
		{
			if (Runtime.HandleTimerSetOnceImport(3600.0f, TimerIndex) <= 0)
			{
				SetTimerSchedulerFailure(
					OutResult,
					TEXT("timer_setup_failed"),
					FString::Printf(TEXT("Failed to register pending timer %d of %d."), TimerIndex + 1, OutResult.PendingTimerCount));
				return false;
			}
		}

		const double IdleTickStartSeconds = FPlatformTime::Seconds();
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			if (!Runtime.Tick(0.0f, SmokeResult))
			{
				SetTimerSchedulerFailure(OutResult, SmokeResult.ErrorCategory, SmokeResult.ErrorMessage);
				return false;
			}
		}
		const double IdleTickMs = MeasureElapsedPerIterationMs(IdleTickStartSeconds, OutResult.IterationsPerSample);

		const double ChurnStartSeconds = FPlatformTime::Seconds();
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			const int32 TimerHandle = Runtime.HandleTimerSetOnceImport(3600.0f, IterationIndex);
			if (TimerHandle <= 0 || Runtime.HandleTimerCancelImport(TimerHandle) != 1)
			{
				SetTimerSchedulerFailure(
					OutResult,
					TEXT("timer_churn_failed"),
					FString::Printf(TEXT("Set/cancel churn failed at iteration %d."), IterationIndex));
				return false;
			}
		}
		const double SetCancelChurnMs = MeasureElapsedPerIterationMs(ChurnStartSeconds, OutResult.IterationsPerSample);

		Runtime.Unload(SmokeResult);
		if (!SmokeResult.bUnloaded)
		{
			SetTimerSchedulerFailure(
				OutResult,
				SmokeResult.ErrorCategory.IsEmpty() ? FString(TEXT("unload_failed")) : SmokeResult.ErrorCategory,
				SmokeResult.ErrorMessage.IsEmpty() ? FString(TEXT("Timer benchmark runtime did not unload.")) : SmokeResult.ErrorMessage);
			return false;
		}

		if (RunIndex >= OutResult.WarmupCount)
		{
			IdleTickSamples.Add(IdleTickMs);
			SetCancelChurnSamples.Add(SetCancelChurnMs);
		}
	}

	OutResult.IdleTick = CalculateStats(IdleTickSamples);
	OutResult.SetCancelChurn = CalculateStats(SetCancelChurnSamples);
	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("timer_scheduler_benchmark | warmup=%d | samples=%d | pending=%d | iterations=%d | idle_tick_avg_ms=%.6f | idle_tick_p95_ms=%.6f | set_cancel_avg_ms=%.6f | set_cancel_p95_ms=%.6f"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.PendingTimerCount,
		OutResult.IterationsPerSample,
		OutResult.IdleTick.AvgMs,
		OutResult.IdleTick.P95Ms,
		OutResult.SetCancelChurn.AvgMs,
		OutResult.SetCancelChurn.P95Ms);

	UE_LOG(LogAvidScriptRuntimeBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	return true;
}

bool FAvidScriptRuntimeBenchmark::RunHostBindingBenchmark(
	const FAvidScriptHostBindingBenchmarkOptions& Options,
	FAvidScriptHostBindingBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptHostBindingBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);
	OutResult.IterationsPerSample = FMath::Max(Options.IterationsPerSample, 1);
	OutResult.TransformBatchSize = FMath::Clamp(Options.TransformBatchSize, 1, AvidScriptMaximumActorTransformBatchSize);
	OutResult.WasmScalarImportsPerIteration = OutResult.TransformBatchSize * 3;
	OutResult.WasmBatchImportsPerIteration = 1;

	UWorld* World = nullptr;
	if (!CreateBenchmarkWorld(World))
	{
		SetHostBindingFailure(
			OutResult,
			TEXT("world_create_failed"),
			TEXT("Failed to create the host binding benchmark world."),
			TEXT("Run this benchmark in an initialized editor or commandlet context."));
		DestroyBenchmarkWorld(World);
		return false;
	}

	AActor* Actor = SpawnBenchmarkActor(World);
	if (Actor == nullptr)
	{
		SetHostBindingFailure(
			OutResult,
			TEXT("actor_spawn_failed"),
			TEXT("Failed to spawn the host binding benchmark actor."),
			TEXT("Verify the benchmark world can spawn actors and register a root component."));
		DestroyBenchmarkWorld(World);
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!RegisterResult.bSucceeded || !ActorHandle.IsValid())
	{
		SetHostBindingFailure(
			OutResult,
			RegisterResult.ErrorCategory.IsEmpty() ? FString(TEXT("register_failed")) : RegisterResult.ErrorCategory,
			RegisterResult.ErrorMessage,
			RegisterResult.NextAction);
		DestroyBenchmarkWorld(World);
		return false;
	}

	TArray<FAvidScriptObjectHandle> TransformHandles;
	TransformHandles.Reserve(OutResult.TransformBatchSize);
	TransformHandles.Add(ActorHandle);
	for (int32 Index = 1; Index < OutResult.TransformBatchSize; ++Index)
	{
		AActor* TransformActor = SpawnBenchmarkActor(World);
		if (TransformActor == nullptr)
		{
			SetHostBindingFailure(
				OutResult,
				TEXT("transform_actor_spawn_failed"),
				FString::Printf(TEXT("Failed to spawn transform batch actor %d."), Index),
				TEXT("Verify the benchmark world can spawn the configured batch size."));
			DestroyBenchmarkWorld(World);
			return false;
		}

		FAvidScriptObjectHandleResult TransformRegisterResult;
		const FAvidScriptObjectHandle TransformHandle = Registry.RegisterObject(TransformActor, TransformRegisterResult);
		if (!TransformRegisterResult.bSucceeded || !TransformHandle.IsValid())
		{
			SetHostBindingFailure(
				OutResult,
				TransformRegisterResult.ErrorCategory.IsEmpty() ? FString(TEXT("transform_register_failed")) : TransformRegisterResult.ErrorCategory,
				TransformRegisterResult.ErrorMessage,
				TransformRegisterResult.NextAction);
			DestroyBenchmarkWorld(World);
			return false;
		}
		TransformHandles.Add(TransformHandle);
	}

	TArray<double> DirectGetSamples;
	TArray<double> RegistryResolveSamples;
	TArray<double> BindingGetSamples;
	TArray<double> BindingSetSamples;
	TArray<double> ScalarTransformSamples;
	TArray<double> BatchTransformSamples;
	TArray<double> WasmScalarTransformSamples;
	TArray<double> WasmBatchTransformSamples;
	DirectGetSamples.Reserve(OutResult.SampleCount);
	RegistryResolveSamples.Reserve(OutResult.SampleCount);
	BindingGetSamples.Reserve(OutResult.SampleCount);
	BindingSetSamples.Reserve(OutResult.SampleCount);
	ScalarTransformSamples.Reserve(OutResult.SampleCount);
	BatchTransformSamples.Reserve(OutResult.SampleCount);
	WasmScalarTransformSamples.Reserve(OutResult.SampleCount);
	WasmBatchTransformSamples.Reserve(OutResult.SampleCount);

	const TArray<uint8> WasmScalarModule = BuildTransformImportBenchmarkModule(TransformHandles, false);
	const TArray<uint8> WasmBatchModule = BuildTransformImportBenchmarkModule(TransformHandles, true);
	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		const double DirectStartSeconds = FPlatformTime::Seconds();
		FVector LastDirectLocation = FVector::ZeroVector;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			LastDirectLocation = Actor->GetActorLocation();
		}
		const double DirectGetMs = MeasureElapsedPerIterationMs(DirectStartSeconds, OutResult.IterationsPerSample);

		OutResult.LastReadLocation = LastDirectLocation;

		const double RegistryStartSeconds = FPlatformTime::Seconds();
		AActor* ResolvedActor = nullptr;
		FAvidScriptObjectHandleResult ResolveResult;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			ResolvedActor = Registry.ResolveObject<AActor>(ActorHandle, ResolveResult);
			if (ResolvedActor == nullptr)
			{
				SetHostBindingFailure(
					OutResult,
					ResolveResult.ErrorCategory.IsEmpty() ? FString(TEXT("resolve_failed")) : ResolveResult.ErrorCategory,
					ResolveResult.ErrorMessage,
					ResolveResult.NextAction);
				DestroyBenchmarkWorld(World);
				return false;
			}
		}
		const double RegistryResolveMs = MeasureElapsedPerIterationMs(RegistryStartSeconds, OutResult.IterationsPerSample);

		const double BindingGetStartSeconds = FPlatformTime::Seconds();
		FVector BindingLocation = FVector::ZeroVector;
		FAvidScriptActorBindingResult BindingGetResult;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			if (!FAvidScriptActorBinding::GetActorLocation(
				Registry,
				ActorHandle,
				BindingLocation,
				BindingGetResult,
				EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
			{
				SetHostBindingFailure(
					OutResult,
					BindingGetResult.ErrorCategory.IsEmpty() ? FString(TEXT("binding_get_failed")) : BindingGetResult.ErrorCategory,
					BindingGetResult.ErrorMessage,
					BindingGetResult.NextAction);
				DestroyBenchmarkWorld(World);
				return false;
			}
		}
		const double BindingGetMs = MeasureElapsedPerIterationMs(BindingGetStartSeconds, OutResult.IterationsPerSample);

		const double BindingSetStartSeconds = FPlatformTime::Seconds();
		FAvidScriptActorBindingResult BindingSetResult;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			const FVector TargetLocation(
				1000.0 + static_cast<double>(RunIndex * OutResult.IterationsPerSample + IterationIndex),
				2000.0,
				3000.0);
			if (!FAvidScriptActorBinding::SetActorLocation(
				Registry,
				ActorHandle,
				TargetLocation,
				EAvidScriptActorWritePolicy::AllowWrites,
				BindingSetResult,
				nullptr,
				EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
			{
				SetHostBindingFailure(
					OutResult,
					BindingSetResult.ErrorCategory.IsEmpty() ? FString(TEXT("binding_set_failed")) : BindingSetResult.ErrorCategory,
					BindingSetResult.ErrorMessage,
					BindingSetResult.NextAction);
				DestroyBenchmarkWorld(World);
				return false;
			}
		}
		const double BindingSetMs = MeasureElapsedPerIterationMs(BindingSetStartSeconds, OutResult.IterationsPerSample);

		const int32 TransformOperationCount = OutResult.IterationsPerSample * TransformHandles.Num();
		const double ScalarTransformStartSeconds = FPlatformTime::Seconds();
		FAvidScriptActorTransformSnapshot ScalarSnapshot;
		FAvidScriptActorBindingResult ScalarTransformResult;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			for (const FAvidScriptObjectHandle& TransformHandle : TransformHandles)
			{
				if (!FAvidScriptActorBinding::GetActorTransform(
					Registry,
					TransformHandle,
					ScalarSnapshot,
					ScalarTransformResult,
					EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
				{
					SetHostBindingFailure(
						OutResult,
						ScalarTransformResult.ErrorCategory.IsEmpty() ? FString(TEXT("scalar_transform_failed")) : ScalarTransformResult.ErrorCategory,
						ScalarTransformResult.ErrorMessage,
						ScalarTransformResult.NextAction);
					DestroyBenchmarkWorld(World);
					return false;
				}
			}
		}
		const double ScalarTransformMs = MeasureElapsedPerIterationMs(ScalarTransformStartSeconds, TransformOperationCount);

		const double BatchTransformStartSeconds = FPlatformTime::Seconds();
		TArray<FAvidScriptActorTransformSnapshot> BatchSnapshots;
		FAvidScriptActorTransformBatchResult BatchTransformResult;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			if (!FAvidScriptActorBinding::GetActorTransforms(
				Registry,
				TransformHandles,
				BatchSnapshots,
				BatchTransformResult,
				EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
			{
				SetHostBindingFailure(
					OutResult,
					BatchTransformResult.ErrorCategory.IsEmpty() ? FString(TEXT("batch_transform_failed")) : BatchTransformResult.ErrorCategory,
					BatchTransformResult.ErrorMessage,
					BatchTransformResult.NextAction);
				DestroyBenchmarkWorld(World);
				return false;
			}
		}
		const double BatchTransformMs = MeasureElapsedPerIterationMs(BatchTransformStartSeconds, TransformOperationCount);

		double WasmScalarTransformMs = 0.0;
		double WasmBatchTransformMs = 0.0;
		FString WasmBenchmarkError;
		if (!MeasureTransformImportModule(
				WasmScalarModule,
				TEXT("benchmark_scalar_transform"),
				Registry,
				OutResult.WasmScalarImportsPerIteration,
				WasmScalarTransformMs,
				WasmBenchmarkError) ||
			!MeasureTransformImportModule(
				WasmBatchModule,
				TEXT("benchmark_batch_transform"),
				Registry,
				OutResult.WasmBatchImportsPerIteration,
				WasmBatchTransformMs,
				WasmBenchmarkError))
		{
			SetHostBindingFailure(
				OutResult,
				TEXT("wasm_transform_benchmark_failed"),
				WasmBenchmarkError,
				TEXT("Verify generated WAMR benchmark modules and transform host imports."));
			DestroyBenchmarkWorld(World);
			return false;
		}

		OutResult.LastReadLocation = BindingLocation;
		OutResult.FinalActorLocation = Actor->GetActorLocation();

		if (RunIndex >= OutResult.WarmupCount)
		{
			DirectGetSamples.Add(DirectGetMs);
			RegistryResolveSamples.Add(RegistryResolveMs);
			BindingGetSamples.Add(BindingGetMs);
			BindingSetSamples.Add(BindingSetMs);
			ScalarTransformSamples.Add(ScalarTransformMs);
			BatchTransformSamples.Add(BatchTransformMs);
			WasmScalarTransformSamples.Add(WasmScalarTransformMs);
			WasmBatchTransformSamples.Add(WasmBatchTransformMs);
		}
	}

	OutResult.DirectGetActorLocation = CalculateStats(DirectGetSamples);
	OutResult.RegistryResolveActor = CalculateStats(RegistryResolveSamples);
	OutResult.BindingGetActorLocation = CalculateStats(BindingGetSamples);
	OutResult.BindingSetActorLocation = CalculateStats(BindingSetSamples);
	OutResult.ScalarGetActorTransform = CalculateStats(ScalarTransformSamples);
	OutResult.BatchGetActorTransforms = CalculateStats(BatchTransformSamples);
	OutResult.WasmScalarGetActorTransforms = CalculateStats(WasmScalarTransformSamples);
	OutResult.WasmBatchGetActorTransforms = CalculateStats(WasmBatchTransformSamples);
	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("host_binding_benchmark | warmup=%d | samples=%d | iterations=%d | transform_batch=%d | direct_get_avg_ms=%.6f | registry_resolve_avg_ms=%.6f | binding_get_avg_ms=%.6f | binding_set_avg_ms=%.6f | binding_set_p95_ms=%.6f | scalar_transform_avg_ms=%.6f | scalar_transform_p95_ms=%.6f | batch_transform_avg_ms=%.6f | batch_transform_p95_ms=%.6f | wasm_scalar_imports=%d | wasm_scalar_transform_avg_ms=%.6f | wasm_batch_imports=%d | wasm_batch_transform_avg_ms=%.6f"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.IterationsPerSample,
		OutResult.TransformBatchSize,
		OutResult.DirectGetActorLocation.AvgMs,
		OutResult.RegistryResolveActor.AvgMs,
		OutResult.BindingGetActorLocation.AvgMs,
		OutResult.BindingSetActorLocation.AvgMs,
		OutResult.BindingSetActorLocation.P95Ms,
		OutResult.ScalarGetActorTransform.AvgMs,
		OutResult.ScalarGetActorTransform.P95Ms,
		OutResult.BatchGetActorTransforms.AvgMs,
		OutResult.BatchGetActorTransforms.P95Ms,
		OutResult.WasmScalarImportsPerIteration,
		OutResult.WasmScalarGetActorTransforms.AvgMs,
		OutResult.WasmBatchImportsPerIteration,
		OutResult.WasmBatchGetActorTransforms.AvgMs);

	UE_LOG(LogAvidScriptRuntimeBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	DestroyBenchmarkWorld(World);
	return true;
}

bool FAvidScriptRuntimeBenchmark::RunTypedObjectBenchmark(
	const FAvidScriptTypedObjectBenchmarkOptions& Options,
	FAvidScriptTypedObjectBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptTypedObjectBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);
	OutResult.IterationsPerSample = FMath::Max(Options.IterationsPerSample, 1);
	OutResult.UpcastHostImportsPerIteration = 0;
	OutResult.ExistingTypedBindingRegressionStatus = TEXT("pending_same_machine_phase49_baseline");

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!MakeTypedObjectBenchmarkPackage(Package, LoadResult) || !Package.IsValid())
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			LoadResult.ErrorCategory.IsEmpty() ? FString(TEXT("package_load_failed")) : LoadResult.ErrorCategory,
			LoadResult.ErrorDetails);
		return false;
	}
	const FAvidScriptBindingPackageInstrumentation InstrumentationBeforeWarmLoop =
		Package->GetInstrumentation();
	if (InstrumentationBeforeWarmLoop.ClassLoadCount > MAX_int32
		|| InstrumentationBeforeWarmLoop.ReflectedNameLookupCount > MAX_int32)
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			TEXT("instrumentation_overflow"),
			TEXT("Typed object package load instrumentation exceeded benchmark result width."));
		return false;
	}
	OutResult.BindingPackageClassLoadsDuringLoad =
		static_cast<int32>(InstrumentationBeforeWarmLoop.ClassLoadCount);
	OutResult.BindingPackageReflectedNameLookupsDuringLoad =
		static_cast<int32>(InstrumentationBeforeWarmLoop.ReflectedNameLookupCount);

	const uint32 ObjectTypeOrdinal = FindTypedObjectBenchmarkOrdinal(*Package);
	UClass* CachedActorComponentClass = nullptr;
	if (ObjectTypeOrdinal == MAX_uint32
		|| !Package->TryResolveObjectType(1, CachedActorComponentClass)
		|| CachedActorComponentClass != UActorComponent::StaticClass())
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			TEXT("object_type_plan_missing"),
			TEXT("The typed object benchmark package did not expose the cached ActorComponent type plan."));
		return false;
	}

	UWorld* World = nullptr;
	if (!CreateBenchmarkWorld(World))
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			TEXT("world_create_failed"),
			TEXT("The typed object benchmark requires an initialized editor or commandlet world context."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyBenchmarkWorld(World);
	};

	AActor* Actor = SpawnBenchmarkActor(World);
	USceneComponent* Component = Actor != nullptr ? Cast<USceneComponent>(Actor->GetRootComponent()) : nullptr;
	if (Actor == nullptr || Component == nullptr || !Component->IsA(CachedActorComponentClass))
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			TEXT("fixture_create_failed"),
			TEXT("The typed object benchmark could not create a SceneComponent fixture for the ActorComponent checked cast."));
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult, false);
	if (!RegisterResult.bSucceeded || !ActorHandle.IsValid())
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			RegisterResult.ErrorCategory.IsEmpty() ? FString(TEXT("actor_register_failed")) : RegisterResult.ErrorCategory,
			RegisterResult.ErrorMessage);
		return false;
	}
	const FAvidScriptObjectHandle ComponentHandle = Registry.RegisterObject(Component, RegisterResult, false);
	if (!RegisterResult.bSucceeded || !ComponentHandle.IsValid())
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			RegisterResult.ErrorCategory.IsEmpty() ? FString(TEXT("component_register_failed")) : RegisterResult.ErrorCategory,
			RegisterResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	const uint64 ObjectTypeArguments[] = {
		ComponentHandle.Slot,
		ComponentHandle.Generation,
		1
	};
	const TArray<uint8> WasmModule = BuildTypedObjectCastBenchmarkModule(ComponentHandle, 1);
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult LoadSmokeResult;
	if (!Runtime.LoadModule(
			WasmModule.GetData(),
			WasmModule.Num(),
			TEXT("benchmark_typed_object_checked_cast"),
			Package,
			LoadSmokeResult)
		|| !Runtime.BeginPlay(LoadSmokeResult))
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			TEXT("wasm_setup_failed"),
			LoadSmokeResult.ErrorMessage);
		return false;
	}
	ON_SCOPE_EXIT
	{
		Runtime.Unload();
	};

	TArray<double> NativeSamples;
	TArray<double> BindingSamples;
	TArray<double> WasmSamples;
	TArray<double> ExistingTypedBindingSamples;
	NativeSamples.Reserve(OutResult.SampleCount);
	BindingSamples.Reserve(OutResult.SampleCount);
	WasmSamples.Reserve(OutResult.SampleCount);
	ExistingTypedBindingSamples.Reserve(OutResult.SampleCount);
	int32 ExpectedWasmHostCrossings = 0;

	auto MeasureNativeIsA = [&]()
	{
		const double StartSeconds = FPlatformTime::Seconds();
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			if (!Component->IsA(CachedActorComponentClass))
			{
				return -1.0;
			}
		}
		return MeasureElapsedPerIterationMs(StartSeconds, OutResult.IterationsPerSample);
	};
	auto MeasureBindingObjectTypeIsA = [&]()
	{
		const double StartSeconds = FPlatformTime::Seconds();
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			FAvidScriptDynamicHostCallResult CallResult;
			if (!DispatchTypedObjectBenchmarkCall(
					*Package,
					ObjectTypeOrdinal,
					ObjectTypeArguments,
					Context,
					CallResult)
				|| CallResult.ReturnValue != 1)
			{
				return -1.0;
			}
		}
		return MeasureElapsedPerIterationMs(StartSeconds, OutResult.IterationsPerSample);
	};
	auto MeasureWasmCheckedCast = [&]()
	{
		const double StartSeconds = FPlatformTime::Seconds();
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			FAvidScriptWasmSmokeResult TickSmokeResult;
			if (!Runtime.Tick(1.0f / 60.0f, TickSmokeResult)
				|| TickSmokeResult.HostImportCallCount != ++ExpectedWasmHostCrossings)
			{
				return -1.0;
			}
		}
		return MeasureElapsedPerIterationMs(StartSeconds, OutResult.IterationsPerSample);
	};
	auto MeasureExistingTypedBinding = [&]()
	{
		const double StartSeconds = FPlatformTime::Seconds();
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			FVector Location;
			FAvidScriptActorBindingResult BindingResult;
			if (!FAvidScriptActorBinding::GetActorLocation(
					Registry,
					ActorHandle,
					Location,
					BindingResult,
					EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
			{
				return -1.0;
			}
		}
		return MeasureElapsedPerIterationMs(StartSeconds, OutResult.IterationsPerSample);
	};

	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		const bool bReverseOrder = (RunIndex & 1) != 0;
		double NativeMs = 0.0;
		double BindingMs = 0.0;
		double WasmMs = 0.0;
		double ExistingTypedBindingMs = 0.0;
		if (bReverseOrder)
		{
			WasmMs = MeasureWasmCheckedCast();
			ExistingTypedBindingMs = MeasureExistingTypedBinding();
			BindingMs = MeasureBindingObjectTypeIsA();
			NativeMs = MeasureNativeIsA();
		}
		else
		{
			NativeMs = MeasureNativeIsA();
			BindingMs = MeasureBindingObjectTypeIsA();
			ExistingTypedBindingMs = MeasureExistingTypedBinding();
			WasmMs = MeasureWasmCheckedCast();
		}
		if (NativeMs < 0.0 || BindingMs < 0.0 || WasmMs < 0.0 || ExistingTypedBindingMs < 0.0)
		{
			SetTypedObjectBenchmarkFailure(
				OutResult,
				TEXT("warm_dispatch_failed"),
				TEXT("A warm typed object dispatch returned an unexpected type result or host crossing count."));
			return false;
		}
		if (RunIndex >= OutResult.WarmupCount)
		{
			NativeSamples.Add(NativeMs);
			BindingSamples.Add(BindingMs);
			WasmSamples.Add(WasmMs);
			ExistingTypedBindingSamples.Add(ExistingTypedBindingMs);
		}
	}

	OutResult.NativeIsA = CalculateStats(NativeSamples);
	OutResult.BindingObjectTypeIsA = CalculateStats(BindingSamples);
	OutResult.WasmCheckedCast = CalculateStats(WasmSamples);
	OutResult.ExistingTypedBindingGetActorLocation = CalculateStats(ExistingTypedBindingSamples);
	const FAvidScriptBindingPackageInstrumentation InstrumentationAfterWarmLoop =
		Package->GetInstrumentation();
	const uint64 WarmClassLoads = InstrumentationAfterWarmLoop.ClassLoadCount
		- InstrumentationBeforeWarmLoop.ClassLoadCount;
	const uint64 WarmNameLookups = InstrumentationAfterWarmLoop.ReflectedNameLookupCount
		- InstrumentationBeforeWarmLoop.ReflectedNameLookupCount;
	if (WarmClassLoads != 0 || WarmNameLookups != 0)
	{
		SetTypedObjectBenchmarkFailure(
			OutResult,
			TEXT("warm_dispatch_lookup_budget_exceeded"),
			FString::Printf(TEXT("class_loads=%llu | reflected_name_lookups=%llu"), WarmClassLoads, WarmNameLookups));
		return false;
	}
	OutResult.BindingPackageClassLoadsDuringWarmLoop = static_cast<int32>(WarmClassLoads);
	OutResult.BindingPackageReflectedNameLookupsDuringWarmLoop = static_cast<int32>(WarmNameLookups);
	OutResult.NativeIsAOperationCount = OutResult.SampleCount * OutResult.IterationsPerSample;
	OutResult.BindingObjectTypeOperationCount = OutResult.NativeIsAOperationCount;
	OutResult.WasmCheckedCastOperationCount = OutResult.NativeIsAOperationCount;
	OutResult.WasmCheckedCastHostCrossingCount = OutResult.WasmCheckedCastOperationCount;
	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("typed_object_benchmark | warmup=%d | samples=%d | iterations=%d | native_is_a_p50_ms=%.6f | native_is_a_p95_ms=%.6f | binding_object_type_is_a_p50_ms=%.6f | binding_object_type_is_a_p95_ms=%.6f | wasm_checked_cast_p50_ms=%.6f | wasm_checked_cast_p95_ms=%.6f | existing_typed_binding_p50_ms=%.6f | native_operations=%d | binding_operations=%d | wasm_operations=%d | wasm_host_crossings=%d | warm_class_loads=%d | warm_reflected_name_lookups=%d | upcast_imports=%d | existing_typed_binding_budget=%s"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.IterationsPerSample,
		OutResult.NativeIsA.P50Ms,
		OutResult.NativeIsA.P95Ms,
		OutResult.BindingObjectTypeIsA.P50Ms,
		OutResult.BindingObjectTypeIsA.P95Ms,
		OutResult.WasmCheckedCast.P50Ms,
		OutResult.WasmCheckedCast.P95Ms,
		OutResult.ExistingTypedBindingGetActorLocation.P50Ms,
		OutResult.NativeIsAOperationCount,
		OutResult.BindingObjectTypeOperationCount,
		OutResult.WasmCheckedCastOperationCount,
		OutResult.WasmCheckedCastHostCrossingCount,
		OutResult.BindingPackageClassLoadsDuringWarmLoop,
		OutResult.BindingPackageReflectedNameLookupsDuringWarmLoop,
		OutResult.UpcastHostImportsPerIteration,
		*OutResult.ExistingTypedBindingRegressionStatus);
	UE_LOG(LogAvidScriptRuntimeBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	return true;
}
