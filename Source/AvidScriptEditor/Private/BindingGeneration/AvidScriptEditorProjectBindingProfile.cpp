#include "AvidScriptEditorProjectBindingProfile.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptHash.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManifest.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace
{
constexpr const TCHAR* LegacyProjectBindingProfileResolverVersion = TEXT("50.1.0");
constexpr const TCHAR* ProjectBindingProfileResolverVersion = TEXT("51.1.0");
constexpr const TCHAR* WritablePropertyProfileResolverVersion = TEXT("52.1.0");
constexpr const TCHAR* NativeDirectProfileResolverVersion = TEXT("54.5.0");
constexpr const TCHAR* GeneratedNativeProfileResolverVersion = TEXT("54.6.0");

void SetProjectProfileFailure(
	FAvidScriptBindingSelectionResolveResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript project binding profile error | category=%s | source=%s | next=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*NextAction);
}

bool IsProjectProfileIdentifier(const FString& Value)
{
	if (Value.IsEmpty() || (!FChar::IsAlpha(Value[0]) && Value[0] != TEXT('_')))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

bool IsScriptModulePath(const FString& ModulePath)
{
	if (!ModulePath.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive)
		|| ModulePath.Len() <= 8
		|| ModulePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 8) != INDEX_NONE)
	{
		return false;
	}
	for (int32 Index = 8; Index < ModulePath.Len(); ++Index)
	{
		const TCHAR Character = ModulePath[Index];
		if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

bool NormalizeNames(
	TArray<FName>& Names,
	const FString& ClassPath,
	const FString& Field,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	Names.Sort([](const FName Left, const FName Right)
	{
		return Left.ToString().Compare(Right.ToString(), ESearchCase::CaseSensitive) < 0;
	});
	for (int32 Index = 0; Index < Names.Num(); ++Index)
	{
		if (Names[Index].IsNone())
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("member_name_empty"),
				ClassPath + TEXT(":") + Field,
				TEXT("Remove empty member names from the project binding profile."));
			return false;
		}
		if (Index > 0 && Names[Index] == Names[Index - 1])
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("duplicate_member_name"),
				ClassPath + TEXT(".") + Names[Index].ToString(),
				TEXT("Keep each include or exclude member name exactly once."));
			return false;
		}
	}
	return true;
}

bool NormalizeClassRule(
	FAvidScriptReflectedClassSelection& Rule,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	UClass* OwnerClass = Rule.OwnerClassPath.IsEmpty()
		? nullptr
		: LoadObject<UClass>(nullptr, *Rule.OwnerClassPath);
	if (OwnerClass == nullptr)
	{
		SetProjectProfileFailure(
			OutResult,
			TEXT("class_missing"),
			Rule.OwnerClassPath,
			TEXT("Use a reflected UClass path available in the active UE5.8 build."));
		return false;
	}
	Rule.OwnerClassPath = OwnerClass->GetPathName();
	if (!NormalizeNames(Rule.IncludeFunctions, Rule.OwnerClassPath, TEXT("include_functions"), OutResult)
		|| !NormalizeNames(Rule.ExcludeFunctions, Rule.OwnerClassPath, TEXT("exclude_functions"), OutResult)
		|| !NormalizeNames(Rule.NativeDirectFunctions, Rule.OwnerClassPath, TEXT("native_direct_functions"), OutResult)
		|| !NormalizeNames(Rule.GeneratedNativeFunctions, Rule.OwnerClassPath, TEXT("generated_native_functions"), OutResult)
		|| !NormalizeNames(Rule.IncludeProperties, Rule.OwnerClassPath, TEXT("include_properties"), OutResult)
		|| !NormalizeNames(Rule.ExcludeProperties, Rule.OwnerClassPath, TEXT("exclude_properties"), OutResult)
		|| !NormalizeNames(Rule.WritableProperties, Rule.OwnerClassPath, TEXT("writable_properties"), OutResult)
		|| !NormalizeNames(Rule.GeneratedNativeProperties, Rule.OwnerClassPath, TEXT("generated_native_properties"), OutResult))
	{
		return false;
	}
	for (const FName Name : Rule.IncludeFunctions)
	{
		if (Rule.ExcludeFunctions.Contains(Name))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("member_filter_conflict"),
				Rule.OwnerClassPath + TEXT(".") + Name.ToString(),
				TEXT("Remove members that appear in both include and exclude filters."));
			return false;
		}
	}
	for (const FName Name : Rule.GeneratedNativeFunctions)
	{
		if (Name.ToString().Contains(TEXT("*"))
			|| Name.ToString().Contains(TEXT("?")))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("generated_native_wildcard_unsupported"),
				Rule.OwnerClassPath + TEXT(".") + Name.ToString(),
				TEXT("Select each generated native function by exact reflected name."));
			return false;
		}
		if (!Rule.IncludeFunctions.Contains(Name))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("generated_native_not_selected"),
				Rule.OwnerClassPath + TEXT(".") + Name.ToString(),
				TEXT("Add generated native functions to functions before granting S1 generation."));
			return false;
		}
		if (Rule.NativeDirectFunctions.Contains(Name))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("generated_native_dispatch_conflict"),
				Rule.OwnerClassPath + TEXT(".") + Name.ToString(),
				TEXT("Choose either native_direct_functions or generated_native_functions for a callable."));
			return false;
		}
	}
	for (const FName Name : Rule.IncludeProperties)
	{
		if (Rule.ExcludeProperties.Contains(Name))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("member_filter_conflict"),
				Rule.OwnerClassPath + TEXT(".") + Name.ToString(),
				TEXT("Remove properties that appear in both include and exclude filters."));
			return false;
		}
	}
	for (const FName Name : Rule.WritableProperties)
	{
		if (Rule.ExcludeProperties.Contains(Name))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("member_filter_conflict"),
				Rule.OwnerClassPath + TEXT(".") + Name.ToString(),
				TEXT("Remove writable properties from the property exclude filter."));
			return false;
		}
	}
	for (const FName Name : Rule.GeneratedNativeProperties)
	{
		if (Name.ToString().Contains(TEXT("*"))
			|| Name.ToString().Contains(TEXT("?")))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("generated_native_property_wildcard_unsupported"),
				Rule.OwnerClassPath + TEXT(".") + Name.ToString(),
				TEXT("Select each generated native property by exact reflected name."));
			return false;
		}
		if (!Rule.IncludeProperties.Contains(Name))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("generated_native_property_not_selected"),
				Rule.OwnerClassPath + TEXT(".") + Name.ToString(),
				TEXT("Add generated native properties to include_properties before granting S1 generation."));
			return false;
		}
	}
	return true;
}

bool NormalizeSelfClassPath(
	FString& SelfClassPath,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	if (SelfClassPath.IsEmpty())
	{
		return true;
	}

	UClass* SelfClass = LoadObject<UClass>(nullptr, *SelfClassPath);
	if (SelfClass == nullptr)
	{
		SetProjectProfileFailure(
			OutResult,
			TEXT("self_class_missing"),
			SelfClassPath,
			TEXT("Use a loadable AActor-derived UClass path for binding_profile.self_class_path."));
		return false;
	}
	if (!SelfClass->IsChildOf(AActor::StaticClass()))
	{
		SetProjectProfileFailure(
			OutResult,
			TEXT("self_class_not_actor"),
			SelfClass->GetPathName(),
			TEXT("Use an AActor-derived UClass path for binding_profile.self_class_path."));
		return false;
	}

	SelfClassPath = SelfClass->GetPathName();
	return true;
}

FString MakeClassReferenceIdentity(const FAvidScriptProjectBindingClassSpec& Spec)
{
	return FAvidScriptBindingDescriptorIdentity::MakeClassReferenceIdentity(
		Spec.ClassPath,
		Spec.BaseClassPath,
		Spec.LoadPolicy);
}

FString MakeClassReferenceDeclarationIdentity(const FAvidScriptProjectBindingClassSpec& Spec)
{
	return MakeClassReferenceIdentity(Spec)
		+ TEXT("|") + Spec.ScriptName;
}

const TCHAR* GetProjectObjectFactoryKindToken(
	const EAvidScriptProjectObjectFactoryKind Kind)
{
	switch (Kind)
	{
	case EAvidScriptProjectObjectFactoryKind::NewObject:
		return TEXT("new_object");
	case EAvidScriptProjectObjectFactoryKind::ActorComponent:
		return TEXT("actor_component");
	default:
		return TEXT("<invalid>");
	}
}

const TCHAR* GetProjectObjectOwnershipToken(
	const EAvidScriptProjectObjectOwnership Ownership)
{
	return Ownership == EAvidScriptProjectObjectOwnership::Session
		? TEXT("session")
		: TEXT("<invalid>");
}

const TCHAR* GetProjectComponentRegistrationToken(
	const EAvidScriptProjectComponentRegistration Registration)
{
	switch (Registration)
	{
	case EAvidScriptProjectComponentRegistration::None:
		return TEXT("none");
	case EAvidScriptProjectComponentRegistration::RegisterInstance:
		return TEXT("register_instance");
	default:
		return TEXT("<invalid>");
	}
}

FString MakeObjectFactoryDeclarationIdentity(
	const FAvidScriptProjectObjectFactorySpec& Factory)
{
	return Factory.ScriptName
		+ TEXT("|class_reference=") + Factory.ClassReference
		+ TEXT("|kind=") + GetProjectObjectFactoryKindToken(Factory.Kind)
		+ TEXT("|outer=") + Factory.OuterBaseClassPath
		+ TEXT("|ownership=") + GetProjectObjectOwnershipToken(Factory.Ownership)
		+ TEXT("|registration=")
		+ GetProjectComponentRegistrationToken(Factory.Registration);
}

bool TryGetEngineBuildIdentity(
	FString& OutBuildIdentity,
	FString& OutManifestPath)
{
	OutBuildIdentity.Reset();
	FModuleManifest ModuleManifest;
	OutManifestPath = FModuleManifest::GetFileName(
		FPlatformProcess::GetModulesDirectory(),
		false);
	if (FModuleManifest::TryRead(OutManifestPath, ModuleManifest)
		&& !ModuleManifest.BuildId.IsEmpty())
	{
		OutBuildIdentity = ModuleManifest.BuildId;
		return true;
	}
	return false;
}

void AppendRuleIdentity(
	const FAvidScriptReflectedClassSelection& Rule,
	const bool bIncludeWritableProperties,
	const bool bIncludeNativeDirectFunctions,
	const bool bIncludeGeneratedNativeFunctions,
	const bool bIncludeGeneratedNativeProperties,
	TArray<FString>& OutIdentity)
{
	const auto JoinNames = [](const TArray<FName>& Names)
	{
		TArray<FString> Values;
		Values.Reserve(Names.Num());
		for (const FName Name : Names)
		{
			Values.Add(Name.ToString());
		}
		return FString::Join(Values, TEXT(","));
	};
	FString Identity =
		TEXT("class_rule:") + Rule.OwnerClassPath
		+ TEXT("|if=") + JoinNames(Rule.IncludeFunctions)
		+ TEXT("|ef=") + JoinNames(Rule.ExcludeFunctions)
		+ TEXT("|ip=") + JoinNames(Rule.IncludeProperties)
		+ TEXT("|ep=") + JoinNames(Rule.ExcludeProperties);
	if (bIncludeWritableProperties)
	{
		Identity += TEXT("|wp=") + JoinNames(Rule.WritableProperties);
	}
	if (bIncludeNativeDirectFunctions)
	{
		Identity += TEXT("|ndf=") + JoinNames(Rule.NativeDirectFunctions);
	}
	if (bIncludeGeneratedNativeFunctions)
	{
		Identity += TEXT("|gnf=") + JoinNames(Rule.GeneratedNativeFunctions);
	}
	if (bIncludeGeneratedNativeProperties)
	{
		Identity += TEXT("|gnp=") + JoinNames(Rule.GeneratedNativeProperties);
	}
	Identity += FString::Printf(
		TEXT("|drp=%d"),
		Rule.bDiscoverReadableProperties ? 1 : 0);
	OutIdentity.Add(MoveTemp(Identity));
}
} // namespace

const TCHAR* FAvidScriptEditorProjectBindingProfile::GetResolverVersion()
{
	return ProjectBindingProfileResolverVersion;
}

bool FAvidScriptEditorProjectBindingProfile::Resolve(
	const FAvidScriptProjectBindingProfileSpec& Spec,
	FAvidScriptBindingSelectionProfile& OutSelection,
	TArray<FAvidScriptProjectBindingClassSpec>& OutClassReferences,
	FString& OutSelectionHash,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	if (!Spec.ObjectFactories.IsEmpty())
	{
		OutSelection = FAvidScriptBindingSelectionProfile();
		OutClassReferences.Empty();
		OutSelectionHash.Reset();
		OutResult = FAvidScriptBindingSelectionResolveResult();
		SetProjectProfileFailure(
			OutResult,
			TEXT("binding_profile_factory_output_required"),
			Spec.PackageName,
			TEXT("Use the factory-aware Resolve overload for profiles that declare object_factories."));
		return false;
	}

	TArray<FAvidScriptProjectObjectFactorySpec> ObjectFactories;
	return Resolve(
		Spec,
		OutSelection,
		OutClassReferences,
		ObjectFactories,
		OutSelectionHash,
		OutResult);
}

bool FAvidScriptEditorProjectBindingProfile::Resolve(
	const FAvidScriptProjectBindingProfileSpec& Spec,
	FAvidScriptBindingSelectionProfile& OutSelection,
	TArray<FAvidScriptProjectBindingClassSpec>& OutClassReferences,
	TArray<FAvidScriptProjectObjectFactorySpec>& OutObjectFactories,
	FString& OutSelectionHash,
	FAvidScriptBindingSelectionResolveResult& OutResult)
{
	OutSelection = FAvidScriptBindingSelectionProfile();
	OutClassReferences.Empty();
	OutObjectFactories.Empty();
	OutSelectionHash.Reset();
	OutResult = FAvidScriptBindingSelectionResolveResult();
	if (Spec.PackageName.IsEmpty())
	{
		SetProjectProfileFailure(
			OutResult,
			TEXT("package_name_missing"),
			Spec.PackageName,
			TEXT("Provide a stable non-empty project binding package name."));
		return false;
	}
	if (Spec.ModulePaths.IsEmpty()
		&& Spec.Classes.IsEmpty()
		&& Spec.ObjectFactories.IsEmpty())
	{
		SetProjectProfileFailure(
			OutResult,
			TEXT("profile_empty"),
			Spec.PackageName,
			TEXT("Declare at least one /Script module or explicit reflected class."));
		return false;
	}
	FString SelfClassPath = Spec.SelfClassPath;
	if (!NormalizeSelfClassPath(SelfClassPath, OutResult))
	{
		return false;
	}
	FString EngineBuildIdentity;
	FString EngineModuleManifestPath;
	if (!TryGetEngineBuildIdentity(
			EngineBuildIdentity,
			EngineModuleManifestPath))
	{
		SetProjectProfileFailure(
			OutResult,
			TEXT("engine_build_id_unavailable"),
			EngineModuleManifestPath,
			TEXT("Build or repair the UE5.8 Editor module manifest before resolving project bindings."));
		return false;
	}

	TMap<FString, FAvidScriptReflectedClassSelection> RulesByPath;
	TArray<FString> ModulePaths = Spec.ModulePaths;
	ModulePaths.Sort();
	for (int32 Index = 0; Index < ModulePaths.Num(); ++Index)
	{
		const FString& ModulePath = ModulePaths[Index];
		if (!IsScriptModulePath(ModulePath))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("module_path_invalid"),
				ModulePath,
				TEXT("Use a canonical /Script/ModuleName path."));
			return false;
		}
		if (Index > 0 && ModulePath == ModulePaths[Index - 1])
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("duplicate_module_path"),
				ModulePath,
				TEXT("Keep each module path exactly once."));
			return false;
		}

		int32 DiscoveredClassCount = 0;
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Class = *ClassIt;
			if (Class == nullptr
				|| Class->GetOutermost()->GetName() != ModulePath
				|| Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			FAvidScriptReflectedClassSelection Rule;
			Rule.OwnerClassPath = Class->GetPathName();
			RulesByPath.Add(Rule.OwnerClassPath, MoveTemp(Rule));
			++DiscoveredClassCount;
		}
		if (DiscoveredClassCount == 0)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("module_classes_missing"),
				ModulePath,
				TEXT("Load the module and verify it contains reflected UClass types."));
			return false;
		}
	}

	TSet<FString> ExplicitClassPaths;
	for (FAvidScriptReflectedClassSelection Rule : Spec.Classes)
	{
		if (!NormalizeClassRule(Rule, OutResult))
		{
			return false;
		}
		if (ExplicitClassPaths.Contains(Rule.OwnerClassPath))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("duplicate_class_rule"),
				Rule.OwnerClassPath,
				TEXT("Keep each explicit reflected class path exactly once."));
			return false;
		}
		ExplicitClassPaths.Add(Rule.OwnerClassPath);
		RulesByPath.Add(Rule.OwnerClassPath, MoveTemp(Rule));
	}

	TSet<FString> FactoryScriptNames;
	TSet<FString> FactoryClassReferenceNames;
	for (const FAvidScriptProjectObjectFactorySpec& Factory : Spec.ObjectFactories)
	{
		if (!IsProjectProfileIdentifier(Factory.ScriptName))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_profile_factory_script_name_invalid"),
				Factory.ScriptName,
				TEXT("Use a unique C# identifier for each object factory."));
			return false;
		}
		if (FactoryScriptNames.Contains(Factory.ScriptName))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_profile_factory_script_name_duplicate"),
				Factory.ScriptName,
				TEXT("Keep each object factory script_name exactly once."));
			return false;
		}
		if (!IsProjectProfileIdentifier(Factory.ClassReference))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_profile_factory_class_reference_invalid"),
				Factory.ClassReference,
				TEXT("Reference a class_references script_name identifier."));
			return false;
		}
		if (Factory.Kind != EAvidScriptProjectObjectFactoryKind::NewObject
			&& Factory.Kind != EAvidScriptProjectObjectFactoryKind::ActorComponent)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_profile_factory_kind_invalid"),
				Factory.ScriptName,
				TEXT("Use new_object or actor_component."));
			return false;
		}
		if (Factory.Ownership != EAvidScriptProjectObjectOwnership::Session)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_profile_factory_ownership_invalid"),
				Factory.ScriptName,
				TEXT("Use session ownership."));
			return false;
		}
		if (Factory.Registration != EAvidScriptProjectComponentRegistration::None
			&& Factory.Registration
				!= EAvidScriptProjectComponentRegistration::RegisterInstance)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_profile_factory_registration_invalid"),
				Factory.ScriptName,
				TEXT("Use none or register_instance."));
			return false;
		}

		FactoryScriptNames.Add(Factory.ScriptName);
		FactoryClassReferenceNames.Add(Factory.ClassReference);
	}

	TSet<FString> ClassReferenceScriptNames;
	TSet<FString> ClassReferenceIdentities;
	for (FAvidScriptProjectBindingClassSpec ClassReference : Spec.ClassReferences)
	{
		if (!IsProjectProfileIdentifier(ClassReference.ScriptName))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_script_name_invalid"),
				ClassReference.ScriptName,
				TEXT("Use a unique C# identifier for each class reference."));
			return false;
		}
		if (ClassReferenceScriptNames.Contains(ClassReference.ScriptName))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_script_name_duplicate"),
				ClassReference.ScriptName,
				TEXT("Keep each class reference script_name exactly once."));
			return false;
		}
		if (ClassReference.LoadPolicy != TEXT("EditorLoad")
			&& ClassReference.LoadPolicy != TEXT("CookRequired"))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_load_policy_invalid"),
				ClassReference.LoadPolicy,
				TEXT("Use EditorLoad or CookRequired."));
			return false;
		}

		UClass* Class = LoadObject<UClass>(nullptr, *ClassReference.ClassPath);
		UClass* BaseClass = LoadObject<UClass>(nullptr, *ClassReference.BaseClassPath);
		if (Class == nullptr || BaseClass == nullptr)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_missing"),
				Class == nullptr ? ClassReference.ClassPath : ClassReference.BaseClassPath,
				TEXT("Use loadable UClass paths for class_path and base_class_path."));
			return false;
		}
		if (!Class->IsChildOf(BaseClass))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_base_mismatch"),
				ClassReference.ClassPath + TEXT(" -> ") + ClassReference.BaseClassPath,
				TEXT("Choose a base_class_path that owns the referenced class."));
			return false;
		}
		const bool bUsedByObjectFactory =
			FactoryClassReferenceNames.Contains(ClassReference.ScriptName);
		if ((!Class->IsChildOf(AActor::StaticClass())
				|| !BaseClass->IsChildOf(AActor::StaticClass()))
			&& !bUsedByObjectFactory)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_not_actor"),
				ClassReference.ClassPath + TEXT(" -> ") + ClassReference.BaseClassPath,
				TEXT("Use an AActor-derived class and AActor-derived base constraint for SpawnActor."));
			return false;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract) && !bUsedByObjectFactory)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_abstract"),
				ClassReference.ClassPath,
				TEXT("Choose a concrete class that can be spawned."));
			return false;
		}
		ClassReference.ClassPath = Class->GetPathName();
		ClassReference.BaseClassPath = BaseClass->GetPathName();

		const FString Identity = MakeClassReferenceIdentity(ClassReference);
		if (ClassReferenceIdentities.Contains(Identity))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_duplicate"),
				Identity,
				TEXT("Remove duplicate class reference declarations."));
			return false;
		}
		ClassReferenceScriptNames.Add(ClassReference.ScriptName);
		ClassReferenceIdentities.Add(Identity);
		OutClassReferences.Add(ClassReference);
	}
	OutClassReferences.Sort([](
		const FAvidScriptProjectBindingClassSpec& Left,
		const FAvidScriptProjectBindingClassSpec& Right)
	{
		return MakeClassReferenceIdentity(Left).Compare(
			MakeClassReferenceIdentity(Right),
			ESearchCase::CaseSensitive) < 0;
	});

	TMap<FString, FAvidScriptProjectBindingClassSpec> ClassReferencesByScriptName;
	for (const FAvidScriptProjectBindingClassSpec& ClassReference : OutClassReferences)
	{
		ClassReferencesByScriptName.Add(ClassReference.ScriptName, ClassReference);
	}

	for (FAvidScriptProjectObjectFactorySpec Factory : Spec.ObjectFactories)
	{
		const FAvidScriptProjectBindingClassSpec* ClassReference =
			ClassReferencesByScriptName.Find(Factory.ClassReference);
		if (ClassReference == nullptr)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_profile_factory_class_reference_missing"),
				Factory.ClassReference,
				TEXT("Reference a class_references entry from the same binding profile."));
			return false;
		}

		UClass* ObjectClass = LoadObject<UClass>(nullptr, *ClassReference->ClassPath);
		UClass* OuterClass = Factory.OuterBaseClassPath.IsEmpty()
			? nullptr
			: LoadObject<UClass>(nullptr, *Factory.OuterBaseClassPath);
		if (ObjectClass == nullptr)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_profile_factory_class_reference_missing"),
				ClassReference->ClassPath,
				TEXT("Use a loadable class reference for the object factory."));
			return false;
		}
		if (OuterClass == nullptr)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_factory_outer_type_mismatch"),
				Factory.OuterBaseClassPath,
				TEXT("Use a loadable UObject-derived UClass as outer_base_class_path."));
			return false;
		}
		if (ObjectClass->HasAnyClassFlags(CLASS_Abstract))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_factory_class_abstract"),
				ObjectClass->GetPathName(),
				TEXT("Choose a concrete UObject class for the factory."));
			return false;
		}
		if (ObjectClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_factory_class_deprecated"),
				ObjectClass->GetPathName(),
				TEXT("Choose a current non-deprecated UObject class for the factory."));
			return false;
		}
		if (ObjectClass->ClassWithin != nullptr
			&& !OuterClass->IsChildOf(ObjectClass->ClassWithin))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_factory_outer_type_mismatch"),
				OuterClass->GetPathName() + TEXT(" -> ")
					+ ObjectClass->ClassWithin->GetPathName(),
				TEXT("Choose an outer_base_class_path compatible with the factory class Within constraint."));
			return false;
		}

		const bool bIsActorComponent =
			ObjectClass->IsChildOf(UActorComponent::StaticClass());
		const bool bIsActor = ObjectClass->IsChildOf(AActor::StaticClass());
		if ((Factory.Kind == EAvidScriptProjectObjectFactoryKind::NewObject
				&& (bIsActorComponent || bIsActor))
			|| (Factory.Kind == EAvidScriptProjectObjectFactoryKind::ActorComponent
				&& !bIsActorComponent))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_factory_kind_mismatch"),
				ObjectClass->GetPathName(),
				TEXT("Match new_object to a non-Actor UObject or actor_component to a UActorComponent class."));
			return false;
		}
		if (Factory.Kind == EAvidScriptProjectObjectFactoryKind::ActorComponent
			&& !OuterClass->IsChildOf(AActor::StaticClass()))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_factory_outer_type_mismatch"),
				OuterClass->GetPathName(),
				TEXT("Use an AActor-derived outer_base_class_path for actor_component."));
			return false;
		}

		const bool bRegistrationMatches =
			(Factory.Kind == EAvidScriptProjectObjectFactoryKind::NewObject
				&& Factory.Registration == EAvidScriptProjectComponentRegistration::None)
			|| (Factory.Kind == EAvidScriptProjectObjectFactoryKind::ActorComponent
				&& Factory.Registration
					== EAvidScriptProjectComponentRegistration::RegisterInstance);
		if (!bRegistrationMatches)
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("binding_factory_registration_mismatch"),
				Factory.ScriptName,
				TEXT("Use none for new_object and register_instance for actor_component."));
			return false;
		}

		Factory.OuterBaseClassPath = OuterClass->GetPathName();
		OutObjectFactories.Add(MoveTemp(Factory));
	}
	OutObjectFactories.Sort([](
		const FAvidScriptProjectObjectFactorySpec& Left,
		const FAvidScriptProjectObjectFactorySpec& Right)
	{
		return Left.ScriptName.Compare(
			Right.ScriptName,
			ESearchCase::CaseSensitive) < 0;
	});

	TArray<FString> ClassPaths;
	RulesByPath.GetKeys(ClassPaths);
	ClassPaths.Sort();
	OutSelection.PackageName = Spec.PackageName;
	OutSelection.SelfClassPath = MoveTemp(SelfClassPath);
	for (const FString& ClassPath : ClassPaths)
	{
		OutSelection.Classes.Add(RulesByPath.FindChecked(ClassPath));
	}

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	TArray<FAvidScriptProjectBindingClassSpec> SpawnClassReferences;
	for (const FAvidScriptProjectBindingClassSpec& ClassReference : OutClassReferences)
	{
		UClass* Class = LoadObject<UClass>(nullptr, *ClassReference.ClassPath);
		UClass* BaseClass = LoadObject<UClass>(nullptr, *ClassReference.BaseClassPath);
		if (Class != nullptr
			&& BaseClass != nullptr
			&& Class->IsChildOf(AActor::StaticClass())
			&& BaseClass->IsChildOf(AActor::StaticClass()))
		{
			SpawnClassReferences.Add(ClassReference);
		}
	}
	if (!FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			OutSelection,
			SpawnClassReferences,
			DescriptorJson,
			SelectionResult,
			DescriptorResult))
	{
		OutResult = MoveTemp(SelectionResult);
		if (OutResult.ErrorCategory.IsEmpty())
		{
			SetProjectProfileFailure(
				OutResult,
				DescriptorResult.ErrorCategory,
				DescriptorResult.ErrorSource,
				DescriptorResult.NextAction);
		}
		return false;
	}
	OutResult = MoveTemp(SelectionResult);

	TArray<FString> Identity;
	const bool bHasWritableProperties = OutSelection.Classes.ContainsByPredicate(
		[](const FAvidScriptReflectedClassSelection& Rule)
		{
			return !Rule.WritableProperties.IsEmpty();
		});
	const bool bHasNativeDirectFunctions = OutSelection.Classes.ContainsByPredicate(
		[](const FAvidScriptReflectedClassSelection& Rule)
		{
			return !Rule.NativeDirectFunctions.IsEmpty();
		});
	const bool bHasGeneratedNativeFunctions = OutSelection.Classes.ContainsByPredicate(
		[](const FAvidScriptReflectedClassSelection& Rule)
		{
			return !Rule.GeneratedNativeFunctions.IsEmpty();
		});
	const bool bHasGeneratedNativeProperties = OutSelection.Classes.ContainsByPredicate(
		[](const FAvidScriptReflectedClassSelection& Rule)
		{
			return !Rule.GeneratedNativeProperties.IsEmpty();
		});
	Identity.Add(
		TEXT("resolver=")
		+ FString(bHasGeneratedNativeFunctions || bHasGeneratedNativeProperties
			? GeneratedNativeProfileResolverVersion
			: bHasNativeDirectFunctions
			? NativeDirectProfileResolverVersion
			: bHasWritableProperties
				? WritablePropertyProfileResolverVersion
				: Spec.ObjectFactories.IsEmpty()
					? LegacyProjectBindingProfileResolverVersion
					: ProjectBindingProfileResolverVersion));
	Identity.Add(TEXT("engine_build_id=") + EngineBuildIdentity);
	Identity.Add(TEXT("package=") + Spec.PackageName);
	Identity.Add(TEXT("self_class=") + OutSelection.SelfClassPath);
	Identity.Add(TEXT("descriptor_selection=") + DescriptorResult.SelectionHash);
	Identity.Add(TEXT("descriptor_package=") + DescriptorResult.PackageHash);
	for (const FString& ModulePath : ModulePaths)
	{
		Identity.Add(TEXT("module=") + ModulePath);
	}
	for (const FAvidScriptReflectedClassSelection& Rule : OutSelection.Classes)
	{
		AppendRuleIdentity(
			Rule,
			bHasWritableProperties,
			bHasNativeDirectFunctions,
			bHasGeneratedNativeFunctions,
			bHasGeneratedNativeProperties,
			Identity);
	}
	for (const FAvidScriptProjectBindingClassSpec& ClassReference : OutClassReferences)
	{
		Identity.Add(
			TEXT("class_reference=")
			+ MakeClassReferenceDeclarationIdentity(ClassReference));
	}
	for (const FAvidScriptProjectObjectFactorySpec& Factory : OutObjectFactories)
	{
		Identity.Add(
			TEXT("object_factory=")
			+ MakeObjectFactoryDeclarationIdentity(Factory));
	}
	OutSelectionHash = FAvidScriptHash::Sha256HexUtf8(FString::Join(Identity, TEXT("\n")));

	OutResult.bSucceeded = true;
	return true;
}
