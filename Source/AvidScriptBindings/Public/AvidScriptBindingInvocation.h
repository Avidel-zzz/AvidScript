#pragma once

#include "AvidScriptActorBinding.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptVmBackend.h"
#include "CoreMinimal.h"

struct FAvidScriptBindingPackageLoadResult
{
	bool bSucceeded = false;
	int32 BindingCount = 0;
	int32 RequiredScratchSize = 0;
	FString PackageName;
	FString PackageHash;
	FString ErrorCategory;
	FString ErrorSource;
	FString ErrorDetails;
};

struct FAvidScriptBindingInvocationContext
{
	FAvidScriptObjectRegistry* ObjectRegistry = nullptr;
	FAvidScriptObjectHandle OwnerHandle;
	EAvidScriptActorWritePolicy WritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptBindingPackage
{
public:
	~FAvidScriptBindingPackage();

	FAvidScriptBindingPackage(const FAvidScriptBindingPackage&) = delete;
	FAvidScriptBindingPackage& operator=(const FAvidScriptBindingPackage&) = delete;

	static bool LoadDescriptor(
		const FString& DescriptorJson,
		TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
		FAvidScriptBindingPackageLoadResult& OutResult);

	const FString& GetPackageName() const;
	const FString& GetPackageHash() const;
	const FAvidScriptVmBindingPackage& GetVmPackage() const;
	int32 GetRequiredScratchSize() const;

	bool Dispatch(
		const FAvidScriptDynamicHostCall& Call,
		const FAvidScriptBindingInvocationContext& Context,
		TArray<uint8>& InvocationScratch,
		FAvidScriptDynamicHostCallResult& OutResult) const;

private:
	FAvidScriptBindingPackage();

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
