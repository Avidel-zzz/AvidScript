#pragma once

#include "CoreMinimal.h"

struct FAvidScriptRuntimeArtifact;
class FAvidScriptGeneratedTypeRegistrySnapshot;
class UObject;

enum class EAvidScriptGeneratedTypePackageReloadDisposition : uint8
{
	Rejected,
	BodyOnlyApplied,
	NativeRebuildRequired,
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypePackageReloadResult
{
	EAvidScriptGeneratedTypePackageReloadDisposition Disposition =
		EAvidScriptGeneratedTypePackageReloadDisposition::Rejected;
	int32 CandidateInstanceCount = 0;
	int32 ReloadedInstanceCount = 0;
	int32 RolledBackInstanceCount = 0;
	bool bRollbackPreservedLivePackage = false;
	FString StructuralChangeReason;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeRuntimeHost final
{
public:
	static FAvidScriptGeneratedTypeRuntimeHost& Get();
	static bool IsCommandletExecutionSuppressed();
	~FAvidScriptGeneratedTypeRuntimeHost();

	bool Startup();
	void Shutdown();

	bool InstallPackage(
		const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
		const FAvidScriptRuntimeArtifact& Artifact,
		FString& OutError);
	bool InstallPackageFromDescriptorFile(
		const FString& DescriptorPath,
		FString& OutError);
	bool ReloadPackage(
		const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
		const FAvidScriptRuntimeArtifact& Artifact,
		FAvidScriptGeneratedTypePackageReloadResult& OutResult,
		FString& OutError);
	bool ReloadPackageFromDescriptorFile(
		const FString& DescriptorPath,
		FAvidScriptGeneratedTypePackageReloadResult& OutResult,
		FString& OutError);
	bool ClearPackage(FString& OutError);
	bool HasInstalledPackage() const;

	bool BeginInstance(UObject& Receiver, uint32 TypeOrdinal, FString& OutError);
	bool EndInstance(UObject& Receiver, FString& OutError);
	bool IsInstanceActive(const UObject& Receiver) const;
	int32 GetActiveInstanceCount() const;
	int32 GetRegisteredHandleCount() const;
#if WITH_DEV_AUTOMATION_TESTS
	static TUniquePtr<FAvidScriptGeneratedTypeRuntimeHost> CreateIsolatedForTesting();
	void SetReloadFailureAfterInstanceCountForTesting(int32 InstanceCount);
#endif

private:
	FAvidScriptGeneratedTypeRuntimeHost();
	FAvidScriptGeneratedTypeRuntimeHost(const FAvidScriptGeneratedTypeRuntimeHost&) = delete;
	FAvidScriptGeneratedTypeRuntimeHost& operator=(const FAvidScriptGeneratedTypeRuntimeHost&) = delete;
	bool LoadPackageFromDescriptorFile(
		const FString& DescriptorPath,
		TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& OutRegistry,
		FAvidScriptRuntimeArtifact& OutArtifact,
		FString& OutError);

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
