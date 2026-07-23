#include "BindingGeneration/AvidScriptEditorObjectTypeGraph.h"

#include "BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
void SetObjectTypeGraphFailure(
	FString& OutErrorCategory,
	FString& OutErrorDetails,
	const TCHAR* Category,
	const FString& Details)
{
	OutErrorCategory = Category;
	OutErrorDetails = Details;
}

FString MakeObjectTypeGraphTypeId(const FString& CanonicalClassPath)
{
	return FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
		TEXT("object:") + CanonicalClassPath,
		{});
}

bool AddObjectTypeClosure(
	UClass* Class,
	TMap<FString, UClass*>& ClassesByPath,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	if (Class == nullptr)
	{
		SetObjectTypeGraphFailure(
			OutErrorCategory,
			OutErrorDetails,
			TEXT("object_type_class_invalid"),
			TEXT("A handle-capable object type class must not be null."));
		return false;
	}
	if (!Class->IsChildOf(UObject::StaticClass()))
	{
		SetObjectTypeGraphFailure(
			OutErrorCategory,
			OutErrorDetails,
			TEXT("object_type_class_invalid"),
			Class->GetPathName());
		return false;
	}

	for (UClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
	{
		const FString CanonicalClassPath = Current->GetPathName();
		if (CanonicalClassPath.IsEmpty())
		{
			SetObjectTypeGraphFailure(
				OutErrorCategory,
				OutErrorDetails,
				TEXT("object_type_class_invalid"),
				TEXT("A handle-capable object type class has no canonical class path."));
			return false;
		}
		ClassesByPath.FindOrAdd(CanonicalClassPath) = Current;
		if (Current == UObject::StaticClass())
		{
			return true;
		}
	}

	SetObjectTypeGraphFailure(
		OutErrorCategory,
		OutErrorDetails,
		TEXT("object_type_root_missing"),
		Class->GetPathName());
	return false;
}
} // namespace

bool FAvidScriptEditorObjectTypeGraph::Build(
	const TConstArrayView<UClass*> HandleClasses,
	UClass* SelfClass,
	const TConstArrayView<FAvidScriptProjectBindingClassSpec> ClassReferences,
	FAvidScriptEditorObjectTypeGraph& OutGraph,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	OutGraph = FAvidScriptEditorObjectTypeGraph();
	OutErrorCategory.Empty();
	OutErrorDetails.Empty();

	TMap<FString, UClass*> ClassesByPath;
	for (UClass* HandleClass : HandleClasses)
	{
		if (!AddObjectTypeClosure(HandleClass, ClassesByPath, OutErrorCategory, OutErrorDetails))
		{
			return false;
		}
	}
	if (SelfClass != nullptr
		&& !AddObjectTypeClosure(SelfClass, ClassesByPath, OutErrorCategory, OutErrorDetails))
	{
		return false;
	}
	for (const FAvidScriptProjectBindingClassSpec& ClassReference : ClassReferences)
	{
		if (ClassReference.BaseClassPath.IsEmpty())
		{
			SetObjectTypeGraphFailure(
				OutErrorCategory,
				OutErrorDetails,
				TEXT("object_type_class_reference_base_missing"),
				ClassReference.ScriptName);
			return false;
		}
		UClass* BaseClass = LoadObject<UClass>(nullptr, *ClassReference.BaseClassPath);
		if (BaseClass == nullptr)
		{
			SetObjectTypeGraphFailure(
				OutErrorCategory,
				OutErrorDetails,
				TEXT("object_type_class_reference_base_missing"),
				ClassReference.BaseClassPath);
			return false;
		}
		if (!AddObjectTypeClosure(BaseClass, ClassesByPath, OutErrorCategory, OutErrorDetails))
		{
			return false;
		}
	}

	TArray<FString> CanonicalClassPaths;
	ClassesByPath.GetKeys(CanonicalClassPaths);
	CanonicalClassPaths.Sort();
	TMap<FString, FString> TypeIdsByPath;
	for (int32 Index = 0; Index < CanonicalClassPaths.Num(); ++Index)
	{
		const FString& CanonicalClassPath = CanonicalClassPaths[Index];
		const FString TypeId = MakeObjectTypeGraphTypeId(CanonicalClassPath);
		for (const TPair<FString, FString>& ExistingType : TypeIdsByPath)
		{
			if (ExistingType.Value == TypeId && ExistingType.Key != CanonicalClassPath)
			{
				SetObjectTypeGraphFailure(
					OutErrorCategory,
					OutErrorDetails,
					TEXT("object_type_id_ambiguous"),
					ExistingType.Key + TEXT(";") + CanonicalClassPath);
				return false;
			}
		}
		TypeIdsByPath.Add(CanonicalClassPath, TypeId);
		OutGraph.Nodes.Add({ TypeId, CanonicalClassPath, FString(), Index });
	}

	for (FAvidScriptEditorObjectTypeNode& Node : OutGraph.Nodes)
	{
		UClass* Class = ClassesByPath.FindChecked(Node.CanonicalClassPath);
		UClass* BaseClass = Class->GetSuperClass();
		if (BaseClass == nullptr)
		{
			continue;
		}
		const FString* BaseTypeId = TypeIdsByPath.Find(BaseClass->GetPathName());
		if (BaseTypeId == nullptr)
		{
			SetObjectTypeGraphFailure(
				OutErrorCategory,
				OutErrorDetails,
				TEXT("object_type_graph_incomplete"),
				Node.CanonicalClassPath);
			OutGraph = FAvidScriptEditorObjectTypeGraph();
			return false;
		}
		Node.BaseTypeId = *BaseTypeId;
	}
	return true;
}
