#include "AvidScriptRuntimeBenchmark.h"

#include "Benchmark/AvidScriptBenchmarkStatistics.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptSceneAttachmentBinding.h"
#include "AvidScriptWasmRuntime.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptObjectFactoryBenchmark, Log, All);

namespace
{
constexpr int32 AvidScriptObjectFactoryTypeOrdinal = 4;
constexpr int32 AvidScriptObjectFactoryImportCount = 4;

double MeasureObjectFactoryPerIterationMs(
	const double StartSeconds,
	const int32 IterationCount)
{
	return (FPlatformTime::Seconds() - StartSeconds) * 1000.0
		/ static_cast<double>(FMath::Max(IterationCount, 1));
}

void SetObjectFactoryBenchmarkFailure(
	FAvidScriptObjectFactoryBenchmarkResult& OutResult,
	const FString& Category,
	const FString& Details)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript object factory benchmark error | category=%s | details=%s"),
		Category.IsEmpty() ? TEXT("<none>") : *Category,
		Details.IsEmpty() ? TEXT("<none>") : *Details);
	OutResult.Summary = FString::Printf(
		TEXT("object_factory_benchmark_failed | category=%s | message=%s"),
		OutResult.ErrorCategory.IsEmpty() ? TEXT("<none>") : *OutResult.ErrorCategory,
		*OutResult.ErrorMessage);
}

bool CreateObjectFactoryBenchmarkWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptObjectFactoryBenchmarkWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyObjectFactoryBenchmarkWorld(UWorld*& World)
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

template <typename TComponent>
TComponent* CreateObjectFactoryBenchmarkComponent(AActor& Owner)
{
	TComponent* Component = NewObject<TComponent>(&Owner);
	if (Component == nullptr)
	{
		return nullptr;
	}
	Component->CreationMethod = EComponentCreationMethod::Instance;
	Owner.AddInstanceComponent(Component);
	Component->RegisterComponent();
	return Component;
}

FAvidScriptBindingTypeModel MakeObjectFactoryBenchmarkType(
	const TCHAR* ClassPath,
	const TCHAR* CppType,
	const int32 Ordinal,
	const FString& BaseTypeId)
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("object:") + FString(ClassPath);
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		Type.CanonicalType,
		{});
	Type.Kind = TEXT("object_handle");
	Type.CppType = CppType;
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiTypes = { TEXT("i"), TEXT("i") };
	Type.ObjectTypeOrdinal = Ordinal;
	Type.ClassPath = ClassPath;
	Type.BaseTypeId = BaseTypeId;
	return Type;
}

void WriteObjectFactoryBenchmarkType(
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

bool MakeObjectFactoryBenchmarkPackage(
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FAvidScriptBindingPackageLoadResult& OutLoadResult)
{
	FAvidScriptBindingPackageModel Model;
	Model.SchemaVersion = 7;
	Model.GeneratorVersion = TEXT("51.5.benchmark");
	Model.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Model.Source = TEXT("ue_reflection");
	Model.PackageName = TEXT("avidscript.benchmark.object_factory");

	const FAvidScriptBindingTypeModel ObjectType = MakeObjectFactoryBenchmarkType(
		TEXT("/Script/CoreUObject.Object"), TEXT("UObject"), 0, FString());
	const FAvidScriptBindingTypeModel ActorType = MakeObjectFactoryBenchmarkType(
		TEXT("/Script/Engine.Actor"), TEXT("AActor"), 1, ObjectType.StableId);
	const FAvidScriptBindingTypeModel ActorComponentType = MakeObjectFactoryBenchmarkType(
		TEXT("/Script/Engine.ActorComponent"), TEXT("UActorComponent"), 2, ObjectType.StableId);
	const FAvidScriptBindingTypeModel SceneComponentType = MakeObjectFactoryBenchmarkType(
		TEXT("/Script/Engine.SceneComponent"), TEXT("USceneComponent"), 3, ActorComponentType.StableId);
	const FAvidScriptBindingTypeModel StaticMeshComponentType = MakeObjectFactoryBenchmarkType(
		TEXT("/Script/Engine.StaticMeshComponent"),
		TEXT("UStaticMeshComponent"),
		AvidScriptObjectFactoryTypeOrdinal,
		SceneComponentType.StableId);
	Model.Types = {
		ObjectType,
		ActorType,
		ActorComponentType,
		SceneComponentType,
		StaticMeshComponentType
	};
	Model.SelfTypeId = ActorType.StableId;

	FAvidScriptBindingClassReferenceModel Reference;
	Reference.Ordinal = 0;
	Reference.ScriptName = TEXT("BenchmarkStaticMeshComponent");
	Reference.ClassPath = TEXT("/Script/Engine.StaticMeshComponent");
	Reference.BaseClassPath = TEXT("/Script/Engine.SceneComponent");
	Reference.LoadPolicy = TEXT("EditorLoad");
	Reference.ResultTypeId = SceneComponentType.StableId;
	Reference.StableId = FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
		Reference.ClassPath,
		Reference.BaseClassPath,
		Reference.LoadPolicy);
	Model.ClassReferences = { Reference };

	FAvidScriptBindingObjectFactoryModel Factory;
	Factory.Ordinal = 0;
	Factory.ScriptName = TEXT("BenchmarkStaticMeshComponent");
	Factory.ClassReferenceId = Reference.StableId;
	Factory.Kind = EAvidScriptObjectFactoryKind::ActorComponent;
	Factory.OuterTypeId = ActorType.StableId;
	Factory.Ownership = EAvidScriptObjectOwnershipPolicy::Session;
	Factory.Registration = EAvidScriptComponentRegistrationPolicy::RegisterInstance;
	Factory.StableId = FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
		Factory.ClassReferenceId,
		Factory.Kind,
		Factory.OuterTypeId,
		Factory.Ownership,
		Factory.Registration);
	Model.ObjectFactories = { Factory };
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
		WriteObjectFactoryBenchmarkType(Writer, Type);
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("class_references"));
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stable_id"), Reference.StableId);
	Writer->WriteValue(TEXT("ordinal"), Reference.Ordinal);
	Writer->WriteValue(TEXT("script_name"), Reference.ScriptName);
	Writer->WriteValue(TEXT("class_path"), Reference.ClassPath);
	Writer->WriteValue(TEXT("base_class_path"), Reference.BaseClassPath);
	Writer->WriteValue(TEXT("load_policy"), Reference.LoadPolicy);
	Writer->WriteValue(TEXT("result_type_id"), Reference.ResultTypeId);
	Writer->WriteObjectEnd();
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("object_factories"));
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stable_id"), Factory.StableId);
	Writer->WriteValue(TEXT("ordinal"), Factory.Ordinal);
	Writer->WriteValue(TEXT("script_name"), Factory.ScriptName);
	Writer->WriteValue(TEXT("class_reference_id"), Factory.ClassReferenceId);
	Writer->WriteValue(TEXT("kind"), LexToString(Factory.Kind));
	Writer->WriteValue(TEXT("outer_type_id"), Factory.OuterTypeId);
	Writer->WriteValue(TEXT("ownership"), LexToString(Factory.Ownership));
	Writer->WriteValue(TEXT("registration"), LexToString(Factory.Registration));
	Writer->WriteObjectEnd();
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("bindings"));
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	if (!Writer->Close())
	{
		return false;
	}
	return FAvidScriptBindingPackage::LoadDescriptor(
		DescriptorJson,
		OutPackage,
		OutLoadResult);
}

uint32 FindObjectFactoryBenchmarkOrdinal(
	const FAvidScriptBindingPackage& Package,
	const EAvidScriptBindingInvocationKind Kind)
{
	for (const FAvidScriptObjectFactoryBindingSpec& Spec :
		FAvidScriptObjectFactoryBinding::GetSpecs())
	{
		if (Spec.Kind != Kind)
		{
			continue;
		}
		const FAvidScriptVmDynamicImport* Import =
			Package.GetVmPackage().Imports.FindByPredicate(
				[&Spec](const FAvidScriptVmDynamicImport& Candidate)
				{
					return Candidate.StableId == Spec.StableId;
				});
		return Import != nullptr ? Import->Ordinal : MAX_uint32;
	}
	for (const FAvidScriptSceneAttachmentBindingSpec& Spec :
		FAvidScriptSceneAttachmentBinding::GetSpecs())
	{
		if (Spec.Kind != Kind)
		{
			continue;
		}
		const FAvidScriptVmDynamicImport* Import =
			Package.GetVmPackage().Imports.FindByPredicate(
				[&Spec](const FAvidScriptVmDynamicImport& Candidate)
				{
					return Candidate.StableId == Spec.StableId;
				});
		return Import != nullptr ? Import->Ordinal : MAX_uint32;
	}
	return MAX_uint32;
}

bool DispatchObjectFactoryBenchmarkCall(
	const FAvidScriptBindingPackage& Package,
	const uint32 Ordinal,
	const TConstArrayView<uint64> Arguments,
	const FAvidScriptBindingInvocationContext& Context,
	TArray<uint8>& Scratch,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Ordinal;
	Call.Arguments = Arguments;
	return Package.Dispatch(Call, Context, Scratch, OutResult);
}

FAvidScriptObjectHandle UnpackObjectFactoryBenchmarkHandle(const int64 Packed)
{
	const uint64 Value = static_cast<uint64>(Packed);
	return {
		static_cast<uint32>(Value & MAX_uint32),
		static_cast<uint32>(Value >> 32)
	};
}

void AppendObjectFactoryU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendObjectFactoryI32Leb(TArray<uint8>& Bytes, int32 Value)
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

void AppendObjectFactoryString(TArray<uint8>& Bytes, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	AppendObjectFactoryU32Leb(Bytes, static_cast<uint32>(Utf8.Length()));
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void AppendObjectFactorySection(
	TArray<uint8>& Module,
	const uint8 SectionId,
	const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendObjectFactoryU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendObjectFactoryI32Const(TArray<uint8>& Body, const int32 Value)
{
	Body.Add(0x41);
	AppendObjectFactoryI32Leb(Body, Value);
}

void AppendObjectFactoryHandleCell(
	TArray<uint8>& Body,
	const uint32 LocalIndex,
	const bool bGeneration)
{
	Body.Add(0x20);
	AppendObjectFactoryU32Leb(Body, LocalIndex);
	if (bGeneration)
	{
		Body.Add(0x42);
		Body.Add(32);
		Body.Add(0x88);
	}
	Body.Add(0xa7);
}

TArray<uint8> BuildObjectFactoryWasmBenchmarkModule(
	const FAvidScriptObjectFactoryBindingSpec& ConstructSpec,
	const FAvidScriptObjectFactoryBindingSpec& FindSpec,
	const FAvidScriptSceneAttachmentBindingSpec& AttachSpec,
	const FAvidScriptObjectFactoryBindingSpec& ReleaseSpec,
	const FAvidScriptObjectHandle& OwnerHandle,
	const FAvidScriptObjectHandle& RootHandle)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendObjectFactoryU32Leb(Types, 5);
	Types.Add(0x60);
	AppendObjectFactoryU32Leb(Types, 3);
	Types.Add(0x7f);
	Types.Add(0x7f);
	Types.Add(0x7f);
	AppendObjectFactoryU32Leb(Types, 1);
	Types.Add(0x7e);
	Types.Add(0x60);
	AppendObjectFactoryU32Leb(Types, 5);
	Types.Add(0x7f);
	Types.Add(0x7f);
	Types.Add(0x7f);
	Types.Add(0x7f);
	Types.Add(0x7f);
	AppendObjectFactoryU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendObjectFactoryU32Leb(Types, 2);
	Types.Add(0x7f);
	Types.Add(0x7f);
	AppendObjectFactoryU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendObjectFactoryU32Leb(Types, 0);
	AppendObjectFactoryU32Leb(Types, 0);
	Types.Add(0x60);
	AppendObjectFactoryU32Leb(Types, 1);
	Types.Add(0x7d);
	AppendObjectFactoryU32Leb(Types, 0);
	AppendObjectFactorySection(Module, 1, Types);

	TArray<uint8> TypedImports;
	AppendObjectFactoryU32Leb(TypedImports, AvidScriptObjectFactoryImportCount);
	const auto AppendImport = [&TypedImports](
		const FString& ModuleName,
		const FString& ImportName,
		const uint32 TypeIndex)
	{
		AppendObjectFactoryString(TypedImports, ModuleName);
		AppendObjectFactoryString(TypedImports, ImportName);
		TypedImports.Add(0x00);
		AppendObjectFactoryU32Leb(TypedImports, TypeIndex);
	};
	AppendImport(ConstructSpec.ModuleName, ConstructSpec.ImportName, 0);
	AppendImport(FindSpec.ModuleName, FindSpec.ImportName, 0);
	AppendImport(AttachSpec.ModuleName, AttachSpec.ImportName, 1);
	AppendImport(ReleaseSpec.ModuleName, ReleaseSpec.ImportName, 2);
	AppendObjectFactorySection(Module, 2, TypedImports);

	TArray<uint8> Functions;
	AppendObjectFactoryU32Leb(Functions, 2);
	AppendObjectFactoryU32Leb(Functions, 3);
	AppendObjectFactoryU32Leb(Functions, 4);
	AppendObjectFactorySection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendObjectFactoryU32Leb(Exports, 2);
	AppendObjectFactoryString(Exports, TEXT("avid_on_begin_play"));
	Exports.Add(0x00);
	AppendObjectFactoryU32Leb(Exports, 4);
	AppendObjectFactoryString(Exports, TEXT("avid_on_tick"));
	Exports.Add(0x00);
	AppendObjectFactoryU32Leb(Exports, 5);
	AppendObjectFactorySection(Module, 7, Exports);

	TArray<uint8> BeginBody;
	AppendObjectFactoryU32Leb(BeginBody, 0);
	BeginBody.Add(0x0b);

	TArray<uint8> TickBody;
	AppendObjectFactoryU32Leb(TickBody, 1);
	AppendObjectFactoryU32Leb(TickBody, 1);
	TickBody.Add(0x7e);
	AppendObjectFactoryI32Const(TickBody, 0);
	AppendObjectFactoryI32Const(TickBody, static_cast<int32>(OwnerHandle.Slot));
	AppendObjectFactoryI32Const(TickBody, static_cast<int32>(OwnerHandle.Generation));
	TickBody.Add(0x10);
	AppendObjectFactoryU32Leb(TickBody, 0);
	TickBody.Add(0x21);
	AppendObjectFactoryU32Leb(TickBody, 1);
	AppendObjectFactoryI32Const(TickBody, static_cast<int32>(OwnerHandle.Slot));
	AppendObjectFactoryI32Const(TickBody, static_cast<int32>(OwnerHandle.Generation));
	AppendObjectFactoryI32Const(TickBody, AvidScriptObjectFactoryTypeOrdinal);
	TickBody.Add(0x10);
	AppendObjectFactoryU32Leb(TickBody, 1);
	TickBody.Add(0x1a);
	AppendObjectFactoryHandleCell(TickBody, 1, false);
	AppendObjectFactoryHandleCell(TickBody, 1, true);
	AppendObjectFactoryI32Const(TickBody, static_cast<int32>(RootHandle.Slot));
	AppendObjectFactoryI32Const(TickBody, static_cast<int32>(RootHandle.Generation));
	AppendObjectFactoryI32Const(TickBody, 0);
	TickBody.Add(0x10);
	AppendObjectFactoryU32Leb(TickBody, 2);
	TickBody.Add(0x1a);
	AppendObjectFactoryHandleCell(TickBody, 1, false);
	AppendObjectFactoryHandleCell(TickBody, 1, true);
	TickBody.Add(0x10);
	AppendObjectFactoryU32Leb(TickBody, 3);
	TickBody.Add(0x1a);
	TickBody.Add(0x0b);

	TArray<uint8> Code;
	AppendObjectFactoryU32Leb(Code, 2);
	AppendObjectFactoryU32Leb(Code, static_cast<uint32>(BeginBody.Num()));
	Code.Append(BeginBody);
	AppendObjectFactoryU32Leb(Code, static_cast<uint32>(TickBody.Num()));
	Code.Append(TickBody);
	AppendObjectFactorySection(Module, 10, Code);
	return Module;
}

bool RunObjectFactoryWasmBenchmark(
	const TSharedPtr<const FAvidScriptBindingPackage>& Package,
	UWorld& World,
	AActor& Owner,
	USceneComponent& Root,
	const int32 WarmupCount,
	const int32 SampleCount,
	const int32 IterationsPerSample,
	TArray<double>& OutSamples,
	int32& OutObservedImports,
	FString& OutError)
{
	const FAvidScriptObjectFactoryBindingSpec* ConstructSpec = nullptr;
	const FAvidScriptObjectFactoryBindingSpec* FindSpec = nullptr;
	const FAvidScriptObjectFactoryBindingSpec* ReleaseSpec = nullptr;
	for (const FAvidScriptObjectFactoryBindingSpec& Spec :
		FAvidScriptObjectFactoryBinding::GetSpecs())
	{
		switch (Spec.Kind)
		{
		case EAvidScriptBindingInvocationKind::ObjectConstruct:
			ConstructSpec = &Spec;
			break;
		case EAvidScriptBindingInvocationKind::ActorFindComponent:
			FindSpec = &Spec;
			break;
		case EAvidScriptBindingInvocationKind::ObjectRelease:
			ReleaseSpec = &Spec;
			break;
		default:
			break;
		}
	}
	const FAvidScriptSceneAttachmentBindingSpec* AttachSpec = nullptr;
	for (const FAvidScriptSceneAttachmentBindingSpec& Spec :
		FAvidScriptSceneAttachmentBinding::GetSpecs())
	{
		if (Spec.Kind == EAvidScriptBindingInvocationKind::SceneComponentAttach)
		{
			AttachSpec = &Spec;
			break;
		}
	}
	if (ConstructSpec == nullptr || FindSpec == nullptr
		|| AttachSpec == nullptr || ReleaseSpec == nullptr)
	{
		OutError = TEXT("The object factory WAMR benchmark could not resolve all four imports.");
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptSessionObjectOwnership Ownership;
	FAvidScriptObjectHandleResult HandleResult;
	const FAvidScriptObjectHandle OwnerHandle = Registry.RegisterObject(
		&Owner,
		HandleResult,
		false);
	const FAvidScriptObjectHandle RootHandle = Registry.RegisterObject(
		&Root,
		HandleResult,
		false);
	if (!OwnerHandle.IsValid() || !RootHandle.IsValid())
	{
		OutError = TEXT("The object factory WAMR benchmark could not register owner fixtures.");
		return false;
	}

	const TArray<uint8> Module = BuildObjectFactoryWasmBenchmarkModule(
		*ConstructSpec,
		*FindSpec,
		*AttachSpec,
		*ReleaseSpec,
		OwnerHandle,
		RootHandle);
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ObjectOwnership = &Ownership;
	HostContext.OwnerHandle = OwnerHandle;
	HostContext.World = &World;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult SmokeResult;
	if (!Runtime.LoadModule(
			Module.GetData(),
			Module.Num(),
			TEXT("benchmark_object_factory_wamr"),
			Package,
			SmokeResult)
		|| !Runtime.BeginPlay(SmokeResult))
	{
		OutError = SmokeResult.ErrorMessage;
		return false;
	}

	const int32 TotalRuns = WarmupCount + SampleCount;
	OutSamples.Reserve(SampleCount);
	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < IterationsPerSample; ++Index)
		{
			if (!Runtime.Tick(0.0f, SmokeResult))
			{
				OutError = SmokeResult.ErrorMessage;
				return false;
			}
		}
		if (RunIndex >= WarmupCount)
		{
			OutSamples.Add(MeasureObjectFactoryPerIterationMs(
				StartSeconds,
				IterationsPerSample));
		}
		if (Registry.GetLiveHandleCount() != 2)
		{
			OutError = FString::Printf(
				TEXT("The WAMR component cycle leaked registry handles | live=%d"),
				Registry.GetLiveHandleCount());
			return false;
		}
	}
	OutObservedImports = SmokeResult.HostImportCallCount;
	const int32 ExpectedImports = TotalRuns
		* IterationsPerSample
		* AvidScriptObjectFactoryImportCount;
	if (OutObservedImports != ExpectedImports)
	{
		OutError = FString::Printf(
			TEXT("The WAMR import count is not exact | expected=%d | actual=%d"),
			ExpectedImports,
			OutObservedImports);
		return false;
	}
	Runtime.Unload();
	Ownership.Cleanup(Registry);
	if (!Registry.ReleaseHandle(RootHandle, HandleResult, false)
		|| !Registry.ReleaseHandle(OwnerHandle, HandleResult, false)
		|| Registry.GetLiveHandleCount() != 0)
	{
		OutError = TEXT("The object factory WAMR fixture did not clean up its root handles.");
		return false;
	}
	return true;
}
} // namespace

bool FAvidScriptRuntimeBenchmark::RunObjectFactoryBenchmark(
	const FAvidScriptObjectFactoryBenchmarkOptions& Options,
	FAvidScriptObjectFactoryBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptObjectFactoryBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);
	OutResult.IterationsPerSample = FMath::Max(Options.IterationsPerSample, 1);
	OutResult.ComponentCount = FMath::Max(Options.ComponentCount, 2);
	OutResult.ConstructImportsPerWasmIteration = 1;
	OutResult.FindImportsPerWasmIteration = 1;
	OutResult.AttachImportsPerWasmIteration = 1;
	OutResult.ReleaseImportsPerWasmIteration = 1;

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!MakeObjectFactoryBenchmarkPackage(Package, LoadResult) || !Package.IsValid())
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			LoadResult.ErrorCategory.IsEmpty()
				? FString(TEXT("package_load_failed"))
				: LoadResult.ErrorCategory,
			LoadResult.ErrorDetails);
		return false;
	}
	const FAvidScriptObjectFactoryPlan* FactoryPlan = nullptr;
	if (!Package->TryResolveObjectFactory(0, FactoryPlan)
		|| FactoryPlan == nullptr
		|| FactoryPlan->ObjectClass != UStaticMeshComponent::StaticClass())
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("factory_plan_missing"),
			TEXT("Factory ordinal zero did not resolve the warmed StaticMeshComponent plan."));
		return false;
	}

	const uint32 ConstructOrdinal = FindObjectFactoryBenchmarkOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::ObjectConstruct);
	const uint32 FindOrdinal = FindObjectFactoryBenchmarkOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::ActorFindComponent);
	const uint32 AttachOrdinal = FindObjectFactoryBenchmarkOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::SceneComponentAttach);
	const uint32 ReleaseOrdinal = FindObjectFactoryBenchmarkOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::ObjectRelease);
	if (ConstructOrdinal == MAX_uint32 || FindOrdinal == MAX_uint32
		|| AttachOrdinal == MAX_uint32 || ReleaseOrdinal == MAX_uint32)
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("benchmark_import_missing"),
			TEXT("The warmed package did not publish Construct, Find, Attach, and Release."));
		return false;
	}

	UWorld* World = nullptr;
	if (!CreateObjectFactoryBenchmarkWorld(World))
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("world_create_failed"),
			TEXT("The benchmark requires an initialized editor or commandlet world context."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyObjectFactoryBenchmarkWorld(World);
	};

	AActor* Owner = World->SpawnActor<AActor>();
	USceneComponent* Root = Owner != nullptr
		? CreateObjectFactoryBenchmarkComponent<USceneComponent>(*Owner)
		: nullptr;
	if (Owner == nullptr || Root == nullptr)
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("component_fixture_failed"),
			TEXT("The benchmark could not create its Actor and root component fixtures."));
		return false;
	}
	Owner->SetRootComponent(Root);
	for (int32 Index = 0; Index < OutResult.ComponentCount - 2; ++Index)
	{
		if (CreateObjectFactoryBenchmarkComponent<USceneComponent>(*Owner) == nullptr)
		{
			SetObjectFactoryBenchmarkFailure(
				OutResult,
				TEXT("component_fixture_failed"),
				TEXT("The benchmark could not create the requested component population."));
			return false;
		}
	}

	TArray<double> WasmSamples;
	FString WasmError;
	if (!RunObjectFactoryWasmBenchmark(
			Package,
			*World,
			*Owner,
			*Root,
			OutResult.WarmupCount,
			OutResult.SampleCount,
			OutResult.IterationsPerSample,
			WasmSamples,
			OutResult.WasmImportsObserved,
			WasmError))
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("wasm_component_cycle_failed"),
			WasmError);
		return false;
	}

	UStaticMeshComponent* FindTarget =
		CreateObjectFactoryBenchmarkComponent<UStaticMeshComponent>(*Owner);
	if (FindTarget == nullptr)
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("find_fixture_failed"),
			TEXT("The benchmark could not create its persistent find target."));
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptSessionObjectOwnership Ownership;
	FAvidScriptObjectHandleResult HandleResult;
	const FAvidScriptObjectHandle OwnerHandle = Registry.RegisterObject(
		Owner,
		HandleResult,
		false);
	const FAvidScriptObjectHandle RootHandle = Registry.RegisterObject(
		Root,
		HandleResult,
		false);
	const FAvidScriptObjectHandle FindTargetHandle = Registry.RegisterObject(
		FindTarget,
		HandleResult,
		false);
	if (!OwnerHandle.IsValid() || !RootHandle.IsValid() || !FindTargetHandle.IsValid())
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("registry_fixture_failed"),
			TEXT("The benchmark could not register its persistent object handles."));
		return false;
	}

	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.ObjectOwnership = &Ownership;
	Context.OwnerHandle = OwnerHandle;
	Context.World = World;
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> Scratch;
	Scratch.SetNumZeroed(Package->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult CallResult;
	const FAvidScriptBindingPackageInstrumentation InstrumentationBeforeWarmLoop =
		Package->GetInstrumentation();
	OutResult.BindingPackageClassLoadsDuringLoad = static_cast<int32>(
		FMath::Min<uint64>(InstrumentationBeforeWarmLoop.ClassLoadCount, MAX_int32));
	OutResult.BindingPackageReflectedNameLookupsDuringLoad = static_cast<int32>(
		FMath::Min<uint64>(
			InstrumentationBeforeWarmLoop.ReflectedNameLookupCount,
			MAX_int32));

	TArray<double> NativeConstructSamples;
	TArray<double> BindingConstructSamples;
	TArray<double> NativeFindSamples;
	TArray<double> BindingFindSamples;
	TArray<double> NativeAttachSamples;
	TArray<double> BindingAttachSamples;
	TArray<double> NativeReleaseSamples;
	TArray<double> BindingReleaseSamples;
	TArray<double> FactoryResolveSamples;
	TArray<double> RegistryResolveSamples;
	for (TArray<double>* Samples : {
		&NativeConstructSamples,
		&BindingConstructSamples,
		&NativeFindSamples,
		&BindingFindSamples,
		&NativeAttachSamples,
		&BindingAttachSamples,
		&NativeReleaseSamples,
		&BindingReleaseSamples,
		&FactoryResolveSamples,
		&RegistryResolveSamples })
	{
		Samples->Reserve(OutResult.SampleCount);
	}

	const int32 BaselineLiveHandles = Registry.GetLiveHandleCount();
	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		const bool bRecord = RunIndex >= OutResult.WarmupCount;

		const double FactoryResolveStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
		{
			const FAvidScriptObjectFactoryPlan* ResolvedPlan = nullptr;
			if (!Package->TryResolveObjectFactory(0, ResolvedPlan)
				|| ResolvedPlan != FactoryPlan)
			{
				SetObjectFactoryBenchmarkFailure(
					OutResult,
					TEXT("factory_resolve_failed"),
					TEXT("A warmed factory ordinal changed during the timed loop."));
				return false;
			}
		}
		const double FactoryResolveMs = MeasureObjectFactoryPerIterationMs(
			FactoryResolveStart,
			OutResult.IterationsPerSample);

		const double RegistryResolveStart = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
		{
			if (Registry.ResolveObject<UStaticMeshComponent>(
					FindTargetHandle,
					HandleResult,
					false) != FindTarget
				|| !HandleResult.bSucceeded
				|| !HandleResult.ObjectPath.IsEmpty())
			{
				SetObjectFactoryBenchmarkFailure(
					OutResult,
					TEXT("registry_resolve_failed"),
					TEXT("A warmed component handle failed zero-diagnostic resolution."));
				return false;
			}
		}
		const double RegistryResolveMs = MeasureObjectFactoryPerIterationMs(
			RegistryResolveStart,
			OutResult.IterationsPerSample);

		auto MeasureNativeConstruct = [&]() -> bool
		{
			TArray<UStaticMeshComponent*> Components;
			Components.SetNumUninitialized(OutResult.IterationsPerSample);
			const double StartSeconds = FPlatformTime::Seconds();
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				Components[Index] =
					CreateObjectFactoryBenchmarkComponent<UStaticMeshComponent>(*Owner);
				if (Components[Index] == nullptr)
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("native_construct_failed"),
						TEXT("Native component construction failed in the timed loop."));
					return false;
				}
			}
			const double SampleMs = MeasureObjectFactoryPerIterationMs(
				StartSeconds,
				OutResult.IterationsPerSample);
			for (UStaticMeshComponent* Component : Components)
			{
				Component->DestroyComponent();
			}
			if (bRecord)
			{
				NativeConstructSamples.Add(SampleMs);
			}
			return true;
		};

		auto MeasureBindingConstruct = [&]() -> bool
		{
			TArray<FAvidScriptObjectHandle> Handles;
			Handles.SetNumUninitialized(OutResult.IterationsPerSample);
			const double StartSeconds = FPlatformTime::Seconds();
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				const uint64 Arguments[] = { 0, OwnerHandle.Slot, OwnerHandle.Generation };
				if (!DispatchObjectFactoryBenchmarkCall(
						*Package,
						ConstructOrdinal,
						Arguments,
						Context,
						Scratch,
						CallResult))
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("binding_construct_failed"),
						CallResult.Details);
					return false;
				}
				Handles[Index] = UnpackObjectFactoryBenchmarkHandle(
					CallResult.ReturnValueI64);
			}
			const double SampleMs = MeasureObjectFactoryPerIterationMs(
				StartSeconds,
				OutResult.IterationsPerSample);
			for (const FAvidScriptObjectHandle& Handle : Handles)
			{
				const uint64 Arguments[] = { Handle.Slot, Handle.Generation };
				if (!DispatchObjectFactoryBenchmarkCall(
						*Package,
						ReleaseOrdinal,
						Arguments,
						Context,
						Scratch,
						CallResult))
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("binding_construct_cleanup_failed"),
						CallResult.Details);
					return false;
				}
			}
			if (bRecord)
			{
				BindingConstructSamples.Add(SampleMs);
			}
			return true;
		};

		auto MeasureNativeFind = [&]() -> bool
		{
			const double StartSeconds = FPlatformTime::Seconds();
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				if (Owner->FindComponentByClass<UStaticMeshComponent>() != FindTarget)
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("native_find_failed"),
						TEXT("Native FindComponentByClass returned an unexpected component."));
					return false;
				}
			}
			if (bRecord)
			{
				NativeFindSamples.Add(MeasureObjectFactoryPerIterationMs(
					StartSeconds,
					OutResult.IterationsPerSample));
			}
			return true;
		};

		auto MeasureBindingFind = [&]() -> bool
		{
			const double StartSeconds = FPlatformTime::Seconds();
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				const uint64 Arguments[] = {
					OwnerHandle.Slot,
					OwnerHandle.Generation,
					AvidScriptObjectFactoryTypeOrdinal
				};
				if (!DispatchObjectFactoryBenchmarkCall(
						*Package,
						FindOrdinal,
						Arguments,
						Context,
						Scratch,
						CallResult)
					|| UnpackObjectFactoryBenchmarkHandle(CallResult.ReturnValueI64)
						!= FindTargetHandle)
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("binding_find_failed"),
						CallResult.Details);
					return false;
				}
			}
			if (bRecord)
			{
				BindingFindSamples.Add(MeasureObjectFactoryPerIterationMs(
					StartSeconds,
					OutResult.IterationsPerSample));
			}
			return true;
		};

		auto MeasureNativeAttach = [&]() -> bool
		{
			TArray<USceneComponent*> Components;
			Components.Reserve(OutResult.IterationsPerSample);
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				Components.Add(CreateObjectFactoryBenchmarkComponent<USceneComponent>(*Owner));
			}
			const double StartSeconds = FPlatformTime::Seconds();
			for (USceneComponent* Component : Components)
			{
				if (Component == nullptr
					|| !Component->AttachToComponent(
						Root,
						FAttachmentTransformRules::KeepRelativeTransform))
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("native_attach_failed"),
						TEXT("Native AttachToComponent failed in the timed loop."));
					return false;
				}
			}
			const double SampleMs = MeasureObjectFactoryPerIterationMs(
				StartSeconds,
				OutResult.IterationsPerSample);
			for (USceneComponent* Component : Components)
			{
				Component->DestroyComponent();
			}
			if (bRecord)
			{
				NativeAttachSamples.Add(SampleMs);
			}
			return true;
		};

		auto MeasureBindingAttach = [&]() -> bool
		{
			TArray<USceneComponent*> Components;
			TArray<FAvidScriptObjectHandle> Handles;
			Components.Reserve(OutResult.IterationsPerSample);
			Handles.Reserve(OutResult.IterationsPerSample);
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				USceneComponent* Component =
					CreateObjectFactoryBenchmarkComponent<USceneComponent>(*Owner);
				Components.Add(Component);
				Handles.Add(Registry.RegisterObject(Component, HandleResult, false));
			}
			const double StartSeconds = FPlatformTime::Seconds();
			for (const FAvidScriptObjectHandle& Handle : Handles)
			{
				const uint64 Arguments[] = {
					Handle.Slot,
					Handle.Generation,
					RootHandle.Slot,
					RootHandle.Generation,
					FAvidScriptSceneAttachmentRules::EncodeAttach(
						EAvidScriptSceneAttachmentRule::KeepRelative,
						false)
				};
				if (!Handle.IsValid()
					|| !DispatchObjectFactoryBenchmarkCall(
						*Package,
						AttachOrdinal,
						Arguments,
						Context,
						Scratch,
						CallResult))
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("binding_attach_failed"),
						CallResult.Details);
					return false;
				}
			}
			const double SampleMs = MeasureObjectFactoryPerIterationMs(
				StartSeconds,
				OutResult.IterationsPerSample);
			for (int32 Index = 0; Index < Components.Num(); ++Index)
			{
				Components[Index]->DestroyComponent();
				if (!Registry.ReleaseHandle(Handles[Index], HandleResult, false))
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("binding_attach_cleanup_failed"),
						HandleResult.ErrorMessage);
					return false;
				}
			}
			if (bRecord)
			{
				BindingAttachSamples.Add(SampleMs);
			}
			return true;
		};

		auto MeasureNativeRelease = [&]() -> bool
		{
			TArray<UStaticMeshComponent*> Components;
			Components.Reserve(OutResult.IterationsPerSample);
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				Components.Add(
					CreateObjectFactoryBenchmarkComponent<UStaticMeshComponent>(*Owner));
			}
			const double StartSeconds = FPlatformTime::Seconds();
			for (UStaticMeshComponent* Component : Components)
			{
				if (Component == nullptr)
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("native_release_setup_failed"),
						TEXT("Native release setup produced a null component."));
					return false;
				}
				Component->DestroyComponent();
			}
			if (bRecord)
			{
				NativeReleaseSamples.Add(MeasureObjectFactoryPerIterationMs(
					StartSeconds,
					OutResult.IterationsPerSample));
			}
			return true;
		};

		auto MeasureBindingRelease = [&]() -> bool
		{
			TArray<FAvidScriptObjectHandle> Handles;
			Handles.Reserve(OutResult.IterationsPerSample);
			for (int32 Index = 0; Index < OutResult.IterationsPerSample; ++Index)
			{
				const uint64 Arguments[] = { 0, OwnerHandle.Slot, OwnerHandle.Generation };
				if (!DispatchObjectFactoryBenchmarkCall(
						*Package,
						ConstructOrdinal,
						Arguments,
						Context,
						Scratch,
						CallResult))
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("binding_release_setup_failed"),
						CallResult.Details);
					return false;
				}
				Handles.Add(UnpackObjectFactoryBenchmarkHandle(CallResult.ReturnValueI64));
			}
			const double StartSeconds = FPlatformTime::Seconds();
			for (const FAvidScriptObjectHandle& Handle : Handles)
			{
				const uint64 Arguments[] = { Handle.Slot, Handle.Generation };
				if (!DispatchObjectFactoryBenchmarkCall(
						*Package,
						ReleaseOrdinal,
						Arguments,
						Context,
						Scratch,
						CallResult))
				{
					SetObjectFactoryBenchmarkFailure(
						OutResult,
						TEXT("binding_release_failed"),
						CallResult.Details);
					return false;
				}
			}
			if (bRecord)
			{
				BindingReleaseSamples.Add(MeasureObjectFactoryPerIterationMs(
					StartSeconds,
					OutResult.IterationsPerSample));
			}
			return true;
		};

		const bool bNativeFirst = (RunIndex & 1) == 0;
		if (bNativeFirst)
		{
			if (!MeasureNativeConstruct() || !MeasureBindingConstruct()
				|| !MeasureNativeFind() || !MeasureBindingFind()
				|| !MeasureNativeAttach() || !MeasureBindingAttach()
				|| !MeasureNativeRelease() || !MeasureBindingRelease())
			{
				return false;
			}
		}
		else if (!MeasureBindingConstruct() || !MeasureNativeConstruct()
			|| !MeasureBindingFind() || !MeasureNativeFind()
			|| !MeasureBindingAttach() || !MeasureNativeAttach()
			|| !MeasureBindingRelease() || !MeasureNativeRelease())
		{
			return false;
		}

		if (Registry.GetLiveHandleCount() != BaselineLiveHandles)
		{
			SetObjectFactoryBenchmarkFailure(
				OutResult,
				TEXT("registry_leak_detected"),
				TEXT("A benchmark run did not return to its persistent handle baseline."));
			return false;
		}
		if (bRecord)
		{
			FactoryResolveSamples.Add(FactoryResolveMs);
			RegistryResolveSamples.Add(RegistryResolveMs);
		}
	}

	OutResult.NativeConstructComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(NativeConstructSamples));
	OutResult.BindingConstructComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(BindingConstructSamples));
	OutResult.NativeFindComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(NativeFindSamples));
	OutResult.BindingFindComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(BindingFindSamples));
	OutResult.NativeAttachComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(NativeAttachSamples));
	OutResult.BindingAttachComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(BindingAttachSamples));
	OutResult.NativeReleaseComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(NativeReleaseSamples));
	OutResult.BindingReleaseComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(BindingReleaseSamples));
	OutResult.WasmComponentCycle =
		CalculateAvidScriptBenchmarkStats(MoveTemp(WasmSamples));
	OutResult.FactoryOrdinalResolve =
		CalculateAvidScriptBenchmarkStats(MoveTemp(FactoryResolveSamples));
	OutResult.RegistryResolveComponent =
		CalculateAvidScriptBenchmarkStats(MoveTemp(RegistryResolveSamples));

	const FAvidScriptBindingPackageInstrumentation InstrumentationAfterWarmLoop =
		Package->GetInstrumentation();
	const uint64 WarmClassLoads = InstrumentationAfterWarmLoop.ClassLoadCount
		- InstrumentationBeforeWarmLoop.ClassLoadCount;
	const uint64 WarmNameLookups = InstrumentationAfterWarmLoop.ReflectedNameLookupCount
		- InstrumentationBeforeWarmLoop.ReflectedNameLookupCount;
	if (WarmClassLoads > MAX_int32 || WarmNameLookups > MAX_int32)
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("instrumentation_overflow"),
			TEXT("Binding package instrumentation exceeded the benchmark result width."));
		return false;
	}
	OutResult.BindingPackageClassLoadsDuringWarmLoop = static_cast<int32>(WarmClassLoads);
	OutResult.BindingPackageReflectedNameLookupsDuringWarmLoop =
		static_cast<int32>(WarmNameLookups);

	FindTarget->DestroyComponent();
	if (!Registry.ReleaseHandle(FindTargetHandle, HandleResult, false)
		|| !Registry.ReleaseHandle(RootHandle, HandleResult, false)
		|| !Registry.ReleaseHandle(OwnerHandle, HandleResult, false))
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("fixture_cleanup_failed"),
			HandleResult.ErrorMessage);
		return false;
	}
	Ownership.Cleanup(Registry);
	if (Registry.GetLiveHandleCount() != 0)
	{
		SetObjectFactoryBenchmarkFailure(
			OutResult,
			TEXT("fixture_cleanup_failed"),
			TEXT("The final registry did not return to zero live handles."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("object_factory_benchmark | warmup=%d | samples=%d | iterations=%d | components=%d | native_construct_p50_ms=%.6f | native_construct_p95_ms=%.6f | binding_construct_p50_ms=%.6f | binding_construct_p95_ms=%.6f | native_find_p50_ms=%.6f | native_find_p95_ms=%.6f | binding_find_p50_ms=%.6f | binding_find_p95_ms=%.6f | native_attach_p50_ms=%.6f | native_attach_p95_ms=%.6f | binding_attach_p50_ms=%.6f | binding_attach_p95_ms=%.6f | native_release_p50_ms=%.6f | native_release_p95_ms=%.6f | binding_release_p50_ms=%.6f | binding_release_p95_ms=%.6f | wasm_cycle_p50_ms=%.6f | wasm_cycle_p95_ms=%.6f | factory_resolve_p50_ms=%.6f | registry_resolve_p50_ms=%.6f | binding_package_class_loads=%d | binding_package_reflected_name_lookups=%d | warm_binding_package_class_loads=%d | warm_binding_package_reflected_name_lookups=%d | wasm_imports_observed=%d | imports_per_wasm_iteration=%d"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.IterationsPerSample,
		OutResult.ComponentCount,
		OutResult.NativeConstructComponent.P50Ms,
		OutResult.NativeConstructComponent.P95Ms,
		OutResult.BindingConstructComponent.P50Ms,
		OutResult.BindingConstructComponent.P95Ms,
		OutResult.NativeFindComponent.P50Ms,
		OutResult.NativeFindComponent.P95Ms,
		OutResult.BindingFindComponent.P50Ms,
		OutResult.BindingFindComponent.P95Ms,
		OutResult.NativeAttachComponent.P50Ms,
		OutResult.NativeAttachComponent.P95Ms,
		OutResult.BindingAttachComponent.P50Ms,
		OutResult.BindingAttachComponent.P95Ms,
		OutResult.NativeReleaseComponent.P50Ms,
		OutResult.NativeReleaseComponent.P95Ms,
		OutResult.BindingReleaseComponent.P50Ms,
		OutResult.BindingReleaseComponent.P95Ms,
		OutResult.WasmComponentCycle.P50Ms,
		OutResult.WasmComponentCycle.P95Ms,
		OutResult.FactoryOrdinalResolve.P50Ms,
		OutResult.RegistryResolveComponent.P50Ms,
		OutResult.BindingPackageClassLoadsDuringLoad,
		OutResult.BindingPackageReflectedNameLookupsDuringLoad,
		OutResult.BindingPackageClassLoadsDuringWarmLoop,
		OutResult.BindingPackageReflectedNameLookupsDuringWarmLoop,
		OutResult.WasmImportsObserved,
		AvidScriptObjectFactoryImportCount);
	UE_LOG(LogAvidScriptObjectFactoryBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	return true;
}
