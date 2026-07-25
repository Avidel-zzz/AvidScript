#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptSceneAttachmentBinding.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonWriter.h"

namespace
{
class FObjectFactoryCandidateJournal final
	: public IAvidScriptBindingHostEffectJournal
{
public:
	bool PrepareEffect(
		FAvidScriptObjectRegistry&,
		const FAvidScriptObjectHandle&,
		UObject&,
		EAvidScriptBindingReloadEffect,
		FAvidScriptBindingHostEffectPrepareResult&) override
	{
		++PrepareCount;
		return false;
	}

	int32 PrepareCount = 0;
};

FAvidScriptBindingTypeModel MakeFactoryPlanObjectType(
	const TCHAR* ClassPath,
	const TCHAR* CppType,
	const int32 Ordinal,
	const FString& BaseTypeId)
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("object:") + FString(ClassPath);
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		Type.CanonicalType, {});
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

void WriteObjectType(
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

FAvidScriptBindingPackageModel MakeFactoryPackage(
	const TCHAR* ClassPath,
	const TCHAR* CppType,
	const EAvidScriptObjectFactoryKind Kind,
	const int32 OuterOrdinal)
{
	FAvidScriptBindingPackageModel Package;
	Package.SchemaVersion = 7;
	Package.GeneratorVersion = TEXT("51.1.test");
	Package.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = TEXT("avidscript.test.object_factory_plan");

	const FAvidScriptBindingTypeModel ObjectType = MakeFactoryPlanObjectType(
		TEXT("/Script/CoreUObject.Object"), TEXT("UObject"), 0, FString());
	const FAvidScriptBindingTypeModel ActorType = MakeFactoryPlanObjectType(
		TEXT("/Script/Engine.Actor"), TEXT("AActor"), 1, ObjectType.StableId);
	const FAvidScriptBindingTypeModel ActorComponentType = MakeFactoryPlanObjectType(
		TEXT("/Script/Engine.ActorComponent"), TEXT("UActorComponent"), 2, ObjectType.StableId);
	Package.Types = { ObjectType, ActorType, ActorComponentType };
	if (FString(ClassPath) != TEXT("/Script/Engine.ActorComponent"))
	{
		Package.Types.Add(MakeFactoryPlanObjectType(
			ClassPath, CppType, 3, ActorComponentType.StableId));
	}
	Package.SelfTypeId = ActorType.StableId;

	FAvidScriptBindingClassReferenceModel Reference;
	Reference.Ordinal = 0;
	Reference.ScriptName = TEXT("FactoryClass");
	Reference.ClassPath = ClassPath;
	Reference.BaseClassPath = FString(ClassPath) == TEXT("/Script/Engine.ActorComponent")
		? TEXT("/Script/CoreUObject.Object")
		: TEXT("/Script/Engine.ActorComponent");
	Reference.LoadPolicy = TEXT("EditorLoad");
	Reference.ResultTypeId = FString(ClassPath) == TEXT("/Script/Engine.ActorComponent")
		? ObjectType.StableId
		: ActorComponentType.StableId;
	Reference.StableId = FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
		Reference.ClassPath, Reference.BaseClassPath, Reference.LoadPolicy);
	Package.ClassReferences = { Reference };

	FAvidScriptBindingObjectFactoryModel Factory;
	Factory.Ordinal = 0;
	Factory.ScriptName = TEXT("Factory");
	Factory.ClassReferenceId = Reference.StableId;
	Factory.Kind = Kind;
	Factory.OuterTypeId = Package.Types[OuterOrdinal].StableId;
	Factory.Ownership = EAvidScriptObjectOwnershipPolicy::Session;
	Factory.Registration = Kind == EAvidScriptObjectFactoryKind::ActorComponent
		? EAvidScriptComponentRegistrationPolicy::RegisterInstance
		: EAvidScriptComponentRegistrationPolicy::None;
	Factory.StableId = FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
		Factory.ClassReferenceId,
		Factory.Kind,
		Factory.OuterTypeId,
		Factory.Ownership,
		Factory.Registration);
	Package.ObjectFactories = { Factory };
	Package.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
	Package.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);
	return Package;
}

bool SerializeFactoryPackage(
	const FAvidScriptBindingPackageModel& Package,
	FString& OutJson)
{
	OutJson.Empty();
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
	for (const FAvidScriptBindingClassReferenceModel& Reference : Package.ClassReferences)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Reference.StableId);
		Writer->WriteValue(TEXT("ordinal"), Reference.Ordinal);
		Writer->WriteValue(TEXT("script_name"), Reference.ScriptName);
		Writer->WriteValue(TEXT("class_path"), Reference.ClassPath);
		Writer->WriteValue(TEXT("base_class_path"), Reference.BaseClassPath);
		Writer->WriteValue(TEXT("load_policy"), Reference.LoadPolicy);
		Writer->WriteValue(TEXT("result_type_id"), Reference.ResultTypeId);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("object_factories"));
	for (const FAvidScriptBindingObjectFactoryModel& Factory : Package.ObjectFactories)
	{
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
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("bindings"));
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	return Writer->Close();
}

bool LoadFactoryPackage(
	const FAvidScriptBindingPackageModel& Model,
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FAvidScriptBindingPackageLoadResult& OutResult)
{
	FString Json;
	return SerializeFactoryPackage(Model, Json)
		&& FAvidScriptBindingPackage::LoadDescriptor(Json, OutPackage, OutResult);
}

uint32 FindObjectFactoryBindingOrdinal(
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
	return MAX_uint32;
}

uint32 FindSceneAttachmentBindingOrdinal(
	const FAvidScriptBindingPackage& Package,
	const EAvidScriptBindingInvocationKind Kind)
{
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectFactoryPlanTest,
	"AvidScript.Runtime.Binding.ObjectFactoryPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectFactoryPlanTest::RunTest(const FString& Parameters)
{
	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	const FAvidScriptBindingPackageModel ValidPackage = MakeFactoryPackage(
		TEXT("/Script/Engine.SceneComponent"),
		TEXT("USceneComponent"),
		EAvidScriptObjectFactoryKind::ActorComponent,
		1);
	if (!TestTrue(TEXT("Factory package loads"),
		LoadFactoryPackage(ValidPackage, Package, LoadResult))
		|| !TestNotNull(TEXT("Factory package is returned"), Package.Get()))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	TestEqual(TEXT("Factory result reports one immutable factory"), LoadResult.ObjectFactoryCount, 1);
	TestEqual(TEXT("Factory package exposes one immutable factory"), Package->GetObjectFactoryCount(), 1);
	const TConstArrayView<FAvidScriptObjectFactoryBindingSpec> FactorySpecs =
		FAvidScriptObjectFactoryBinding::GetSpecs();
	TestEqual(TEXT("Object factory publishes three generic import specs"), FactorySpecs.Num(), 3);
	const TConstArrayView<FAvidScriptSceneAttachmentBindingSpec> AttachmentSpecs =
		FAvidScriptSceneAttachmentBinding::GetSpecs();
	TestEqual(TEXT("Scene attachment publishes two generic import specs"), AttachmentSpecs.Num(), 2);
	TestEqual(
		TEXT("Component factory package adds type, factory, and attachment imports"),
		Package->GetVmPackage().Imports.Num(),
		6);
	TestTrue(
		TEXT("Construct uses one packed-i64 crossing"),
		FactorySpecs.ContainsByPredicate(
			[](const FAvidScriptObjectFactoryBindingSpec& Spec)
			{
				return Spec.Kind == EAvidScriptBindingInvocationKind::ObjectConstruct
					&& Spec.ImportName == TEXT("avid_object_construct")
					&& Spec.Signature == TEXT("(iii)I");
			}));
	TestTrue(
		TEXT("Release uses one i32 crossing"),
		FactorySpecs.ContainsByPredicate(
			[](const FAvidScriptObjectFactoryBindingSpec& Spec)
			{
				return Spec.Kind == EAvidScriptBindingInvocationKind::ObjectRelease
					&& Spec.ImportName == TEXT("avid_object_release")
					&& Spec.Signature == TEXT("(ii)i");
			}));
	TestTrue(
		TEXT("FindComponent uses one packed-i64 crossing"),
		FactorySpecs.ContainsByPredicate(
			[](const FAvidScriptObjectFactoryBindingSpec& Spec)
			{
				return Spec.Kind == EAvidScriptBindingInvocationKind::ActorFindComponent
					&& Spec.ImportName == TEXT("avid_actor_find_component")
					&& Spec.Signature == TEXT("(iii)I");
			}));
	TestTrue(
		TEXT("Attach uses one five-cell i32 crossing"),
		AttachmentSpecs.ContainsByPredicate(
			[](const FAvidScriptSceneAttachmentBindingSpec& Spec)
			{
				return Spec.Kind
					== EAvidScriptBindingInvocationKind::SceneComponentAttach
					&& Spec.ImportName == TEXT("avid_scene_component_attach")
					&& Spec.Signature == TEXT("(iiiii)i");
			}));
	TestTrue(
		TEXT("Detach uses one three-cell i32 crossing"),
		AttachmentSpecs.ContainsByPredicate(
			[](const FAvidScriptSceneAttachmentBindingSpec& Spec)
			{
				return Spec.Kind
					== EAvidScriptBindingInvocationKind::SceneComponentDetach
					&& Spec.ImportName == TEXT("avid_scene_component_detach")
					&& Spec.Signature == TEXT("(iii)i");
			}));
	TestEqual(TEXT("Factory package retains its descriptor schema provenance"),
		Package->GetDescriptorSchemaVersion(), 7);
	TestEqual(TEXT("Each unique factory and graph class path loads once"),
		Package->GetInstrumentation().ClassLoadCount, static_cast<uint64>(4));
	TestEqual(TEXT("Factory plan load performs no reflected member-name lookup"),
		Package->GetInstrumentation().ReflectedNameLookupCount,
		static_cast<uint64>(0));
	const FAvidScriptObjectFactoryPlan* Plan = nullptr;
	if (!TestTrue(TEXT("Factory ordinal resolves from the immutable plan"),
		Package->TryResolveObjectFactory(0, Plan))
		|| !TestNotNull(TEXT("Factory lookup returns a plan"), Plan))
	{
		return false;
	}
	TestEqual(TEXT("Factory caches its concrete class"), Plan->ObjectClass, USceneComponent::StaticClass());
	TestEqual(TEXT("Factory caches its required outer"), Plan->RequiredOuterClass, AActor::StaticClass());
	TestEqual(TEXT("Factory caches the concrete object-type ordinal"), Plan->ResultObjectTypeOrdinal, 3);
	TestEqual(TEXT("Factory caches session ownership"), Plan->Ownership, EAvidScriptObjectOwnershipPolicy::Session);
	TestEqual(TEXT("Factory caches component registration"),
		Plan->Registration, EAvidScriptComponentRegistrationPolicy::RegisterInstance);
	TestEqual(TEXT("Ordinal factory lookup performs no additional class load"),
		Package->GetInstrumentation().ClassLoadCount, static_cast<uint64>(4));
	UClass* LegacyClass = nullptr;
	UClass* LegacyBaseClass = nullptr;
	TestFalse(TEXT("Factory-owned class is not exposed through lifecycle lookup"),
		Package->TryResolveClassReference(0, LegacyClass, LegacyBaseClass));
	Plan = reinterpret_cast<const FAvidScriptObjectFactoryPlan*>(static_cast<UPTRINT>(1));
	TestFalse(TEXT("Out-of-range factory ordinal fails closed"),
		Package->TryResolveObjectFactory(1, Plan));
	TestNull(TEXT("Out-of-range factory ordinal clears output"), Plan);

	const uint32 ConstructOrdinal = FindObjectFactoryBindingOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::ObjectConstruct);
	const uint32 ReleaseOrdinal = FindObjectFactoryBindingOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::ObjectRelease);
	const uint32 FindOrdinal = FindObjectFactoryBindingOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::ActorFindComponent);
	const uint32 AttachOrdinal = FindSceneAttachmentBindingOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::SceneComponentAttach);
	const uint32 DetachOrdinal = FindSceneAttachmentBindingOrdinal(
		*Package,
		EAvidScriptBindingInvocationKind::SceneComponentDetach);
	if (!TestTrue(TEXT("Construct import ordinal resolves"), ConstructOrdinal != MAX_uint32)
		|| !TestTrue(TEXT("Release import ordinal resolves"), ReleaseOrdinal != MAX_uint32)
		|| !TestTrue(TEXT("FindComponent import ordinal resolves"), FindOrdinal != MAX_uint32)
		|| !TestTrue(TEXT("Attach import ordinal resolves"), AttachOrdinal != MAX_uint32)
		|| !TestTrue(TEXT("Detach import ordinal resolves"), DetachOrdinal != MAX_uint32))
	{
		return false;
	}

	FAvidScriptObjectRegistry CandidateRegistry;
	FAvidScriptSessionObjectOwnership CandidateOwnership;
	FObjectFactoryCandidateJournal CandidateJournal;
	FAvidScriptBindingInvocationContext CandidateContext;
	CandidateContext.ObjectRegistry = &CandidateRegistry;
	CandidateContext.ObjectOwnership = &CandidateOwnership;
	CandidateContext.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	CandidateContext.HostEffectJournal = &CandidateJournal;
	TArray<uint8> CandidateScratch;
	FAvidScriptDynamicHostCallResult CandidateResult;
	const uint64 ConstructArguments[] = { 0, 0, 0 };
	FAvidScriptDynamicHostCall CandidateCall;
	CandidateCall.BindingOrdinal = ConstructOrdinal;
	CandidateCall.Arguments = ConstructArguments;
	TestFalse(
		TEXT("Candidate reload rejects Construct before object mutation"),
		Package->Dispatch(
			CandidateCall,
			CandidateContext,
			CandidateScratch,
			CandidateResult));
	TestTrue(TEXT("Construct candidate rejection has the reload category"),
		CandidateResult.Details.Contains(TEXT("binding_reload_effect_unsupported")));
	const uint64 ReleaseArguments[] = { 1, 1 };
	CandidateCall.BindingOrdinal = ReleaseOrdinal;
	CandidateCall.Arguments = ReleaseArguments;
	TestFalse(
		TEXT("Candidate reload rejects Release before ownership mutation"),
		Package->Dispatch(
			CandidateCall,
			CandidateContext,
			CandidateScratch,
			CandidateResult));
	TestTrue(TEXT("Release candidate rejection has the reload category"),
		CandidateResult.Details.Contains(TEXT("binding_reload_effect_unsupported")));
	const uint64 FindArguments[] = { 1, 1, 3 };
	CandidateCall.BindingOrdinal = FindOrdinal;
	CandidateCall.Arguments = FindArguments;
	TestFalse(
		TEXT("Candidate FindComponent passes the reload gate and reaches handle validation"),
		Package->Dispatch(
			CandidateCall,
			CandidateContext,
			CandidateScratch,
			CandidateResult));
	TestFalse(TEXT("FindComponent is not rejected as a candidate side effect"),
		CandidateResult.Details.Contains(TEXT("binding_reload_effect_unsupported")));
	const uint64 AttachArguments[] = { 1, 1, 2, 1, 0 };
	CandidateCall.BindingOrdinal = AttachOrdinal;
	CandidateCall.Arguments = AttachArguments;
	TestFalse(
		TEXT("Candidate reload rejects Attach before component mutation"),
		Package->Dispatch(
			CandidateCall,
			CandidateContext,
			CandidateScratch,
			CandidateResult));
	TestTrue(TEXT("Attach candidate rejection has the reload category"),
		CandidateResult.Details.Contains(TEXT("binding_reload_effect_unsupported")));
	const uint64 DetachArguments[] = { 1, 1, 0 };
	CandidateCall.BindingOrdinal = DetachOrdinal;
	CandidateCall.Arguments = DetachArguments;
	TestFalse(
		TEXT("Candidate reload rejects Detach before component mutation"),
		Package->Dispatch(
			CandidateCall,
			CandidateContext,
			CandidateScratch,
			CandidateResult));
	TestTrue(TEXT("Detach candidate rejection has the reload category"),
		CandidateResult.Details.Contains(TEXT("binding_reload_effect_unsupported")));
	TestEqual(TEXT("Candidate object gates do not invoke the transform journal"),
		CandidateJournal.PrepareCount, 0);

	const auto TestActivationFailure = [this](
		const TCHAR* Label,
		const FAvidScriptBindingPackageModel& Model,
		const TCHAR* ExpectedCategory)
	{
		TSharedPtr<const FAvidScriptBindingPackage> RejectedPackage;
		FAvidScriptBindingPackageLoadResult RejectedResult;
		TestFalse(Label, LoadFactoryPackage(Model, RejectedPackage, RejectedResult));
		TestNull(TEXT("Rejected factory package does not activate"), RejectedPackage.Get());
		TestEqual(TEXT("Factory rejection category is stable"),
			RejectedResult.ErrorCategory, FString(ExpectedCategory));
	};
	TestActivationFailure(
		TEXT("Abstract factory class fails before activation"),
		MakeFactoryPackage(
			TEXT("/Script/Engine.ActorComponent"),
			TEXT("UActorComponent"),
			EAvidScriptObjectFactoryKind::ActorComponent,
			1),
		TEXT("binding_factory_class_abstract"));
	TestActivationFailure(
		TEXT("Component class cannot use new-object factory kind"),
		MakeFactoryPackage(
			TEXT("/Script/Engine.SceneComponent"),
			TEXT("USceneComponent"),
			EAvidScriptObjectFactoryKind::NewObject,
			1),
		TEXT("binding_factory_kind_mismatch"));
	TestActivationFailure(
		TEXT("Component outer and ClassWithin mismatch fails before activation"),
		MakeFactoryPackage(
			TEXT("/Script/Engine.SceneComponent"),
			TEXT("USceneComponent"),
			EAvidScriptObjectFactoryKind::ActorComponent,
			0),
		TEXT("binding_factory_outer_mismatch"));

	FAvidScriptBindingPackageModel InheritanceMismatch = MakeFactoryPackage(
		TEXT("/Script/Engine.SceneComponent"),
		TEXT("USceneComponent"),
		EAvidScriptObjectFactoryKind::ActorComponent,
		1);
	const FAvidScriptBindingTypeModel ConsoleType = MakeFactoryPlanObjectType(
		TEXT("/Script/Engine.Console"),
		TEXT("UConsole"),
		InheritanceMismatch.Types.Num(),
		InheritanceMismatch.Types[0].StableId);
	InheritanceMismatch.Types.Add(ConsoleType);
	InheritanceMismatch.Types.Sort([](
		const FAvidScriptBindingTypeModel& Left,
		const FAvidScriptBindingTypeModel& Right)
	{
		return Left.ClassPath < Right.ClassPath;
	});
	for (int32 Index = 0; Index < InheritanceMismatch.Types.Num(); ++Index)
	{
		InheritanceMismatch.Types[Index].ObjectTypeOrdinal = Index;
	}
	FAvidScriptBindingClassReferenceModel& MismatchedReference =
		InheritanceMismatch.ClassReferences[0];
	MismatchedReference.BaseClassPath = ConsoleType.ClassPath;
	MismatchedReference.ResultTypeId = ConsoleType.StableId;
	MismatchedReference.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
			MismatchedReference.ClassPath,
			MismatchedReference.BaseClassPath,
			MismatchedReference.LoadPolicy);
	FAvidScriptBindingObjectFactoryModel& MismatchedFactory =
		InheritanceMismatch.ObjectFactories[0];
	MismatchedFactory.ClassReferenceId = MismatchedReference.StableId;
	MismatchedFactory.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
			MismatchedFactory.ClassReferenceId,
			MismatchedFactory.Kind,
			MismatchedFactory.OuterTypeId,
			MismatchedFactory.Ownership,
			MismatchedFactory.Registration);
	InheritanceMismatch.SelectionHash =
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(
			InheritanceMismatch);
	InheritanceMismatch.PackageHash =
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(
			InheritanceMismatch);
	TestActivationFailure(
		TEXT("Factory class cannot escape its declared base constraint"),
		InheritanceMismatch,
		TEXT("binding_factory_class_inheritance_mismatch"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
