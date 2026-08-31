#pragma once

#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

#include "CoreMinimal.h"

enum class EAvidScriptEditorGeneratedTypeReloadClassification : uint8
{
	InitialInstall,
	BodyOnly,
	NativeRebuildRequired,
};

enum class EAvidScriptEditorGeneratedTypeReloadStatus : uint8
{
	Unknown,
	Watching,
	DuplicateIgnored,
	InitialInstallApplied,
	BodyOnlyApplied,
	NativeRebuildRequired,
	Rejected,
	StartFailed,
	Stopped,
};

struct FAvidScriptEditorGeneratedTypeDescriptorIdentity
{
	FString PackageId;
	FString PreviousPackageId;
	FString NativeStructureSha256;
	FString PreviousNativeStructureSha256;
	EAvidScriptEditorGeneratedTypeReloadClassification Classification =
		EAvidScriptEditorGeneratedTypeReloadClassification::InitialInstall;
};

struct FAvidScriptEditorGeneratedTypeReloadStats
{
	uint64 ObservedBatchCount = 0;
	uint64 AppliedCount = 0;
	uint64 DuplicateCount = 0;
	uint64 NativeRebuildRequiredCount = 0;
	uint64 RejectedCount = 0;
};

struct FAvidScriptEditorGeneratedTypeReloadServiceResult
{
	bool bSucceeded = false;
	bool bRunning = false;
	bool bRuntimeMutationAttempted = false;
	bool bRuntimeApplied = false;
	EAvidScriptEditorGeneratedTypeReloadStatus Status =
		EAvidScriptEditorGeneratedTypeReloadStatus::Unknown;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString DescriptorPath;
	FAvidScriptEditorGeneratedTypeDescriptorIdentity DescriptorIdentity;
	FAvidScriptGeneratedTypePackageReloadResult RuntimeReloadResult;
	FAvidScriptEditorGeneratedTypeReloadStats Stats;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorGeneratedTypeReloadPolicy final
{
public:
	static FString GetDefaultDescriptorPath();
	static bool ReadPackageId(
		const FString& DescriptorPath,
		FString& OutPackageId,
		FString& OutErrorCategory,
		FString& OutErrorMessage);
	static bool ReadDescriptorIdentity(
		const FString& DescriptorPath,
		FAvidScriptEditorGeneratedTypeDescriptorIdentity& OutIdentity,
		FString& OutErrorCategory,
		FString& OutErrorMessage);
	static bool ApplyPublishedDescriptor(
		const FString& DescriptorPath,
		EAvidScriptEditorGeneratedTypeReloadClassification Classification,
		FAvidScriptEditorGeneratedTypeReloadServiceResult& OutResult);
};
