#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
class FAvidScriptLifecycleGuestMemory final : public IAvidScriptVmGuestMemory
{
public:
	explicit FAvidScriptLifecycleGuestMemory(const int32 Size)
	{
		Bytes.SetNumZeroed(Size);
	}

	bool ReadBytes(const uint32 GuestAddress, TArrayView<uint8> OutBytes, FString& OutError) override
	{
		if (!IsRangeValid(GuestAddress, OutBytes.Num()))
		{
			OutError = TEXT("lifecycle test guest read is out of bounds");
			return false;
		}
		FMemory::Memcpy(OutBytes.GetData(), Bytes.GetData() + GuestAddress, OutBytes.Num());
		return true;
	}

	bool WriteBytes(const uint32 GuestAddress, TConstArrayView<uint8> InBytes, FString& OutError) override
	{
		if (!IsRangeValid(GuestAddress, InBytes.Num()))
		{
			OutError = TEXT("lifecycle test guest write is out of bounds");
			return false;
		}
		FMemory::Memcpy(Bytes.GetData() + GuestAddress, InBytes.GetData(), InBytes.Num());
		return true;
	}

	template <typename ValueType>
	ValueType ReadValue(const uint32 GuestAddress) const
	{
		ValueType Value{};
		if (IsRangeValid(GuestAddress, sizeof(ValueType)))
		{
			FMemory::Memcpy(&Value, Bytes.GetData() + GuestAddress, sizeof(ValueType));
		}
		return Value;
	}

	template <typename ValueType>
	void WriteValue(const uint32 GuestAddress, const ValueType& Value)
	{
		if (IsRangeValid(GuestAddress, sizeof(ValueType)))
		{
			FMemory::Memcpy(Bytes.GetData() + GuestAddress, &Value, sizeof(ValueType));
		}
	}

private:
	bool IsRangeValid(const uint32 GuestAddress, const uint64 Size) const
	{
		return GuestAddress <= static_cast<uint64>(Bytes.Num())
			&& Size <= static_cast<uint64>(Bytes.Num()) - GuestAddress;
	}

	TArray<uint8> Bytes;
};

class FAvidScriptRejectingLifecycleJournal final : public IAvidScriptBindingHostEffectJournal
{
public:
	bool PrepareEffect(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		EAvidScriptBindingReloadEffect Effect,
		FAvidScriptBindingHostEffectPrepareResult& OutResult) override
	{
		OutResult.ErrorCategory = TEXT("unexpected_prepare");
		return false;
	}
};

bool CreateLifecycleWorld(const TCHAR* Name, UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, FName(Name));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyLifecycleWorld(UWorld*& World)
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

bool MakeLifecycleDescriptor(FString& OutJson)
{
	FAvidScriptBindingPackageModel Package;
	Package.SchemaVersion = 5;
	Package.GeneratorVersion = TEXT("49.3.test");
	Package.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = TEXT("avidscript.test.object_lifecycle");
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
	Package.ClassReferences.Add(ActorReference);
	Package.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
	Package.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);

	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), Package.SchemaVersion);
	Writer->WriteValue(TEXT("generator_version"), Package.GeneratorVersion);
	Writer->WriteValue(TEXT("engine_version"), Package.EngineVersion);
	Writer->WriteValue(TEXT("source"), Package.Source);
	Writer->WriteValue(TEXT("package_name"), Package.PackageName);
	Writer->WriteValue(TEXT("package_hash"), Package.PackageHash);
	Writer->WriteValue(TEXT("selection_hash"), Package.SelectionHash);
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
	return Writer->Close();
}

uint32 FindLifecycleOrdinal(
	const FAvidScriptBindingPackage& Package,
	const EAvidScriptBindingInvocationKind Kind)
{
	const TConstArrayView<FAvidScriptObjectLifecycleBindingSpec> Specs =
		FAvidScriptObjectLifecycleBindings::GetSpecs();
	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		if (Specs[Index].Kind == Kind)
		{
			const FAvidScriptVmDynamicImport* Import = Package.GetVmPackage().Imports.FindByPredicate(
				[&Specs, Index](const FAvidScriptVmDynamicImport& Candidate)
				{
					return Candidate.StableId == Specs[Index].StableId;
				});
			return Import != nullptr ? Import->Ordinal : MAX_uint32;
		}
	}
	return MAX_uint32;
}

bool DispatchLifecycle(
	const FAvidScriptBindingPackage& Package,
	const uint32 Ordinal,
	const TConstArrayView<uint64> Arguments,
	IAvidScriptVmGuestMemory* GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Ordinal;
	Call.Arguments = Arguments;
	Call.GuestMemory = GuestMemory;
	TArray<uint8> Scratch;
	return Package.Dispatch(Call, Context, Scratch, OutResult);
}

void WriteIdentityTransform(FAvidScriptLifecycleGuestMemory& GuestMemory, const uint32 Address)
{
	const float Components[9] = {
		100.0f, 200.0f, 300.0f,
		0.0f, 45.0f, 0.0f,
		1.0f, 1.0f, 1.0f
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Components); ++Index)
	{
		GuestMemory.WriteValue<float>(Address + Index * sizeof(float), Components[Index]);
	}
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectLifecycleBindingTest,
	"AvidScript.Runtime.Binding.ObjectLifecycleDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectLifecycleBindingTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	if (!TestTrue(TEXT("Class-only lifecycle descriptor serializes"), MakeLifecycleDescriptor(DescriptorJson)))
	{
		return false;
	}
	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
		TEXT("Class-only lifecycle package loads"),
		FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, Package, LoadResult))
		|| !Package.IsValid())
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}
	TestEqual(TEXT("Descriptor binding count remains reflected-only"), LoadResult.BindingCount, 0);
	TestEqual(
		TEXT("Runtime publishes one import per lifecycle capability"),
		Package->GetVmPackage().Imports.Num(),
		FAvidScriptObjectLifecycleBindings::GetSpecs().Num());

	const uint32 SpawnOrdinal = FindLifecycleOrdinal(*Package, EAvidScriptBindingInvocationKind::ObjectSpawnActor);
	const uint32 DestroyOrdinal = FindLifecycleOrdinal(*Package, EAvidScriptBindingInvocationKind::ObjectDestroyActor);
	const uint32 IsAOrdinal = FindLifecycleOrdinal(*Package, EAvidScriptBindingInvocationKind::ObjectIsA);
	if (!TestTrue(TEXT("Spawn lifecycle ordinal resolves"), SpawnOrdinal != MAX_uint32)
		|| !TestTrue(TEXT("Destroy lifecycle ordinal resolves"), DestroyOrdinal != MAX_uint32)
		|| !TestTrue(TEXT("IsA lifecycle ordinal resolves"), IsAOrdinal != MAX_uint32))
	{
		return false;
	}

	UWorld* World = nullptr;
	UWorld* OtherWorld = nullptr;
	if (!CreateLifecycleWorld(TEXT("AvidScriptLifecycleWorld"), World)
		|| !CreateLifecycleWorld(TEXT("AvidScriptLifecycleOtherWorld"), OtherWorld))
	{
		DestroyLifecycleWorld(OtherWorld);
		DestroyLifecycleWorld(World);
		AddError(TEXT("Failed to create object lifecycle test worlds."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyLifecycleWorld(OtherWorld);
		DestroyLifecycleWorld(World);
	};

	FAvidScriptObjectRegistry Registry;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.World = World;
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptLifecycleGuestMemory GuestMemory(128);
	constexpr uint32 TransformAddress = 16;
	constexpr uint32 HandleAddress = 64;
	WriteIdentityTransform(GuestMemory, TransformAddress);

	FAvidScriptDynamicHostCallResult Result;
	const uint64 SpawnArguments[] = { 0, TransformAddress, HandleAddress };
	if (!TestTrue(
		TEXT("SpawnActor creates and registers an Actor"),
		DispatchLifecycle(*Package, SpawnOrdinal, SpawnArguments, &GuestMemory, Context, Result)))
	{
		AddError(Result.Details);
		return false;
	}
	const FAvidScriptObjectHandle FirstHandle{
		GuestMemory.ReadValue<uint32>(HandleAddress),
		GuestMemory.ReadValue<uint32>(HandleAddress + sizeof(uint32))
	};
	TestTrue(TEXT("SpawnActor writes a valid generational handle"), FirstHandle.IsValid());
	FAvidScriptObjectHandleResult ResolveResult;
	AActor* FirstActor = Registry.ResolveObject<AActor>(FirstHandle, ResolveResult);
	TestTrue(TEXT("Spawned Actor resolves from the registry"), ResolveResult.bSucceeded && FirstActor != nullptr);
	TestTrue(TEXT("Spawned Actor belongs to the injected World"), FirstActor != nullptr && FirstActor->GetWorld() == World);

	const uint64 IsAArguments[] = { FirstHandle.Slot, FirstHandle.Generation, 0 };
	TestTrue(
		TEXT("IsA performs a cached class-plan check"),
		DispatchLifecycle(*Package, IsAOrdinal, IsAArguments, nullptr, Context, Result));
	TestEqual(TEXT("Spawned Actor is an AActor"), Result.ReturnValue, 1);
	const uint64 InvalidClassArguments[] = { FirstHandle.Slot, FirstHandle.Generation, 1 };
	TestFalse(
		TEXT("IsA rejects an out-of-range class ordinal"),
		DispatchLifecycle(*Package, IsAOrdinal, InvalidClassArguments, nullptr, Context, Result));
	TestTrue(TEXT("Class ordinal failure is categorized"), Result.Details.Contains(TEXT("binding_class_ordinal_invalid")));

	const int32 LiveCountBeforeFailure = Registry.GetLiveHandleCount();
	const uint64 InvalidTransformArguments[] = { 0, 112, HandleAddress };
	TestFalse(
		TEXT("SpawnActor rejects an out-of-bounds 36-byte transform"),
		DispatchLifecycle(*Package, SpawnOrdinal, InvalidTransformArguments, &GuestMemory, Context, Result));
	TestTrue(TEXT("Transform failure is categorized"), Result.Details.Contains(TEXT("binding_guest_read_failed")));
	TestEqual(TEXT("Transform failure leaves no handle"), Registry.GetLiveHandleCount(), LiveCountBeforeFailure);

	const uint64 InvalidOutputArguments[] = { 0, TransformAddress, 124 };
	TestFalse(
		TEXT("SpawnActor cleans up when the handle output is invalid"),
		DispatchLifecycle(*Package, SpawnOrdinal, InvalidOutputArguments, &GuestMemory, Context, Result));
	TestTrue(TEXT("Handle output failure is categorized"), Result.Details.Contains(TEXT("binding_guest_write_failed")));
	TestEqual(TEXT("Handle output failure leaves no registered Actor"), Registry.GetLiveHandleCount(), LiveCountBeforeFailure);

	FAvidScriptBindingInvocationContext InvalidWorldContext = Context;
	InvalidWorldContext.World.Reset();
	TestFalse(
		TEXT("SpawnActor rejects an invalid World before entering UE spawn"),
		DispatchLifecycle(*Package, SpawnOrdinal, SpawnArguments, &GuestMemory, InvalidWorldContext, Result));
	TestTrue(TEXT("Invalid World failure is categorized"), Result.Details.Contains(TEXT("binding_world_invalid")));

	FAvidScriptBindingInvocationContext ReadOnlyContext = Context;
	ReadOnlyContext.WritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
	TestFalse(
		TEXT("SpawnActor rejects a read-only host context"),
		DispatchLifecycle(*Package, SpawnOrdinal, SpawnArguments, &GuestMemory, ReadOnlyContext, Result));
	TestTrue(TEXT("Read-only lifecycle failure is categorized"), Result.Details.Contains(TEXT("binding_write_denied")));

	FAvidScriptRejectingLifecycleJournal Journal;
	FAvidScriptBindingInvocationContext CandidateContext = Context;
	CandidateContext.HostEffectJournal = &Journal;
	TestFalse(
		TEXT("Candidate reload rejects SpawnActor side effects"),
		DispatchLifecycle(*Package, SpawnOrdinal, SpawnArguments, &GuestMemory, CandidateContext, Result));
	TestTrue(TEXT("Spawn reload rejection is categorized"), Result.Details.Contains(TEXT("binding_reload_effect_unsupported")));
	TestFalse(
		TEXT("Candidate reload rejects DestroyActor side effects"),
		DispatchLifecycle(
			*Package,
			DestroyOrdinal,
			MakeArrayView(IsAArguments, 2),
			nullptr,
			CandidateContext,
			Result));
	TestTrue(TEXT("Destroy reload rejection is categorized"), Result.Details.Contains(TEXT("binding_reload_effect_unsupported")));
	TestTrue(
		TEXT("Candidate reload keeps read-only IsA available"),
		DispatchLifecycle(*Package, IsAOrdinal, IsAArguments, nullptr, CandidateContext, Result));

	FAvidScriptBindingInvocationContext OwnerContext = Context;
	OwnerContext.OwnerHandle = FirstHandle;
	const uint64 OwnerDestroyArguments[] = { FirstHandle.Slot, FirstHandle.Generation };
	TestFalse(
		TEXT("DestroyActor rejects the active Runtime owner"),
		DispatchLifecycle(*Package, DestroyOrdinal, OwnerDestroyArguments, nullptr, OwnerContext, Result));
	TestTrue(
		TEXT("Owner destruction requires deferred shutdown"),
		Result.Details.Contains(TEXT("binding_destroy_owner_unsupported")));

	AActor* OtherActor = OtherWorld->SpawnActor<AActor>();
	FAvidScriptObjectHandleResult OtherRegisterResult;
	const FAvidScriptObjectHandle OtherHandle = Registry.RegisterObject(OtherActor, OtherRegisterResult);
	const uint64 CrossWorldArguments[] = { OtherHandle.Slot, OtherHandle.Generation };
	TestFalse(
		TEXT("DestroyActor rejects a cross-world Actor"),
		DispatchLifecycle(*Package, DestroyOrdinal, CrossWorldArguments, nullptr, Context, Result));
	TestTrue(TEXT("Cross-world failure is categorized"), Result.Details.Contains(TEXT("binding_cross_world")));
	TestTrue(TEXT("Cross-world rejection preserves the Actor"), IsValid(OtherActor));
	FAvidScriptObjectHandleResult OtherReleaseResult;
	Registry.ReleaseHandle(OtherHandle, OtherReleaseResult);

	const uint64 DestroyArguments[] = { FirstHandle.Slot, FirstHandle.Generation };
	TestTrue(
		TEXT("DestroyActor destroys and releases a live Actor"),
		DispatchLifecycle(*Package, DestroyOrdinal, DestroyArguments, nullptr, Context, Result));
	TestFalse(
		TEXT("Double Destroy fails closed on the stale generation"),
		DispatchLifecycle(*Package, DestroyOrdinal, DestroyArguments, nullptr, Context, Result));
	TestTrue(TEXT("Double Destroy reports a stale target"), Result.Details.Contains(TEXT("binding_target_invalid")));

	constexpr uint32 SecondHandleAddress = 72;
	const uint64 SecondSpawnArguments[] = { 0, TransformAddress, SecondHandleAddress };
	TestTrue(
		TEXT("A later SpawnActor reuses the released slot safely"),
		DispatchLifecycle(*Package, SpawnOrdinal, SecondSpawnArguments, &GuestMemory, Context, Result));
	const FAvidScriptObjectHandle SecondHandle{
		GuestMemory.ReadValue<uint32>(SecondHandleAddress),
		GuestMemory.ReadValue<uint32>(SecondHandleAddress + sizeof(uint32))
	};
	TestEqual(TEXT("Lifecycle registry reuses the released slot"), SecondHandle.Slot, FirstHandle.Slot);
	TestNotEqual(TEXT("Lifecycle slot reuse advances generation"), SecondHandle.Generation, FirstHandle.Generation);
	TestFalse(
		TEXT("Old generation cannot destroy the reused slot"),
		DispatchLifecycle(*Package, DestroyOrdinal, DestroyArguments, nullptr, Context, Result));
	TestTrue(TEXT("Old generation reports mismatch"), Result.Details.Contains(TEXT("generation_mismatch")));
	FAvidScriptObjectHandleResult SecondResolveResult;
	TestNotNull(
		TEXT("Reused slot still resolves its new Actor"),
		Registry.ResolveObject<AActor>(SecondHandle, SecondResolveResult));
	const uint64 SecondDestroyArguments[] = { SecondHandle.Slot, SecondHandle.Generation };
	TestTrue(
		TEXT("DestroyActor accepts the current reused generation"),
		DispatchLifecycle(*Package, DestroyOrdinal, SecondDestroyArguments, nullptr, Context, Result));

	FAvidScriptSessionObjectOwnership Ownership;
	FAvidScriptBindingInvocationContext OwnedContext = Context;
	OwnedContext.ObjectOwnership = &Ownership;
	constexpr uint32 OwnedHandleAddress = 80;
	const uint64 OwnedSpawnArguments[] = { 0, TransformAddress, OwnedHandleAddress };
	TestTrue(
		TEXT("Session SpawnActor adopts the returned Actor capability"),
		DispatchLifecycle(
			*Package,
			SpawnOrdinal,
			OwnedSpawnArguments,
			&GuestMemory,
			OwnedContext,
			Result));
	const FAvidScriptObjectHandle OwnedHandle{
		GuestMemory.ReadValue<uint32>(OwnedHandleAddress),
		GuestMemory.ReadValue<uint32>(OwnedHandleAddress + sizeof(uint32))
	};
	AActor* OwnedActor = Registry.ResolveObject<AActor>(OwnedHandle, ResolveResult, false);
	TestTrue(
		TEXT("Session-owned Actor is an authorized receiver"),
		OwnedActor != nullptr && Ownership.HasCapability(OwnedHandle, OwnedActor));
	const uint64 OwnedIsAArguments[] = {
		OwnedHandle.Slot, OwnedHandle.Generation, 0
	};
	TestTrue(
		TEXT("Session-owned Actor supports capability-checked IsA"),
		DispatchLifecycle(
			*Package,
			IsAOrdinal,
			OwnedIsAArguments,
			nullptr,
			OwnedContext,
			Result));
	TestTrue(
		TEXT("Session-owned Actor releases through DestroyActor"),
		DispatchLifecycle(
			*Package,
			DestroyOrdinal,
			MakeArrayView(OwnedIsAArguments, 2),
			nullptr,
			OwnedContext,
			Result));
	TestFalse(
		TEXT("Destroyed Actor capability is revoked"),
		Ownership.HasCapability(OwnedHandle));
	return true;
}

#endif
