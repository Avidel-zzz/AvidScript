#pragma once

#include "CoreMinimal.h"

struct FAvidScriptBindingSelectionProfile;
struct FAvidScriptProjectBindingClassSpec;

struct FAvidScriptCSharpBindingEmitResult
{
	bool bSucceeded = false;
	bool bReusedExistingPackage = false;
	int32 BindingCount = 0;
	int32 TypeCount = 0;
	int32 ClassReferenceCount = 0;
	FString PackageName;
	FString PackageHash;
	FString DescriptorHash;
	FString SourceHash;
	FString ManifestHash;
	FString PackageDirectory;
	FString DescriptorPath;
	FString ReferenceSourcePath;
	FString ManifestPath;
	FString ErrorCategory;
	FString ErrorSource;
	FString NextAction;
	FString ErrorMessage;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpBindingEmitter
{
public:
	static bool Emit(
		const FString& DescriptorJson,
		FString& OutReferenceSource,
		FString& OutManifestJson,
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static bool EmitDefault(
		FString& OutDescriptorJson,
		FString& OutReferenceSource,
		FString& OutManifestJson,
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static bool EmitEngineGameplay(
		FString& OutDescriptorJson,
		FString& OutReferenceSource,
		FString& OutManifestJson,
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static bool EmitProfile(
		const FAvidScriptBindingSelectionProfile& Profile,
		FString& OutDescriptorJson,
		FString& OutReferenceSource,
		FString& OutManifestJson,
		FAvidScriptCSharpBindingEmitResult& OutResult);
	static bool EmitProfile(
		const FAvidScriptBindingSelectionProfile& Profile,
		const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
		FString& OutDescriptorJson,
		FString& OutReferenceSource,
		FString& OutManifestJson,
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static FString GetDefaultOutputRoot();

	static bool PublishDescriptor(
		const FString& DescriptorJson,
		const FString& OutputRoot,
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static bool PublishDefault(
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static bool PublishDefault(
		const FString& OutputRoot,
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static bool PublishEngineGameplay(
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static bool PublishEngineGameplay(
		const FString& OutputRoot,
		FAvidScriptCSharpBindingEmitResult& OutResult);

	static bool PublishProfile(
		const FAvidScriptBindingSelectionProfile& Profile,
		const FString& OutputRoot,
		FAvidScriptCSharpBindingEmitResult& OutResult);
	static bool PublishProfile(
		const FAvidScriptBindingSelectionProfile& Profile,
		const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
		const FString& OutputRoot,
		FAvidScriptCSharpBindingEmitResult& OutResult);
};
