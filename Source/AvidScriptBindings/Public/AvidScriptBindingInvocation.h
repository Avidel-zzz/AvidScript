#pragma once

#include "AvidScriptActorBinding.h"
#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptGeneratedBindingRegistry.h"
#include "AvidScriptObjectFactoryPolicy.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptVmBackend.h"
#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UClass;
class UWorld;
class IAvidScriptObjectOwnershipDomain;

enum class EAvidScriptBindingFastPathKind : uint8
{
	None,
	ScalarI32PairToI32
};

enum class EAvidScriptBindingInvocationPolicy : uint8
{
	SemanticProcessEvent,
	QualifiedNativeDirect
};

enum class EAvidScriptBindingInvocationMode : uint8
{
	SemanticProcessEvent,
	QualifiedNativeDirect,
	GeneratedNativeS1
};

struct FAvidScriptBindingPackageLoadResult
{
	bool bSucceeded = false;
	int32 BindingCount = 0;
	int32 ClassReferenceCount = 0;
	int32 ObjectFactoryCount = 0;
	int32 RequiredScratchSize = 0;
	FString PackageName;
	FString PackageHash;
	FString ErrorCategory;
	FString ErrorSource;
	FString ErrorDetails;
};

struct FAvidScriptBindingPackageInstrumentation
{
	uint64 ClassLoadCount = 0;
	uint64 ReflectedNameLookupCount = 0;
	uint64 TypedThunkPlanCount = 0;
	uint64 ReflectionFallbackPlanCount = 0;
	uint64 QualifiedNativeDirectPlanCount = 0;
	uint64 GeneratedNativeS1PlanCount = 0;
	uint64 SemanticOnlyPlanCount = 0;
};

struct FAvidScriptBindingInvocationInstrumentation
{
	uint64 SemanticProcessEventCount = 0;
	uint64 QualifiedNativeDirectCount = 0;
	uint64 RequestedNativeDirectFallbackCount = 0;
	uint64 GeneratedNativeS1HitCount = 0;
	uint64 GeneratedNativeS1FallbackCount = 0;
	uint64 GeneratedNativeS1RejectCount = 0;
};

struct FAvidScriptBindingInvocationContext
{
	FAvidScriptObjectRegistry* ObjectRegistry = nullptr;
	IAvidScriptObjectOwnershipDomain* ObjectOwnership = nullptr;
	FAvidScriptObjectHandle OwnerHandle;
	TWeakObjectPtr<UWorld> World;
	EAvidScriptActorWritePolicy WritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
	IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr;
	EAvidScriptBindingInvocationPolicy InvocationPolicy =
		EAvidScriptBindingInvocationPolicy::SemanticProcessEvent;
	// Optional caller-owned counters. The owner must outlive every dispatch using this context.
	FAvidScriptBindingInvocationInstrumentation* InvocationInstrumentation = nullptr;
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
#if WITH_DEV_AUTOMATION_TESTS
	static TSharedPtr<const FAvidScriptBindingPackage>
		MakeGeneratedPlanForTesting(
			const FString& PackageHash,
			const FString& StableId,
			const FString& DescriptorIdentity,
			EAvidScriptGeneratedBindingShape Shape,
			UClass* ExpectedClass,
			FProperty* ReflectedProperty,
			bool bPropertyWrite,
			bool bRequiresWriteAccess,
			EAvidScriptBindingReloadEffect ReloadEffect);
#endif

	const FString& GetPackageName() const;
	const FString& GetPackageHash() const;
	int32 GetDescriptorSchemaVersion() const;
	const FAvidScriptVmBindingPackage& GetVmPackage() const;
	const FAvidScriptBindingPackageInstrumentation& GetInstrumentation() const;
	bool TryGetFastPathKind(
		uint32 Ordinal,
		EAvidScriptBindingFastPathKind& OutKind) const;
	bool TryGetInvocationMode(
		uint32 Ordinal,
		EAvidScriptBindingInvocationMode& OutMode) const;
	bool BuildTypedHostImports(
		TArray<FAvidScriptVmTypedHostImport>& OutImports,
		FString& OutError) const;
	bool TryGetGeneratedBinding(
		uint32 Ordinal,
		const FAvidScriptGeneratedBindingEntry*& OutEntry,
		UClass*& OutExpectedClass,
		bool& bOutPropertyWrite,
		bool& bOutRequiresWriteAccess) const;
	bool PrepareGeneratedHostEffect(
		uint32 Ordinal,
		const FAvidScriptObjectHandle& ReceiverHandle,
		UObject& Receiver,
		const FAvidScriptBindingInvocationContext& Context) const;
	bool TryFindFunctionOrdinal(
		const UClass& OwnerClass,
		FName FunctionName,
		uint32& OutOrdinal) const;
	int32 GetRequiredScratchSize() const;
	int32 GetObjectTypeCount() const;
	bool TryResolveObjectType(uint32 Ordinal, UClass*& OutClass) const;
	UClass* GetExpectedSelfClass() const;
	int32 GetClassReferenceCount() const;
	bool TryResolveClassReference(
		uint32 Ordinal,
		UClass*& OutClass,
		UClass*& OutBaseClass) const;
	int32 GetObjectFactoryCount() const;
	bool TryResolveObjectFactory(
		uint32 Ordinal,
		const FAvidScriptObjectFactoryPlan*& OutPlan) const;

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
