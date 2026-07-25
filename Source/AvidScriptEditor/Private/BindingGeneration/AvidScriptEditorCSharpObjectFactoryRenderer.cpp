#include "BindingGeneration/AvidScriptEditorCSharpObjectFactoryRenderer.h"

#include "AvidScriptObjectFactoryBinding.h"
#include "BindingGeneration/AvidScriptEditorCSharpSyntax.h"

namespace
{
const FAvidScriptBindingTypeModel* FindObjectType(
	const FAvidScriptBindingPackageModel& Package,
	const FString& ClassPath)
{
	return Package.Types.FindByPredicate(
		[&ClassPath](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.CanonicalType == TEXT("object:") + ClassPath;
		});
}

const FAvidScriptBindingTypeModel* FindTypeById(
	const FAvidScriptBindingPackageModel& Package,
	const FString& StableId)
{
	return Package.Types.FindByPredicate(
		[&StableId](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.StableId == StableId;
		});
}
} // namespace

bool FAvidScriptEditorCSharpObjectFactoryRenderer::ValidateBindingContract(
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	bool bHasConstruct = false;
	bool bHasRelease = false;
	bool bHasFindComponent = false;
	const TConstArrayView<FAvidScriptObjectFactoryBindingSpec> Specs =
		FAvidScriptObjectFactoryBinding::GetSpecs();
	for (const FAvidScriptObjectFactoryBindingSpec& Spec : Specs)
	{
		switch (Spec.Kind)
		{
		case EAvidScriptBindingInvocationKind::ObjectConstruct:
			bHasConstruct = !bHasConstruct && Spec.Signature == TEXT("(iii)I");
			break;
		case EAvidScriptBindingInvocationKind::ObjectRelease:
			bHasRelease = !bHasRelease && Spec.Signature == TEXT("(ii)i");
			break;
		case EAvidScriptBindingInvocationKind::ActorFindComponent:
			bHasFindComponent = !bHasFindComponent
				&& Spec.Signature == TEXT("(iii)I");
			break;
		default:
			OutErrorCategory = TEXT("object_factory_binding_contract_invalid");
			OutErrorSource = Spec.StableId;
			return false;
		}
	}
	if (Specs.Num() == 3 && bHasConstruct && bHasRelease && bHasFindComponent)
	{
		return true;
	}
	OutErrorCategory = TEXT("object_factory_binding_contract_invalid");
	OutErrorSource = TEXT("missing_required_kind");
	return false;
}

bool FAvidScriptEditorCSharpObjectFactoryRenderer::BuildSurfaces(
	const FAvidScriptBindingPackageModel& Package,
	TSet<FString>& InOutCSharpTypeNames,
	TArray<FAvidScriptEditorCSharpObjectFactorySurface>& OutSurfaces,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	OutSurfaces.Reset();
	if (Package.ObjectFactories.IsEmpty())
	{
		return true;
	}
	if (Package.SchemaVersion < 7)
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = TEXT("object_factories");
		return false;
	}
	const bool bHasComponentFactories = Package.ObjectFactories.ContainsByPredicate(
		[](const FAvidScriptBindingObjectFactoryModel& Factory)
		{
			return Factory.Kind == EAvidScriptObjectFactoryKind::ActorComponent;
		});
	if (InOutCSharpTypeNames.Contains(TEXT("ProjectFactories"))
		|| (bHasComponentFactories
			&& InOutCSharpTypeNames.Contains(TEXT("ProjectTypes"))))
	{
		OutErrorCategory = TEXT("csharp_type_collision");
		OutErrorSource = TEXT("ProjectFactories|ProjectTypes");
		return false;
	}
	InOutCSharpTypeNames.Add(TEXT("ProjectFactories"));
	if (bHasComponentFactories)
	{
		InOutCSharpTypeNames.Add(TEXT("ProjectTypes"));
	}

	TSet<FString> PropertyNames;
	for (const FAvidScriptBindingObjectFactoryModel& Factory :
		Package.ObjectFactories)
	{
		const FAvidScriptBindingClassReferenceModel* ClassReference =
			Package.ClassReferences.FindByPredicate(
				[&Factory](
					const FAvidScriptBindingClassReferenceModel& Reference)
				{
					return Reference.StableId == Factory.ClassReferenceId;
				});
		const FAvidScriptBindingTypeModel* ResultType =
			ClassReference == nullptr
				? nullptr
				: FindObjectType(Package, ClassReference->ClassPath);
		const FAvidScriptBindingTypeModel* OuterType =
			FindTypeById(Package, Factory.OuterTypeId);
		const bool bComponentFactory =
			Factory.Kind == EAvidScriptObjectFactoryKind::ActorComponent;
		const FAvidScriptBindingTypeModel* ActorType = bComponentFactory
			? FindObjectType(Package, TEXT("/Script/Engine.Actor"))
			: nullptr;
		if (ClassReference == nullptr
			|| ResultType == nullptr
			|| ResultType->Kind != TEXT("object_handle")
			|| ResultType->ObjectTypeOrdinal == INDEX_NONE
			|| OuterType == nullptr
			|| OuterType->Kind != TEXT("object_handle")
			|| OuterType->ObjectTypeOrdinal == INDEX_NONE
			|| (bComponentFactory
				&& (ActorType == nullptr
					|| ActorType->ObjectTypeOrdinal == INDEX_NONE)))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = Factory.StableId;
			return false;
		}

		const FString PropertyName =
			FAvidScriptEditorCSharpSyntax::MakeIdentifier(Factory.ScriptName);
		const FString TokenSuffix = PropertyName.StartsWith(TEXT("@"))
			? PropertyName.RightChop(1)
			: PropertyName;
		const FString FactoryTokenName = TEXT("TObjectFactoryOf") + TokenSuffix;
		const FString TypeTokenName = bComponentFactory
			? TEXT("TObjectTypeOf") + TokenSuffix
			: FString();
		if (PropertyNames.Contains(PropertyName)
			|| PropertyName == TEXT("ProjectFactories")
			|| (bComponentFactory && PropertyName == TEXT("ProjectTypes"))
			|| InOutCSharpTypeNames.Contains(FactoryTokenName)
			|| (!TypeTokenName.IsEmpty()
				&& InOutCSharpTypeNames.Contains(TypeTokenName)))
		{
			OutErrorCategory = TEXT("csharp_type_collision");
			OutErrorSource = PropertyName;
			return false;
		}
		PropertyNames.Add(PropertyName);
		InOutCSharpTypeNames.Add(FactoryTokenName);
		if (!TypeTokenName.IsEmpty())
		{
			InOutCSharpTypeNames.Add(TypeTokenName);
		}
		OutSurfaces.Add({
			&Factory,
			ResultType,
			OuterType,
			PropertyName,
			FactoryTokenName,
			TypeTokenName
		});
	}
	OutSurfaces.Sort(
		[](const FAvidScriptEditorCSharpObjectFactorySurface& Left,
			const FAvidScriptEditorCSharpObjectFactorySurface& Right)
		{
			return Left.Factory->Ordinal < Right.Factory->Ordinal;
		});
	return true;
}

void FAvidScriptEditorCSharpObjectFactoryRenderer::AppendCapabilityTokens(
	const TArray<FAvidScriptEditorCSharpObjectFactorySurface>& Surfaces,
	TArray<FString>& Lines)
{
	for (const FAvidScriptEditorCSharpObjectFactorySurface& Surface : Surfaces)
	{
		Lines.Append({
			TEXT("[StructLayout(LayoutKind.Sequential)]"),
			TEXT("public readonly struct ") + Surface.FactoryTokenName,
			TEXT("{"),
			TEXT("    private readonly int Ordinal;"),
			TEXT(""),
			TEXT("    internal ") + Surface.FactoryTokenName + TEXT("(int ordinal)"),
			TEXT("    {"),
			TEXT("        Ordinal = ordinal;"),
			TEXT("    }"),
			TEXT(""),
			TEXT("    internal int AvidScriptOrdinal => Ordinal;"),
			TEXT("}"),
			TEXT("")
		});
		if (!Surface.TypeTokenName.IsEmpty())
		{
			Lines.Append({
				TEXT("[StructLayout(LayoutKind.Sequential)]"),
				TEXT("public readonly struct ") + Surface.TypeTokenName,
				TEXT("{"),
				TEXT("    private readonly int Ordinal;"),
				TEXT(""),
				TEXT("    internal ") + Surface.TypeTokenName + TEXT("(int ordinal)"),
				TEXT("    {"),
				TEXT("        Ordinal = ordinal;"),
				TEXT("    }"),
				TEXT(""),
				TEXT("    internal int AvidScriptOrdinal => Ordinal;"),
				TEXT("}"),
				TEXT("")
			});
		}
	}

	Lines.Append({ TEXT("public static class ProjectFactories"), TEXT("{") });
	for (const FAvidScriptEditorCSharpObjectFactorySurface& Surface : Surfaces)
	{
		Lines.Add(FString::Printf(
			TEXT("    public static %s %s => new(%d);"),
			*Surface.FactoryTokenName,
			*Surface.PropertyName,
			Surface.Factory->Ordinal));
	}
	Lines.Append({ TEXT("}"), TEXT("") });

	const bool bHasProjectTypes = Surfaces.ContainsByPredicate(
		[](const FAvidScriptEditorCSharpObjectFactorySurface& Surface)
		{
			return !Surface.TypeTokenName.IsEmpty();
		});
	if (bHasProjectTypes)
	{
		Lines.Append({ TEXT("public static class ProjectTypes"), TEXT("{") });
		for (const FAvidScriptEditorCSharpObjectFactorySurface& Surface : Surfaces)
		{
			if (!Surface.TypeTokenName.IsEmpty())
			{
				Lines.Add(FString::Printf(
					TEXT("    public static %s %s => new(%d);"),
					*Surface.TypeTokenName,
					*Surface.PropertyName,
					Surface.ResultType->ObjectTypeOrdinal));
			}
		}
		Lines.Append({ TEXT("}"), TEXT("") });
	}
}

void FAvidScriptEditorCSharpObjectFactoryRenderer::AppendFacadeMethods(
	const TArray<FAvidScriptEditorCSharpObjectFactorySurface>& Surfaces,
	TArray<FString>& Lines)
{
	TSet<FString> ReleaseResultTypeIds;
	for (const FAvidScriptEditorCSharpObjectFactorySurface& Surface : Surfaces)
	{
		const FString ResultTypeName =
			FAvidScriptEditorCSharpSyntax::MakeIdentifier(Surface.ResultType->CppType);
		const FString OuterTypeName =
			FAvidScriptEditorCSharpSyntax::MakeIdentifier(Surface.OuterType->CppType);
		const TCHAR* ConstructMethodName =
			Surface.Factory->Kind == EAvidScriptObjectFactoryKind::ActorComponent
				? TEXT("CreateComponent")
				: TEXT("NewObject");
		Lines.Append({
			FString::Printf(
				TEXT("    public static %s %s(%s outer, %s factory)"),
				*ResultTypeName,
				ConstructMethodName,
				*OuterTypeName,
				*Surface.FactoryTokenName),
			TEXT("    {"),
			TEXT("        long packedHandle = AvidScriptNative.ObjectConstruct("),
			TEXT("            factory.AvidScriptOrdinal,"),
			TEXT("            outer.AvidScriptSlot, outer.AvidScriptGeneration);"),
			TEXT("        return new((int)packedHandle, (int)(packedHandle >> 32));"),
			TEXT("    }"),
			TEXT("")
		});

		if (Surface.Factory->Kind == EAvidScriptObjectFactoryKind::ActorComponent)
		{
			Lines.Append({
				FString::Printf(
					TEXT("    public static %s FindComponent(%s actor, %s type)"),
					*ResultTypeName,
					TEXT("AActor"),
					*Surface.TypeTokenName),
				TEXT("    {"),
				TEXT("        long packedHandle = AvidScriptNative.ActorFindComponent("),
				TEXT("            actor.AvidScriptSlot, actor.AvidScriptGeneration,"),
				TEXT("            type.AvidScriptOrdinal);"),
				TEXT("        return new((int)packedHandle, (int)(packedHandle >> 32));"),
				TEXT("    }"),
				TEXT("")
			});
		}

		if (!ReleaseResultTypeIds.Contains(Surface.ResultType->StableId))
		{
			ReleaseResultTypeIds.Add(Surface.ResultType->StableId);
			Lines.Append({
				FString::Printf(
					TEXT("    public static bool Release(%s value)"),
					*ResultTypeName),
				TEXT("        => AvidScriptNative.ObjectRelease("),
				TEXT("            value.AvidScriptSlot, value.AvidScriptGeneration) != 0;"),
				TEXT("")
			});
		}
	}
	if (!Lines.IsEmpty() && Lines.Last().IsEmpty())
	{
		Lines.Pop();
	}
}

void FAvidScriptEditorCSharpObjectFactoryRenderer::AppendNativeImports(
	const bool bNeedsLeadingBlank,
	TArray<FString>& Lines)
{
	const TConstArrayView<FAvidScriptObjectFactoryBindingSpec> Specs =
		FAvidScriptObjectFactoryBinding::GetSpecs();
	for (int32 SpecIndex = 0; SpecIndex < Specs.Num(); ++SpecIndex)
	{
		const FAvidScriptObjectFactoryBindingSpec& Spec = Specs[SpecIndex];
		if (bNeedsLeadingBlank || SpecIndex > 0)
		{
			Lines.Add(TEXT(""));
		}
		Lines.Add(FString::Printf(
			TEXT("    [DllImport(\"%s\", EntryPoint = \"%s\")]"),
			*Spec.ModuleName,
			*Spec.ImportName));
		switch (Spec.Kind)
		{
		case EAvidScriptBindingInvocationKind::ObjectConstruct:
			Lines.Add(TEXT("    internal static extern long ObjectConstruct(int factoryOrdinal, int outerSlot, int outerGeneration);"));
			break;
		case EAvidScriptBindingInvocationKind::ObjectRelease:
			Lines.Add(TEXT("    internal static extern int ObjectRelease(int slot, int generation);"));
			break;
		case EAvidScriptBindingInvocationKind::ActorFindComponent:
			Lines.Add(TEXT("    internal static extern long ActorFindComponent(int actorSlot, int actorGeneration, int typeOrdinal);"));
			break;
		default:
			checkNoEntry();
			break;
		}
	}
}
