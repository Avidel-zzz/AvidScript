#pragma once

#include "AvidScriptEditorBindingSelectionTypes.h"

#include "CoreMinimal.h"

struct FAvidScriptProjectBindingClassSpec
{
	FString ScriptName;
	FString ClassPath;
	FString BaseClassPath;
	FString LoadPolicy = TEXT("EditorLoad");
};

enum class EAvidScriptProjectObjectFactoryKind : uint8
{
	NewObject,
	ActorComponent
};

enum class EAvidScriptProjectObjectOwnership : uint8
{
	Session
};

enum class EAvidScriptProjectComponentRegistration : uint8
{
	None,
	RegisterInstance
};

struct FAvidScriptProjectObjectFactorySpec
{
	FString ScriptName;
	FString ClassReference;
	FString OuterBaseClassPath;
	EAvidScriptProjectObjectFactoryKind Kind = EAvidScriptProjectObjectFactoryKind::NewObject;
	EAvidScriptProjectObjectOwnership Ownership = EAvidScriptProjectObjectOwnership::Session;
	EAvidScriptProjectComponentRegistration Registration =
		EAvidScriptProjectComponentRegistration::None;
};

struct FAvidScriptProjectBindingProfileSpec
{
	FString PackageName;
	FString SelfClassPath;
	TArray<FString> ModulePaths;
	TArray<FAvidScriptReflectedClassSelection> Classes;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	TArray<FAvidScriptProjectObjectFactorySpec> ObjectFactories;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorProjectBindingProfile
{
public:
	static const TCHAR* GetResolverVersion();

	static bool Resolve(
		const FAvidScriptProjectBindingProfileSpec& Spec,
		FAvidScriptBindingSelectionProfile& OutSelection,
		TArray<FAvidScriptProjectBindingClassSpec>& OutClassReferences,
		FString& OutSelectionHash,
		FAvidScriptBindingSelectionResolveResult& OutResult);

	static bool Resolve(
		const FAvidScriptProjectBindingProfileSpec& Spec,
		FAvidScriptBindingSelectionProfile& OutSelection,
		TArray<FAvidScriptProjectBindingClassSpec>& OutClassReferences,
		TArray<FAvidScriptProjectObjectFactorySpec>& OutObjectFactories,
		FString& OutSelectionHash,
		FAvidScriptBindingSelectionResolveResult& OutResult);
};
