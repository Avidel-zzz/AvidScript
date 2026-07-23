#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectTypeBinding.h"

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
	const int32 ObjectTypeOrdinal,
	const FString& BaseTypeId)
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("object:") + FString(ClassPath);
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(Type.CanonicalType, {});
	Type.Kind = TEXT("object_handle");
	Type.CppType = TEXT("UObject*");
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiTypes = { TEXT("i"), TEXT("i") };
	Type.ObjectTypeOrdinal = ObjectTypeOrdinal;
	Type.ClassPath = ClassPath;
	Type.BaseTypeId = BaseTypeId;
	return Type;
}

bool WriteObjectType(
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
	return true;
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
		0,
		FString());
	const FAvidScriptBindingTypeModel ActorType = MakeObjectType(
		TEXT("/Script/Engine.Actor"),
		1,
		UObjectType.StableId);
	const FAvidScriptBindingTypeModel ActorComponentType = MakeObjectType(
		TEXT("/Script/Engine.ActorComponent"),
		2,
		UObjectType.StableId);
	const FAvidScriptBindingTypeModel SceneComponentType = MakeObjectType(
		TEXT("/Script/Engine.SceneComponent"),
		3,
		ActorComponentType.StableId);
	Package.Types = { UObjectType, ActorType, ActorComponentType, SceneComponentType };
	Package.SelfTypeId = ActorType.StableId;

	FAvidScriptBindingClassReferenceModel ActorReference;
	ActorReference.Ordinal = 0;
	ActorReference.ScriptName = TEXT("ActorClass");
	ActorReference.ClassPath = TEXT("/Script/Engine.Actor");
	ActorReference.BaseClassPath = TEXT("/Script/Engine.Actor");
	ActorReference.LoadPolicy = TEXT("EditorLoad");
	ActorReference.ResultTypeId = ActorType.StableId;
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
	Writer->WriteValue(TEXT("self_type_id"), Package.SelfTypeId);
	Writer->WriteArrayStart(TEXT("types"));
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		WriteObjectType(Writer, Type);
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("class_references"));
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stable_id"), ActorReference.StableId);
	Writer->WriteValue(TEXT("ordinal"), ActorReference.Ordinal);
	Writer->WriteValue(TEXT("script_name"), ActorReference.ScriptName);
	Writer->WriteValue(TEXT("class_path"), ActorReference.ClassPath);
	Writer->WriteValue(TEXT("base_class_path"), ActorReference.BaseClassPath);
	Writer->WriteValue(TEXT("load_policy"), ActorReference.LoadPolicy);
	Writer->WriteValue(TEXT("result_type_id"), ActorReference.ResultTypeId);
	Writer->WriteObjectEnd();
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("bindings"));
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
	TestEqual(TEXT("Immutable plan contains all object types"), Package->GetObjectTypeCount(), 4);
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
	const uint32 ObjectTypeOrdinal = FindObjectTypeOrdinal(*Package);
	if (!TestTrue(TEXT("Object type import is attached to the package"), ObjectTypeOrdinal != MAX_uint32))
	{
		return false;
	}

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
	const uint64 ComponentArguments[] = { ComponentHandle.Slot, ComponentHandle.Generation, 2 };
	if (!TestTrue(TEXT("Component matches its cached ActorComponent class"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, ComponentArguments, Context, Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestEqual(TEXT("Component type match returns one"), Result.ReturnValue, static_cast<uint64>(1));

	const uint64 MismatchArguments[] = { ComponentHandle.Slot, ComponentHandle.Generation, 1 };
	if (!TestTrue(TEXT("Component mismatch remains a successful dispatch"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, MismatchArguments, Context, Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestEqual(TEXT("Component mismatch returns zero"), Result.ReturnValue, static_cast<uint64>(0));

	FAvidScriptObjectRegistry ForeignRegistry;
	FAvidScriptObjectHandleResult ForeignRegisterResult;
	const FAvidScriptObjectHandle ForeignHandle = ForeignRegistry.RegisterObject(
		NewObject<USceneComponent>(GetTransientPackage()),
		ForeignRegisterResult,
		false);
	if (!TestTrue(TEXT("Foreign component registers"), ForeignRegisterResult.bSucceeded))
	{
		return false;
	}
	const uint64 ForeignArguments[] = { ForeignHandle.Slot, ForeignHandle.Generation, 2 };
	TestFalse(TEXT("Foreign registry generation fails closed"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, ForeignArguments, Context, Result));
	TestTrue(TEXT("Foreign registry reports the registry generation failure"),
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
	const uint64 InvalidOrdinalArguments[] = { CurrentHandle.Slot, CurrentHandle.Generation, 4 };
	TestFalse(TEXT("Out-of-range object type ordinal fails closed"),
		DispatchObjectType(*Package, ObjectTypeOrdinal, InvalidOrdinalArguments, Context, Result));
	TestTrue(TEXT("Out-of-range object type ordinal is stable"),
		Result.Details.Contains(TEXT("object_type_ordinal_out_of_range")));
	return true;
}

#endif
