#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingSelectionResolver.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorProjectBindingProfile.h"
#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptHash.h"
#include "BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.h"

#include "Algo/Reverse.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManifest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <initializer_list>

namespace
{
constexpr const TCHAR* ProjectBindingProfileTypedSelfClassPath =
	TEXT("/Script/AvidScriptEditor.AvidScriptBindingRuntimeProcessEventTestActor");

bool IsProjectProfileSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

FAvidScriptReflectedClassSelection MakeProjectProfileClassRule(
	const FString& ClassPath,
	std::initializer_list<const TCHAR*> Functions)
{
	FAvidScriptReflectedClassSelection Rule;
	Rule.OwnerClassPath = ClassPath;
	for (const TCHAR* Function : Functions)
	{
		Rule.IncludeFunctions.Add(FName(Function));
	}
	return Rule;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileGeneratedGrantRejectTest,
	"AvidScript.Editor.ProjectBindingProfile.GeneratedGrantRejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileGeneratedGrantRejectTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec Spec;
	Spec.PackageName = TEXT("avidscript.project.generated.reject");
	FAvidScriptReflectedClassSelection Rule;
	Rule.OwnerClassPath = TEXT("/Script/Engine.Actor");
	Rule.IncludeFunctions.Add(TEXT("K2_GetActorLocation"));
	Rule.GeneratedNativeFunctions.Add(TEXT("SetActorScale3D"));
	Spec.Classes.Add(Rule);

	FAvidScriptBindingSelectionProfile Selection;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	FString SelectionHash;
	FAvidScriptBindingSelectionResolveResult Result;
	TestFalse(
		TEXT("Generated grant must be a strict function subset"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			Selection,
			ClassReferences,
			SelectionHash,
			Result));
	TestEqual(
		TEXT("Subset failure category is stable"),
		Result.ErrorCategory,
		FString(TEXT("generated_native_not_selected")));

	Spec.Classes[0].IncludeFunctions.Add(TEXT("SetActorScale3D"));
	Spec.Classes[0].NativeDirectFunctions.Add(TEXT("SetActorScale3D"));
	TestFalse(
		TEXT("Direct and generated ownership conflict"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			Selection,
			ClassReferences,
			SelectionHash,
			Result));
	TestEqual(
		TEXT("Ownership failure category is stable"),
		Result.ErrorCategory,
		FString(TEXT("generated_native_dispatch_conflict")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileStableResolutionTest,
	"AvidScript.Editor.ProjectBindingProfile.StableResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileStableResolutionTest::RunTest(const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec FirstSpec;
	FirstSpec.PackageName = TEXT("avidscript.project.stable");
	FirstSpec.SelfClassPath = ProjectBindingProfileTypedSelfClassPath;
	FirstSpec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.Pawn"),
		{ TEXT("AddMovementInput") }));
	FirstSpec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.Actor"),
		{ TEXT("SetActorScale3D"), TEXT("K2_GetActorLocation") }));
	FirstSpec.Classes[1].ExcludeFunctions.Add(FName(TEXT("K2_SetActorLocation")));
	FirstSpec.ClassReferences.Add({
		TEXT("ProjectileClass"),
		TEXT("/Script/Engine.StaticMeshActor"),
		TEXT("/Script/Engine.Actor"),
		TEXT("EditorLoad")
	});
	FirstSpec.ClassReferences.Add({
		TEXT("LightClass"),
		TEXT("/Script/Engine.PointLight"),
		TEXT("/Script/Engine.Actor"),
		TEXT("EditorLoad")
	});

	FAvidScriptBindingSelectionProfile FirstSelection;
	TArray<FAvidScriptProjectBindingClassSpec> FirstClassReferences;
	FString FirstHash;
	FAvidScriptBindingSelectionResolveResult FirstResult;
	TestTrue(
		TEXT("Project profile resolves"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			FirstSpec,
			FirstSelection,
			FirstClassReferences,
			FirstHash,
			FirstResult));
	TestTrue(TEXT("Project profile result succeeds"), FirstResult.bSucceeded);
	TestEqual(
		TEXT("Typed self class path is resolved"),
		FirstSelection.SelfClassPath,
		FString(ProjectBindingProfileTypedSelfClassPath));
	TestEqual(TEXT("Project profile keeps two classes"), FirstSelection.Classes.Num(), 2);
	TestEqual(TEXT("Project profile keeps two class references"), FirstClassReferences.Num(), 2);
	TestTrue(TEXT("Project profile hash is SHA-256"), IsProjectProfileSha256(FirstHash));
	if (FirstSelection.Classes.Num() == 2)
	{
		TestEqual(
			TEXT("Actor class sorts first"),
			FirstSelection.Classes[0].OwnerClassPath,
			FString(TEXT("/Script/Engine.Actor")));
		if (FirstSelection.Classes[0].IncludeFunctions.Num() == 2)
		{
			TestEqual(
				TEXT("Actor function names are normalized"),
				FirstSelection.Classes[0].IncludeFunctions[0],
				FName(TEXT("K2_GetActorLocation")));
		}
		TestEqual(
			TEXT("Actor exclude filters are retained"),
			FirstSelection.Classes[0].ExcludeFunctions.Num(),
			1);
	}

	FAvidScriptProjectBindingProfileSpec SecondSpec = FirstSpec;
	Algo::Reverse(SecondSpec.Classes);
	Algo::Reverse(SecondSpec.Classes[1].IncludeFunctions);
	Algo::Reverse(SecondSpec.ClassReferences);
	FAvidScriptBindingSelectionProfile SecondSelection;
	TArray<FAvidScriptProjectBindingClassSpec> SecondClassReferences;
	FString SecondHash;
	FAvidScriptBindingSelectionResolveResult SecondResult;
	TestTrue(
		TEXT("Reordered project profile resolves"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			SecondSpec,
			SecondSelection,
			SecondClassReferences,
			SecondHash,
			SecondResult));
	TestEqual(TEXT("Reordered profile hash is stable"), SecondHash, FirstHash);

	FAvidScriptProjectBindingProfileSpec DifferentSelfSpec = FirstSpec;
	DifferentSelfSpec.SelfClassPath = TEXT("/Script/Engine.StaticMeshActor");
	FAvidScriptBindingSelectionProfile DifferentSelfSelection;
	TArray<FAvidScriptProjectBindingClassSpec> DifferentSelfClassReferences;
	FString DifferentSelfHash;
	FAvidScriptBindingSelectionResolveResult DifferentSelfResult;
	TestTrue(
		TEXT("Different typed self profile resolves"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			DifferentSelfSpec,
			DifferentSelfSelection,
			DifferentSelfClassReferences,
			DifferentSelfHash,
			DifferentSelfResult));
	TestNotEqual(
		TEXT("Typed self class path changes selection identity"),
		DifferentSelfHash,
		FirstHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfilePhase51IdentityGoldenTest,
	"AvidScript.Editor.ProjectBindingProfile.Phase51IdentityGolden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfilePhase51IdentityGoldenTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec Spec;
	Spec.PackageName = TEXT("avidscript.project.phase51_identity_golden");
	Spec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.Actor"),
		{ TEXT("K2_GetActorLocation") }));

	FAvidScriptBindingSelectionProfile Selection;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	FString SelectionHash;
	FAvidScriptBindingSelectionResolveResult ResolveResult;
	if (!TestTrue(
			TEXT("Phase51 identity fixture resolves"),
			FAvidScriptEditorProjectBindingProfile::Resolve(
				Spec,
				Selection,
				ClassReferences,
				SelectionHash,
				ResolveResult)))
	{
		AddError(ResolveResult.ErrorMessage);
		return false;
	}

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult DescriptorSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!TestTrue(
			TEXT("Phase51 identity fixture regenerates its descriptor"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				Selection,
				ClassReferences,
				DescriptorJson,
				DescriptorSelectionResult,
				DescriptorResult)))
	{
		AddError(DescriptorResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel DescriptorPackage;
	FString DescriptorErrorCategory;
	FString DescriptorErrorSource;
	if (!TestTrue(
		TEXT("Phase51 identity fixture descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			DescriptorPackage,
			DescriptorErrorCategory,
			DescriptorErrorSource)))
	{
		AddError(DescriptorErrorCategory + TEXT(":") + DescriptorErrorSource);
		return false;
	}
	TestEqual(
		TEXT("Non-writable descriptor keeps the Phase51 generator identity"),
		DescriptorPackage.GeneratorVersion,
		FString(TEXT("50.1.0")));

	FModuleManifest ModuleManifest;
	const FString ManifestPath = FModuleManifest::GetFileName(
		FPlatformProcess::GetModulesDirectory(),
		false);
	if (!TestTrue(
			TEXT("Phase51 identity golden can read the active engine build id"),
			FModuleManifest::TryRead(ManifestPath, ModuleManifest)
				&& !ModuleManifest.BuildId.IsEmpty()))
	{
		return false;
	}

	const FString Phase51Identity =
		TEXT("resolver=50.1.0\n")
		TEXT("engine_build_id=") + ModuleManifest.BuildId
		+ TEXT("\npackage=") + Spec.PackageName
		+ TEXT("\nself_class=")
		+ TEXT("\ndescriptor_selection=") + DescriptorResult.SelectionHash
		+ TEXT("\ndescriptor_package=") + DescriptorResult.PackageHash
		+ TEXT("\nclass_rule:/Script/Engine.Actor")
		TEXT("|if=K2_GetActorLocation|ef=|ip=|ep=|drp=0");
	TestEqual(
		TEXT("Profile selection hash retains the Phase51 identity bytes"),
		SelectionHash,
		FAvidScriptHash::Sha256HexUtf8(Phase51Identity));

	FAvidScriptProjectBindingProfileSpec FactorySpec = Spec;
	FactorySpec.PackageName =
		TEXT("avidscript.project.phase51_factory_identity_golden");
	FactorySpec.ClassReferences.Add({
		TEXT("InventoryStateClass"),
		TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"),
		TEXT("/Script/CoreUObject.Object"),
		TEXT("EditorLoad")
	});
	FactorySpec.ObjectFactories.Add({
		TEXT("InventoryState"),
		TEXT("InventoryStateClass"),
		TEXT("/Script/CoreUObject.Object"),
		EAvidScriptProjectObjectFactoryKind::NewObject,
		EAvidScriptProjectObjectOwnership::Session,
		EAvidScriptProjectComponentRegistration::None
	});

	FAvidScriptBindingSelectionProfile FactorySelection;
	TArray<FAvidScriptProjectBindingClassSpec> FactoryClassReferences;
	TArray<FAvidScriptProjectObjectFactorySpec> ObjectFactories;
	FString FactorySelectionHash;
	FAvidScriptBindingSelectionResolveResult FactoryResolveResult;
	if (!TestTrue(
			TEXT("Phase51 object-factory identity fixture resolves"),
			FAvidScriptEditorProjectBindingProfile::Resolve(
				FactorySpec,
				FactorySelection,
				FactoryClassReferences,
				ObjectFactories,
				FactorySelectionHash,
				FactoryResolveResult)))
	{
		AddError(FactoryResolveResult.ErrorMessage);
		return false;
	}

	FString FactoryDescriptorJson;
	FAvidScriptBindingSelectionResolveResult FactoryDescriptorSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult FactoryDescriptorResult;
	if (!TestTrue(
			TEXT("Phase51 resolver identity descriptor regenerates"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				FactorySelection,
				{},
				FactoryDescriptorJson,
				FactoryDescriptorSelectionResult,
				FactoryDescriptorResult)))
	{
		AddError(FactoryDescriptorResult.ErrorMessage);
		return false;
	}

	FString FullFactoryDescriptorJson;
	FAvidScriptBindingSelectionResolveResult FullFactoryDescriptorSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult FullFactoryDescriptorResult;
	if (!TestTrue(
			TEXT("Phase51 object-factory descriptor regenerates"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				FactorySelection,
				FactoryClassReferences,
				ObjectFactories,
				FullFactoryDescriptorJson,
				FullFactoryDescriptorSelectionResult,
				FullFactoryDescriptorResult)))
	{
		AddError(FullFactoryDescriptorResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel FullFactoryDescriptorPackage;
	if (!TestTrue(
			TEXT("Phase51 object-factory descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				FullFactoryDescriptorJson,
				FullFactoryDescriptorPackage,
				DescriptorErrorCategory,
				DescriptorErrorSource)))
	{
		AddError(DescriptorErrorCategory + TEXT(":") + DescriptorErrorSource);
		return false;
	}
	TestEqual(
		TEXT("Non-writable object-factory descriptor keeps the Phase51 generator identity"),
		FullFactoryDescriptorPackage.GeneratorVersion,
		FString(TEXT("51.1.0")));

	if (!TestEqual(
		TEXT("Phase51 object-factory fixture retains one class reference"),
		FactoryClassReferences.Num(),
		1))
	{
		return false;
	}
	const FAvidScriptProjectBindingClassSpec& FactoryClassReference =
		FactoryClassReferences[0];
	const FString FactoryClassReferenceIdentity =
		FAvidScriptBindingDescriptorIdentity::MakeClassReferenceIdentity(
			FactoryClassReference.ClassPath,
			FactoryClassReference.BaseClassPath,
			FactoryClassReference.LoadPolicy)
		+ TEXT("|") + FactoryClassReference.ScriptName;
	const FString Phase51FactoryIdentity =
		TEXT("resolver=51.1.0\n")
		TEXT("engine_build_id=") + ModuleManifest.BuildId
		+ TEXT("\npackage=") + FactorySpec.PackageName
		+ TEXT("\nself_class=")
		+ TEXT("\ndescriptor_selection=")
		+ FactoryDescriptorResult.SelectionHash
		+ TEXT("\ndescriptor_package=") + FactoryDescriptorResult.PackageHash
		+ TEXT("\nclass_rule:/Script/Engine.Actor")
		TEXT("|if=K2_GetActorLocation|ef=|ip=|ep=|drp=0")
		+ TEXT("\nclass_reference=") + FactoryClassReferenceIdentity
		+ TEXT("\nobject_factory=InventoryState")
		TEXT("|class_reference=InventoryStateClass")
		TEXT("|kind=new_object|outer=/Script/CoreUObject.Object")
		TEXT("|ownership=session|registration=none");
	TestEqual(
		TEXT("Object-factory profile selection hash retains the Phase51 identity bytes"),
		FactorySelectionHash,
		FAvidScriptHash::Sha256HexUtf8(Phase51FactoryIdentity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileTypedSelfValidationTest,
	"AvidScript.Editor.ProjectBindingProfile.TypedSelfValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileTypedSelfValidationTest::RunTest(const FString& Parameters)
{
	const auto ResolveInvalidSelf = [this](
		const FString& SelfClassPath,
		const FString& ExpectedCategory)
	{
		FAvidScriptProjectBindingProfileSpec Spec;
		Spec.PackageName = TEXT("avidscript.project.typed_self_validation");
		Spec.SelfClassPath = SelfClassPath;
		Spec.Classes.Add(MakeProjectProfileClassRule(
			TEXT("/Script/Engine.Actor"),
			{ TEXT("K2_GetActorLocation") }));
		FAvidScriptBindingSelectionProfile Selection;
		TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
		FString SelectionHash;
		FAvidScriptBindingSelectionResolveResult Result;
		TestFalse(
			FString::Printf(TEXT("Invalid typed self fails: %s"), *ExpectedCategory),
			FAvidScriptEditorProjectBindingProfile::Resolve(
				Spec,
				Selection,
				ClassReferences,
				SelectionHash,
				Result));
		TestEqual(TEXT("Invalid typed self category is stable"), Result.ErrorCategory, ExpectedCategory);
		return true;
	};

	ResolveInvalidSelf(
		TEXT("/Script/Engine.AvidScriptMissingSelfActor"),
		TEXT("self_class_missing"));
	ResolveInvalidSelf(
		TEXT("/Script/Engine.SceneComponent"),
		TEXT("self_class_not_actor"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileEditorOnlyPolicyBoundaryTest,
	"AvidScript.Editor.ProjectBindingProfile.EditorOnlyPolicyBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileEditorOnlyPolicyBoundaryTest::RunTest(const FString& Parameters)
{
	UFunction* EditorOnlyFunction = NewObject<UFunction>(
		GetTransientPackage(),
		TEXT("AvidScriptEditorOnlyProjectBindingFunction"));
	TestNotNull(TEXT("Editor-only policy fixture can be created"), EditorOnlyFunction);
	if (EditorOnlyFunction == nullptr)
	{
		return false;
	}
	EditorOnlyFunction->FunctionFlags = FUNC_BlueprintCallable | FUNC_EditorOnly;

	FString Category;
	FString Source;
	TestFalse(
		TEXT("Project profiles reuse the shared function policy for editor-only members"),
		FAvidScriptEditorReflectedFunctionPolicy::Evaluate(
			EditorOnlyFunction,
			Category,
			Source));
	TestEqual(
		TEXT("Editor-only member rejection category remains stable"),
		Category,
		FString(TEXT("function_not_allowed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileModuleDiscoveryTest,
	"AvidScript.Editor.ProjectBindingProfile.ModuleDiscovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileModuleDiscoveryTest::RunTest(const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec Spec;
	Spec.PackageName = TEXT("avidscript.project.runtime_module");
	Spec.ModulePaths.Add(TEXT("/Script/AvidScriptRuntime"));

	FAvidScriptBindingSelectionProfile Selection;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	FString SelectionHash;
	FAvidScriptBindingSelectionResolveResult Result;
	TestTrue(
		TEXT("Runtime module profile resolves"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			Selection,
			ClassReferences,
			SelectionHash,
			Result));
	TestTrue(
		TEXT("Runtime module discovers AvidScriptComponent"),
		Selection.Classes.ContainsByPredicate([](const FAvidScriptReflectedClassSelection& Rule)
		{
			return Rule.OwnerClassPath == TEXT("/Script/AvidScriptRuntime.AvidScriptComponent");
		}));
	TestTrue(TEXT("Runtime module discovery is non-empty"), Selection.Classes.Num() >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileReusesSelectionPolicyTest,
	"AvidScript.Editor.ProjectBindingProfile.ReusesSelectionPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileReusesSelectionPolicyTest::RunTest(const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec Spec;
	Spec.PackageName = TEXT("avidscript.project.policy");
	Spec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.Actor"),
		{ TEXT("K2_SetActorLocation"), TEXT("K2_GetActorLocation") }));

	FAvidScriptBindingSelectionProfile Selection;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	FString SelectionHash;
	FAvidScriptBindingSelectionResolveResult ProjectResult;
	TestTrue(
		TEXT("Project profile resolves before member policy"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			Selection,
			ClassReferences,
			SelectionHash,
			ProjectResult));

	TArray<FAvidScriptReflectedFunctionSelection> Functions;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	TestTrue(
		TEXT("Existing function selection policy resolves project profile"),
		FAvidScriptEditorBindingSelectionResolver::Resolve(
			Selection,
			Functions,
			SelectionResult));
	TestEqual(TEXT("Existing policy retains one compatible function"), Functions.Num(), 1);
	TestEqual(TEXT("Existing policy rejects one unsupported peer"), SelectionResult.RejectedFunctionCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileClassReferenceValidationTest,
	"AvidScript.Editor.ProjectBindingProfile.ClassReferenceValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileClassReferenceValidationTest::RunTest(const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec MissingClassSpec;
	MissingClassSpec.PackageName = TEXT("avidscript.project.invalid_class");
	MissingClassSpec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.AvidScriptMissingClass"),
		{ TEXT("MissingFunction") }));
	FAvidScriptBindingSelectionProfile MissingClassSelection;
	TArray<FAvidScriptProjectBindingClassSpec> MissingClassReferences;
	FString MissingClassHash;
	FAvidScriptBindingSelectionResolveResult MissingClassResult;
	TestFalse(
		TEXT("Missing reflected class fails closed"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			MissingClassSpec,
			MissingClassSelection,
			MissingClassReferences,
			MissingClassHash,
			MissingClassResult));
	TestEqual(
		TEXT("Missing reflected class category is stable"),
		MissingClassResult.ErrorCategory,
		FString(TEXT("class_missing")));

	const auto ResolveInvalid = [this](
		const FAvidScriptProjectBindingClassSpec& InvalidClassReference,
		const FString& ExpectedCategory)
	{
		FAvidScriptProjectBindingProfileSpec Spec;
		Spec.PackageName = TEXT("avidscript.project.invalid_ref");
		Spec.Classes.Add(MakeProjectProfileClassRule(
			TEXT("/Script/Engine.Actor"),
			{ TEXT("K2_GetActorLocation") }));
		Spec.ClassReferences.Add(InvalidClassReference);
		FAvidScriptBindingSelectionProfile Selection;
		TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
		FString SelectionHash;
		FAvidScriptBindingSelectionResolveResult Result;
		TestFalse(
			FString::Printf(TEXT("Invalid class reference fails: %s"), *ExpectedCategory),
			FAvidScriptEditorProjectBindingProfile::Resolve(
				Spec,
				Selection,
				ClassReferences,
				SelectionHash,
				Result));
		TestEqual(TEXT("Invalid class reference category"), Result.ErrorCategory, ExpectedCategory);
		return true;
	};

	ResolveInvalid(
		{ TEXT("Missing"), TEXT("/Script/Engine.AvidScriptMissing"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") },
		TEXT("class_reference_missing"));
	ResolveInvalid(
		{ TEXT("WrongBase"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Script/Engine.SceneComponent"), TEXT("EditorLoad") },
		TEXT("class_reference_base_mismatch"));
	ResolveInvalid(
		{ TEXT("BadPolicy"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Script/Engine.Actor"), TEXT("RuntimeSearch") },
		TEXT("class_reference_load_policy_invalid"));
	ResolveInvalid(
		{ TEXT("NotActor"), TEXT("/Script/Engine.StaticMeshComponent"), TEXT("/Script/Engine.SceneComponent"), TEXT("EditorLoad") },
		TEXT("class_reference_not_actor"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileDuplicateScriptNameTest,
	"AvidScript.Editor.ProjectBindingProfile.DuplicateScriptName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileDuplicateScriptNameTest::RunTest(const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec Spec;
	Spec.PackageName = TEXT("avidscript.project.duplicate_script_name");
	Spec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.Actor"),
		{ TEXT("K2_GetActorLocation") }));
	Spec.ClassReferences.Add({
		TEXT("SpawnClass"),
		TEXT("/Script/Engine.StaticMeshActor"),
		TEXT("/Script/Engine.Actor"),
		TEXT("EditorLoad")
	});
	Spec.ClassReferences.Add({
		TEXT("SpawnClass"),
		TEXT("/Script/Engine.PointLight"),
		TEXT("/Script/Engine.Actor"),
		TEXT("EditorLoad")
	});

	FAvidScriptBindingSelectionProfile Selection;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	FString SelectionHash;
	FAvidScriptBindingSelectionResolveResult Result;
	TestFalse(
		TEXT("Duplicate class reference script name fails"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			Selection,
			ClassReferences,
			SelectionHash,
			Result));
	TestEqual(
		TEXT("Duplicate script name category is stable"),
		Result.ErrorCategory,
		FString(TEXT("class_reference_script_name_duplicate")));

	Spec.ClassReferences.Reset();
	Spec.ClassReferences.Add({
		TEXT("PrimarySpawnClass"),
		TEXT("/Script/Engine.StaticMeshActor"),
		TEXT("/Script/Engine.Actor"),
		TEXT("EditorLoad")
	});
	Spec.ClassReferences.Add({
		TEXT("AliasSpawnClass"),
		TEXT("/Script/Engine.StaticMeshActor"),
		TEXT("/Script/Engine.Actor"),
		TEXT("EditorLoad")
	});
	TestFalse(
		TEXT("Duplicate stable class reference identity fails"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			Selection,
			ClassReferences,
			SelectionHash,
			Result));
	TestEqual(
		TEXT("Duplicate stable class reference category is stable"),
		Result.ErrorCategory,
		FString(TEXT("class_reference_duplicate")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileObjectFactoryResolutionTest,
	"AvidScript.Editor.ProjectBindingProfile.ObjectFactoryResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileObjectFactoryResolutionTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec Spec;
	Spec.PackageName = TEXT("avidscript.project.object_factory_resolution");
	Spec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.Actor"),
		{ TEXT("K2_GetActorLocation") }));
	Spec.ClassReferences.Add({
		TEXT("SensorComponentClass"),
		TEXT("/Script/Engine.StaticMeshComponent"),
		TEXT("/Script/Engine.ActorComponent"),
		TEXT("EditorLoad")
	});
	Spec.ClassReferences.Add({
		TEXT("InventoryStateClass"),
		TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"),
		TEXT("/Script/CoreUObject.Object"),
		TEXT("EditorLoad")
	});
	Spec.ObjectFactories.Add({
		TEXT("SensorComponent"),
		TEXT("SensorComponentClass"),
		TEXT("/Script/Engine.Actor"),
		EAvidScriptProjectObjectFactoryKind::ActorComponent,
		EAvidScriptProjectObjectOwnership::Session,
		EAvidScriptProjectComponentRegistration::RegisterInstance
	});
	Spec.ObjectFactories.Add({
		TEXT("InventoryState"),
		TEXT("InventoryStateClass"),
		TEXT("/Script/CoreUObject.Object"),
		EAvidScriptProjectObjectFactoryKind::NewObject,
		EAvidScriptProjectObjectOwnership::Session,
		EAvidScriptProjectComponentRegistration::None
	});

	FAvidScriptBindingSelectionProfile Selection;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	TArray<FAvidScriptProjectObjectFactorySpec> ObjectFactories;
	FString SelectionHash;
	FAvidScriptBindingSelectionResolveResult Result;
	TestTrue(
		TEXT("Object factory project profile resolves"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			Selection,
			ClassReferences,
			ObjectFactories,
			SelectionHash,
			Result));
	TestTrue(TEXT("Object factory resolution succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Object factory profile keeps two class references"), ClassReferences.Num(), 2);
	TestEqual(TEXT("Object factory profile keeps two factories"), ObjectFactories.Num(), 2);
	TestTrue(TEXT("Object factory profile hash is SHA-256"), IsProjectProfileSha256(SelectionHash));
	if (ObjectFactories.Num() == 2)
	{
		TestEqual(
			TEXT("Object factories sort by script name"),
			ObjectFactories[0].ScriptName,
			FString(TEXT("InventoryState")));
		TestEqual(
			TEXT("Component factory sorts second"),
			ObjectFactories[1].ScriptName,
			FString(TEXT("SensorComponent")));
	}

	FAvidScriptProjectBindingProfileSpec ReorderedSpec = Spec;
	Algo::Reverse(ReorderedSpec.ClassReferences);
	Algo::Reverse(ReorderedSpec.ObjectFactories);
	FAvidScriptBindingSelectionProfile ReorderedSelection;
	TArray<FAvidScriptProjectBindingClassSpec> ReorderedClassReferences;
	TArray<FAvidScriptProjectObjectFactorySpec> ReorderedObjectFactories;
	FString ReorderedHash;
	FAvidScriptBindingSelectionResolveResult ReorderedResult;
	TestTrue(
		TEXT("Reordered object factory profile resolves"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			ReorderedSpec,
			ReorderedSelection,
			ReorderedClassReferences,
			ReorderedObjectFactories,
			ReorderedHash,
			ReorderedResult));
	TestEqual(
		TEXT("Object factory declaration order does not change selection identity"),
		ReorderedHash,
		SelectionHash);

	FAvidScriptProjectBindingProfileSpec DifferentOuterSpec = Spec;
	DifferentOuterSpec.ObjectFactories[1].OuterBaseClassPath = TEXT("/Script/Engine.Actor");
	FAvidScriptBindingSelectionProfile DifferentOuterSelection;
	TArray<FAvidScriptProjectBindingClassSpec> DifferentOuterClassReferences;
	TArray<FAvidScriptProjectObjectFactorySpec> DifferentOuterFactories;
	FString DifferentOuterHash;
	FAvidScriptBindingSelectionResolveResult DifferentOuterResult;
	TestTrue(
		TEXT("New object factory accepts an Actor outer constraint"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			DifferentOuterSpec,
			DifferentOuterSelection,
			DifferentOuterClassReferences,
			DifferentOuterFactories,
			DifferentOuterHash,
			DifferentOuterResult));
	TestNotEqual(
		TEXT("Object factory outer constraint changes selection identity"),
		DifferentOuterHash,
		SelectionHash);

	FAvidScriptBindingSelectionProfile LegacyOverloadSelection;
	TArray<FAvidScriptProjectBindingClassSpec> LegacyOverloadClassReferences;
	FString LegacyOverloadHash;
	FAvidScriptBindingSelectionResolveResult LegacyOverloadResult;
	TestFalse(
		TEXT("Legacy resolver overload cannot discard object factories"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			LegacyOverloadSelection,
			LegacyOverloadClassReferences,
			LegacyOverloadHash,
			LegacyOverloadResult));
	TestEqual(
		TEXT("Factory-aware resolver requirement category is stable"),
		LegacyOverloadResult.ErrorCategory,
		FString(TEXT("binding_profile_factory_output_required")));

	FAvidScriptProjectBindingProfileSpec WithinMismatchSpec;
	WithinMismatchSpec.PackageName = TEXT("avidscript.project.factory_within_mismatch");
	WithinMismatchSpec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.Actor"),
		{ TEXT("K2_GetActorLocation") }));
	WithinMismatchSpec.ClassReferences.Add({
		TEXT("ConsoleClass"),
		TEXT("/Script/Engine.Console"),
		TEXT("/Script/CoreUObject.Object"),
		TEXT("EditorLoad")
	});
	WithinMismatchSpec.ObjectFactories.Add({
		TEXT("Console"),
		TEXT("ConsoleClass"),
		TEXT("/Script/Engine.Actor"),
		EAvidScriptProjectObjectFactoryKind::NewObject,
		EAvidScriptProjectObjectOwnership::Session,
		EAvidScriptProjectComponentRegistration::None
	});
	FAvidScriptBindingSelectionProfile WithinMismatchSelection;
	TArray<FAvidScriptProjectBindingClassSpec> WithinMismatchClassReferences;
	TArray<FAvidScriptProjectObjectFactorySpec> WithinMismatchFactories;
	FString WithinMismatchHash;
	FAvidScriptBindingSelectionResolveResult WithinMismatchResult;
	TestFalse(
		TEXT("Factory outer must satisfy the UE ClassWithin constraint"),
		FAvidScriptEditorProjectBindingProfile::Resolve(
			WithinMismatchSpec,
			WithinMismatchSelection,
			WithinMismatchClassReferences,
			WithinMismatchFactories,
			WithinMismatchHash,
			WithinMismatchResult));
	TestEqual(
		TEXT("ClassWithin mismatch category is stable"),
		WithinMismatchResult.ErrorCategory,
		FString(TEXT("binding_factory_outer_type_mismatch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProjectBindingProfileNativeDirectIdentityTest,
	"AvidScript.Editor.ProjectBindingProfile.NativeDirectIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProjectBindingProfileNativeDirectIdentityTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptProjectBindingProfileSpec LegacySpec;
	LegacySpec.PackageName = TEXT("avidscript.project.native_direct_identity");
	LegacySpec.Classes.Add(MakeProjectProfileClassRule(
		TEXT("/Script/Engine.Actor"),
		{ TEXT("ActorHasTag"), TEXT("K2_GetActorLocation") }));

	const auto ResolveSpec = [this](
		const FAvidScriptProjectBindingProfileSpec& Spec,
		FAvidScriptBindingSelectionProfile& OutSelection,
		FString& OutHash)
	{
		TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
		FAvidScriptBindingSelectionResolveResult Result;
		const bool bResolved = FAvidScriptEditorProjectBindingProfile::Resolve(
			Spec,
			OutSelection,
			ClassReferences,
			OutHash,
			Result);
		TestTrue(TEXT("Native-direct project profile fixture resolves"), bResolved);
		if (!bResolved)
		{
			AddError(Result.ErrorMessage);
		}
		return bResolved;
	};

	FAvidScriptBindingSelectionProfile LegacySelection;
	FString LegacyHash;
	if (!ResolveSpec(LegacySpec, LegacySelection, LegacyHash))
	{
		return false;
	}
	if (!TestEqual(
			TEXT("Legacy project profile retains one class rule"),
			LegacySelection.Classes.Num(),
			1))
	{
		return false;
	}
	TestTrue(
		TEXT("Legacy project profile omits native-direct authorization"),
		LegacySelection.Classes[0].NativeDirectFunctions.IsEmpty());

	FAvidScriptProjectBindingProfileSpec AuthorizedSpec = LegacySpec;
	AuthorizedSpec.Classes[0].NativeDirectFunctions = {
		TEXT("K2_GetActorLocation"),
		TEXT("ActorHasTag")
	};
	FAvidScriptBindingSelectionProfile AuthorizedSelection;
	FString AuthorizedHash;
	if (!ResolveSpec(AuthorizedSpec, AuthorizedSelection, AuthorizedHash))
	{
		return false;
	}
	if (!TestEqual(
			TEXT("Authorized project profile retains one class rule"),
			AuthorizedSelection.Classes.Num(),
			1)
		|| !TestEqual(
			TEXT("Authorized project profile retains two normalized functions"),
			AuthorizedSelection.Classes[0].NativeDirectFunctions.Num(),
			2))
	{
		return false;
	}
	TestEqual(
		TEXT("Project profile normalizes native-direct function order"),
		AuthorizedSelection.Classes[0].NativeDirectFunctions[0],
		FName(TEXT("ActorHasTag")));
	TestNotEqual(
		TEXT("Adding only native-direct authorization changes selection hash"),
		AuthorizedHash,
		LegacyHash);

	FAvidScriptProjectBindingProfileSpec ReorderedSpec = AuthorizedSpec;
	Algo::Reverse(ReorderedSpec.Classes[0].NativeDirectFunctions);
	FAvidScriptBindingSelectionProfile ReorderedSelection;
	FString ReorderedHash;
	if (!ResolveSpec(ReorderedSpec, ReorderedSelection, ReorderedHash))
	{
		return false;
	}
	TestEqual(
		TEXT("Native-direct authorization order does not change selection hash"),
		ReorderedHash,
		AuthorizedHash);

	FAvidScriptProjectBindingProfileSpec SubsetSpec = AuthorizedSpec;
	SubsetSpec.Classes[0].NativeDirectFunctions = { TEXT("K2_GetActorLocation") };
	FAvidScriptBindingSelectionProfile SubsetSelection;
	FString SubsetHash;
	if (!ResolveSpec(SubsetSpec, SubsetSelection, SubsetHash))
	{
		return false;
	}
	TestNotEqual(
		TEXT("Changing only the native-direct authorization subset changes selection hash"),
		SubsetHash,
		AuthorizedHash);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
