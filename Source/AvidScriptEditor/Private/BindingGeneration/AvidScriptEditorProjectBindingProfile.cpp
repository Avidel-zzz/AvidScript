#include "AvidScriptEditorProjectBindingProfile.h"

#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptHash.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManifest.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace
{
constexpr const TCHAR* ProjectBindingProfileResolverVersion = TEXT("49.1.0");

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
		|| !NormalizeNames(Rule.IncludeProperties, Rule.OwnerClassPath, TEXT("include_properties"), OutResult)
		|| !NormalizeNames(Rule.ExcludeProperties, Rule.OwnerClassPath, TEXT("exclude_properties"), OutResult))
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
	return true;
}

FString MakeClassReferenceIdentity(const FAvidScriptProjectBindingClassSpec& Spec)
{
	return Spec.ClassPath
		+ TEXT("|") + Spec.BaseClassPath
		+ TEXT("|") + Spec.LoadPolicy;
}

FString MakeClassReferenceDeclarationIdentity(const FAvidScriptProjectBindingClassSpec& Spec)
{
	return MakeClassReferenceIdentity(Spec)
		+ TEXT("|") + Spec.ScriptName;
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

void AppendRuleIdentity(const FAvidScriptReflectedClassSelection& Rule, TArray<FString>& OutIdentity)
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
	OutIdentity.Add(
		TEXT("class_rule:") + Rule.OwnerClassPath
		+ TEXT("|if=") + JoinNames(Rule.IncludeFunctions)
		+ TEXT("|ef=") + JoinNames(Rule.ExcludeFunctions)
		+ TEXT("|ip=") + JoinNames(Rule.IncludeProperties)
		+ TEXT("|ep=") + JoinNames(Rule.ExcludeProperties)
		+ FString::Printf(TEXT("|drp=%d"), Rule.bDiscoverReadableProperties ? 1 : 0));
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
	OutSelection = FAvidScriptBindingSelectionProfile();
	OutClassReferences.Empty();
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
	if (Spec.ModulePaths.IsEmpty() && Spec.Classes.IsEmpty())
	{
		SetProjectProfileFailure(
			OutResult,
			TEXT("profile_empty"),
			Spec.PackageName,
			TEXT("Declare at least one /Script module or explicit reflected class."));
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
		if (!Class->IsChildOf(AActor::StaticClass())
			|| !BaseClass->IsChildOf(AActor::StaticClass()))
		{
			SetProjectProfileFailure(
				OutResult,
				TEXT("class_reference_not_actor"),
				ClassReference.ClassPath + TEXT(" -> ") + ClassReference.BaseClassPath,
				TEXT("Use an AActor-derived class and AActor-derived base constraint for SpawnActor."));
			return false;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
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

	TArray<FString> ClassPaths;
	RulesByPath.GetKeys(ClassPaths);
	ClassPaths.Sort();
	OutSelection.PackageName = Spec.PackageName;
	for (const FString& ClassPath : ClassPaths)
	{
		OutSelection.Classes.Add(RulesByPath.FindChecked(ClassPath));
	}

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	if (!FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			OutSelection,
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
	Identity.Add(TEXT("resolver=") + FString(ProjectBindingProfileResolverVersion));
	Identity.Add(TEXT("engine_build_id=") + EngineBuildIdentity);
	Identity.Add(TEXT("package=") + Spec.PackageName);
	Identity.Add(TEXT("descriptor_selection=") + DescriptorResult.SelectionHash);
	Identity.Add(TEXT("descriptor_package=") + DescriptorResult.PackageHash);
	for (const FString& ModulePath : ModulePaths)
	{
		Identity.Add(TEXT("module=") + ModulePath);
	}
	for (const FAvidScriptReflectedClassSelection& Rule : OutSelection.Classes)
	{
		AppendRuleIdentity(Rule, Identity);
	}
	for (const FAvidScriptProjectBindingClassSpec& ClassReference : OutClassReferences)
	{
		Identity.Add(
			TEXT("class_reference=")
			+ MakeClassReferenceDeclarationIdentity(ClassReference));
	}
	OutSelectionHash = FAvidScriptHash::Sha256HexUtf8(FString::Join(Identity, TEXT("\n")));

	OutResult.bSucceeded = true;
	return true;
}
