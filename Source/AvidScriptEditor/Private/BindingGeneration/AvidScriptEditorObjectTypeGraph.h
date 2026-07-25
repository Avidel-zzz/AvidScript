#pragma once

#include "AvidScriptEditorProjectBindingProfile.h"

#include "CoreMinimal.h"

class UClass;

struct FAvidScriptEditorObjectTypeNode
{
	FString TypeId;
	FString CanonicalClassPath;
	FString BaseTypeId;
	int32 Ordinal = INDEX_NONE;
};

struct FAvidScriptEditorObjectTypeGraph
{
	TArray<FAvidScriptEditorObjectTypeNode> Nodes;

	static bool Build(
		TConstArrayView<UClass*> HandleClasses,
		UClass* SelfClass,
		TConstArrayView<FAvidScriptProjectBindingClassSpec> ClassReferences,
		FAvidScriptEditorObjectTypeGraph& OutGraph,
		FString& OutErrorCategory,
		FString& OutErrorDetails);

	static bool Build(
		TConstArrayView<UClass*> HandleClasses,
		UClass* SelfClass,
		TConstArrayView<FAvidScriptProjectBindingClassSpec> ClassReferences,
		TConstArrayView<FAvidScriptProjectObjectFactorySpec> ObjectFactories,
		FAvidScriptEditorObjectTypeGraph& OutGraph,
		FString& OutErrorCategory,
		FString& OutErrorDetails);
};
