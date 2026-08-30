#pragma once

#include "CoreMinimal.h"

struct FAvidScriptRuntimeArtifact;
class FAvidScriptGeneratedTypeRegistrySnapshot;
class UObject;

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeRuntimeHost final
{
public:
	static FAvidScriptGeneratedTypeRuntimeHost& Get();

	bool Startup();
	void Shutdown();

	bool InstallPackage(
		const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
		const FAvidScriptRuntimeArtifact& Artifact,
		FString& OutError);
	bool ClearPackage(FString& OutError);

	bool BeginInstance(UObject& Receiver, uint32 TypeOrdinal, FString& OutError);
	bool EndInstance(UObject& Receiver, FString& OutError);
	bool IsInstanceActive(const UObject& Receiver) const;
	int32 GetActiveInstanceCount() const;
	int32 GetRegisteredHandleCount() const;

private:
	FAvidScriptGeneratedTypeRuntimeHost();
	~FAvidScriptGeneratedTypeRuntimeHost();
	FAvidScriptGeneratedTypeRuntimeHost(const FAvidScriptGeneratedTypeRuntimeHost&) = delete;
	FAvidScriptGeneratedTypeRuntimeHost& operator=(const FAvidScriptGeneratedTypeRuntimeHost&) = delete;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
