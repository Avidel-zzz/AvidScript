#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptHash.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptObjectTypeBinding.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"

namespace
{
FAvidScriptBindingTypeModel MakeObjectType(
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

FAvidScriptBindingTypeModel MakeStaticOwnerType()
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

FAvidScriptBindingTypeModel MakeBoolType()
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

FAvidScriptBindingValueModel MakeBoolValue(
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

FAvidScriptBindingFunctionModel MakeStaticSentinelBinding(
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
	Binding.ReturnValue = MakeBoolValue(TEXT("ReturnValue"), TEXT("return"), BoolType);
	Binding.Parameters.Add(MakeBoolValue(TEXT("A"), TEXT("value"), BoolType));
	Binding.CanonicalIdentity =
		TEXT("/Script/Engine.KismetMathLibrary::Not_PreBool(scalar:bool;A:value:scalar:bool)");
	Binding.StableId = FAvidScriptHash::Sha256HexUtf8(Binding.CanonicalIdentity);
	Binding.HostImport.Module = TEXT("avidscript");
	Binding.HostImport.Name = TEXT("avid_ue_") + Binding.StableId.Left(16);
	Binding.HostImport.Signature = TEXT("(ii)i");
	return Binding;
}

void WriteType(
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

void WriteValue(
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

bool MakeObjectTypeDescriptor(FString& OutJson)
{
	FAvidScriptBindingPackageModel Package;
	Package.SchemaVersion = 6;
	Package.GeneratorVersion = TEXT("50.0.test");
	Package.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = TEXT("avidscript.test.object_type");

	const FAvidScriptBindingTypeModel UObjectType = MakeObjectType(
		TEXT("/Script/CoreUObject.Object"),
		TEXT("UObject"),
		0,
		FString());
	const FAvidScriptBindingTypeModel ActorComponentType = MakeObjectType(
		TEXT("/Script/Engine.ActorComponent"),
		TEXT("UActorComponent"),
		1,
		UObjectType.StableId);
	const FAvidScriptBindingTypeModel SceneComponentType = MakeObjectType(
		TEXT("/Script/Engine.SceneComponent"),
		TEXT("USceneComponent"),
		2,
		ActorComponentType.StableId);
	const FAvidScriptBindingTypeModel StaticOwnerType = MakeStaticOwnerType();
	const FAvidScriptBindingTypeModel BoolType = MakeBoolType();
	Package.Types = {
		UObjectType,
		ActorComponentType,
		SceneComponentType,
		StaticOwnerType,
		BoolType
	};
	const FAvidScriptBindingFunctionModel StaticBinding = MakeStaticSentinelBinding(BoolType);
	Package.Bindings.Add(StaticBinding);
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
	Writer->WriteValue(TEXT("self_type_id"), Package.SelfTypeId);
	Writer->WriteArrayStart(TEXT("types"));
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		WriteType(Writer, Type);
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("class_references"));
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("bindings"));
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stable_id"), StaticBinding.StableId);
	Writer->WriteValue(TEXT("canonical_identity"), StaticBinding.CanonicalIdentity);
	Writer->WriteValue(TEXT("ordinal"), StaticBinding.Ordinal);
	Writer->WriteValue(TEXT("owner_class"), StaticBinding.OwnerClass);
	Writer->WriteValue(TEXT("binding_kind"), StaticBinding.BindingKind);
	Writer->WriteValue(TEXT("ue_member"), StaticBinding.UeMember);
	Writer->WriteValue(TEXT("script_name"), StaticBinding.ScriptName);
	Writer->WriteValue(TEXT("dispatch_mode"), StaticBinding.DispatchMode);
	Writer->WriteValue(TEXT("is_static"), StaticBinding.bStatic);
	Writer->WriteValue(TEXT("is_const"), StaticBinding.bConst);
	Writer->WriteValue(TEXT("reload_effect"), TEXT("none"));
	Writer->WriteObjectStart(TEXT("return"));
	WriteValue(Writer, StaticBinding.ReturnValue);
	Writer->WriteObjectEnd();
	Writer->WriteArrayStart(TEXT("parameters"));
	for (const FAvidScriptBindingValueModel& Parameter : StaticBinding.Parameters)
	{
		Writer->WriteObjectStart();
		WriteValue(Writer, Parameter);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectStart(TEXT("host_import"));
	Writer->WriteValue(TEXT("module"), StaticBinding.HostImport.Module);
	Writer->WriteValue(TEXT("name"), StaticBinding.HostImport.Name);
	Writer->WriteValue(TEXT("signature"), StaticBinding.HostImport.Signature);
	Writer->WriteObjectEnd();
	Writer->WriteObjectEnd();
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	return Writer->Close();
}

uint32 FindObjectTypeOrdinal(const FAvidScriptBindingPackage& Package)
{
	const TConstArrayView<FAvidScriptObjectTypeBindingSpec> Specs =
		FAvidScriptObjectTypeBindings::GetSpecs();
	for (const FAvidScriptObjectTypeBindingSpec& Spec : Specs)
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

bool DispatchObjectType(
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectTypeBindingTest,
	"AvidScript.Runtime.Binding.ObjectTypeDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectTypeBindingTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	if (!TestTrue(TEXT("Object type descriptor serializes"), MakeObjectTypeDescriptor(DescriptorJson)))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
		TEXT("Object type package loads"),
		FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, Package, LoadResult))
		|| !Package.IsValid())
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}
	TestEqual(TEXT("Immutable plan contains all object types"), Package->GetObjectTypeCount(), 3);
	TestEqual(TEXT("Class-reference-free package keeps no expected Self class"),
		Package->GetExpectedSelfClass(), static_cast<UClass*>(nullptr));
	TestEqual(TEXT("Class-reference-free package keeps no lifecycle class references"),
		Package->GetClassReferenceCount(), 0);
	TestEqual(TEXT("Package exposes only the reflected sentinel and object-type imports"),
		Package->GetVmPackage().Imports.Num(), 2);
	const TConstArrayView<FAvidScriptObjectTypeBindingSpec> Specs =
		FAvidScriptObjectTypeBindings::GetSpecs();
	TestEqual(TEXT("Object type capability publishes one import spec"), Specs.Num(), 1);
	if (!TestTrue(TEXT("Object type import uses the fixed ABI"),
		Specs.Num() == 1
		&& Specs[0].Kind == EAvidScriptBindingInvocationKind::ObjectTypeIsA
		&& Specs[0].ModuleName == TEXT("avidscript")
		&& Specs[0].ImportName == TEXT("avid_object_type_is_a")
		&& Specs[0].Signature == TEXT("(iii)i")))
	{
		return false;
	}
	for (const FAvidScriptObjectLifecycleBindingSpec& LifecycleSpec :
		FAvidScriptObjectLifecycleBindings::GetSpecs())
	{
		TestFalse(
			*FString::Printf(TEXT("Class-reference-free package excludes lifecycle import %s"),
				*LifecycleSpec.ImportName),
			Package->GetVmPackage().Imports.ContainsByPredicate(
				[&LifecycleSpec](const FAvidScriptVmDynamicImport& Import)
				{
					return Import.StableId == LifecycleSpec.StableId;
				}));
	}
	const uint32 ObjectTypeOrdinal = FindObjectTypeOrdinal(*Package);
	if (!TestTrue(TEXT("Object type import is attached to the package"), ObjectTypeOrdinal != MAX_uint32))
	{
		return false;
	}

	const uint64 ClassLoadCount = Package->GetInstrumentation().ClassLoadCount;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	UClass* CachedObjectClass = nullptr;
	UClass* CachedActorComponentClass = nullptr;
	UClass* CachedSceneComponentClass = nullptr;
	TestTrue(TEXT("Package retains cached UObject class after GC"),
		Package->TryResolveObjectType(0, CachedObjectClass));
	TestTrue(TEXT("Package retains cached ActorComponent class after GC"),
		Package->TryResolveObjectType(1, CachedActorComponentClass));
	TestTrue(TEXT("Package retains cached SceneComponent class after GC"),
		Package->TryResolveObjectType(2, CachedSceneComponentClass));
	TestEqual(TEXT("Cached UObject class remains stable"), CachedObjectClass, UObject::StaticClass());
	TestEqual(TEXT("Cached ActorComponent class remains stable"),
		CachedActorComponentClass, UActorComponent::StaticClass());
	TestEqual(TEXT("Cached SceneComponent class remains stable"),
		CachedSceneComponentClass, USceneComponent::StaticClass());

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle RetiredHandle = Registry.RegisterObject(
		NewObject<UObject>(),
		RegisterResult,
		false);
	if (!TestTrue(TEXT("Registry establishes a retired handle generation"), RegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptObjectHandleResult ReleaseResult;
	if (!TestTrue(TEXT("Registry retires the initial generation"),
		Registry.ReleaseHandle(RetiredHandle, ReleaseResult, false)))
	{
		return false;
	}

	USceneComponent* Component = NewObject<USceneComponent>(GetTransientPackage());
	const FAvidScriptObjectHandle ComponentHandle = Registry.RegisterObject(Component, RegisterResult, false);
	if (!TestTrue(TEXT("Component registers without an Actor or World"),
		RegisterResult.bSucceeded && ComponentHandle.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("Test target is not an Actor"), Component->IsA(AActor::StaticClass()));

	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	FAvidScriptDynamicHostCallResult Result;
	const uint64 ComponentArguments[] = { ComponentHandle.Slot, ComponentHandle.Generation, 1 };
	if (!TestTrue(TEXT("Component matches its cached ActorComponent class"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, ComponentArguments, Context, Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestEqual(TEXT("Component type match returns one"), Result.ReturnValue, 1);

	const FAvidScriptObjectHandle PlainObjectHandle = Registry.RegisterObject(
		NewObject<UObject>(),
		RegisterResult,
		false);
	if (!TestTrue(TEXT("Plain UObject registers for mismatch coverage"),
		RegisterResult.bSucceeded && PlainObjectHandle.IsValid()))
	{
		return false;
	}
	const uint64 MismatchArguments[] = { PlainObjectHandle.Slot, PlainObjectHandle.Generation, 2 };
	if (!TestTrue(TEXT("Plain UObject mismatch remains a successful dispatch"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, MismatchArguments, Context, Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestEqual(TEXT("Plain UObject mismatch returns zero"), Result.ReturnValue, 0);

	FAvidScriptObjectRegistry CrossRegistry;
	FAvidScriptObjectHandleResult CrossRegistryRegisterResult;
	const FAvidScriptObjectHandle CrossRegistryHandle = CrossRegistry.RegisterObject(
		NewObject<USceneComponent>(GetTransientPackage()),
		CrossRegistryRegisterResult,
		false);
	if (!TestTrue(TEXT("Cross-registry component registers"),
		CrossRegistryRegisterResult.bSucceeded && CrossRegistryHandle.IsValid()))
	{
		return false;
	}
	const uint64 CrossRegistryArguments[] = {
		CrossRegistryHandle.Slot,
		CrossRegistryHandle.Generation,
		1
	};
	TestFalse(TEXT("Cross-registry handle with a mismatched local generation fails closed"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, CrossRegistryArguments, Context, Result));
	TestTrue(TEXT("Cross-registry generation mismatch reports the local registry failure"),
		Result.Details.Contains(TEXT("generation_mismatch")));

	if (!TestTrue(TEXT("Component handle releases"),
		Registry.ReleaseHandle(ComponentHandle, ReleaseResult, false)))
	{
		return false;
	}
	TestFalse(TEXT("Stale component handle fails closed"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, ComponentArguments, Context, Result));
	TestTrue(TEXT("Stale component handle reports generation mismatch"),
		Result.Details.Contains(TEXT("generation_mismatch")));

	const uint64 InvalidHandleArguments[] = { 0, 1, 2 };
	TestFalse(TEXT("Invalid component handle fails closed"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, InvalidHandleArguments, Context, Result));
	TestTrue(TEXT("Invalid component handle reports invalid handle"),
		Result.Details.Contains(TEXT("invalid_handle")));

	const FAvidScriptObjectHandle CurrentHandle = Registry.RegisterObject(
		NewObject<USceneComponent>(GetTransientPackage()),
		RegisterResult,
		false);
	if (!TestTrue(TEXT("Current component registers for ordinal bounds coverage"),
		RegisterResult.bSucceeded && CurrentHandle.IsValid()))
	{
		return false;
	}
	const uint64 InvalidOrdinalArguments[] = { CurrentHandle.Slot, CurrentHandle.Generation, 3 };
	TestFalse(TEXT("Out-of-range object type ordinal fails closed"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, InvalidOrdinalArguments, Context, Result));
	TestTrue(TEXT("Out-of-range object type ordinal is stable"),
		Result.Details.Contains(TEXT("object_type_ordinal_out_of_range")));
	TestEqual(TEXT("GC and dispatch do not trigger additional class loads"),
		Package->GetInstrumentation().ClassLoadCount, ClassLoadCount);
	return true;
}

#endif
