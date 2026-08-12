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
class UFunction;
class FProperty;
class FMulticastDelegateProperty;
class UWorld;
class IAvidScriptObjectOwnershipDomain;
class FAvidScriptUtf8ValueHeap;
class FAvidScriptArrayValueHeap;

enum class EAvidScriptBindingFastPathKind : uint8
{
	None,
	ScalarI32PairToI32,
	VectorValueToVector,
	ObjectToObject
};

enum class EAvidScriptBindingInvocationPolicy : uint8
{
	SemanticProcessEvent,
	AdaptiveSemantic,
	QualifiedNativeDirect
};

enum class EAvidScriptBindingInvocationMode : uint8
{
	SemanticProcessEvent,
	AdaptivePreparedNative,
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
	uint64 AdaptivePreparedNativePlanCount = 0;
	uint64 AdaptiveStrictFallbackPlanCount = 0;
	uint64 QualifiedNativeDirectPlanCount = 0;
	uint64 GeneratedNativeS1PlanCount = 0;
	uint64 SemanticOnlyPlanCount = 0;
};

struct FAvidScriptBindingInvocationInstrumentation
{
	uint64 PreparedDynamicHitCount = 0;
	uint64 PreparedDynamicFallbackCount = 0;
	uint64 PreparedDynamicRejectCount = 0;
	uint64 SemanticProcessEventCount = 0;
	uint64 AdaptivePreparedNativeHitCount = 0;
	uint64 AdaptiveProcessEventFallbackCount = 0;
	uint64 AdaptiveGuardRejectCount = 0;
	uint64 QualifiedNativeDirectCount = 0;
	uint64 RequestedNativeDirectFallbackCount = 0;
	uint64 GeneratedNativeS1HitCount = 0;
	uint64 GeneratedNativeS1FallbackCount = 0;
	uint64 GeneratedNativeS1RejectCount = 0;
	uint64 GeneratedFusedFastHitCount = 0;
	uint64 GeneratedFusedRevalidateCount = 0;
	uint64 GeneratedFusedCallSitePrepareCount = 0;
	uint64 GeneratedDirectReadPrepareCount = 0;
	uint64 GeneratedDirectWritePrepareCount = 0;
	uint64 GeneratedJournalSlowPathCount = 0;
	uint64 GeneratedFusedFastHitCycles = 0;
	uint64 GeneratedFusedRevalidateCycles = 0;
	uint64 GeneratedFusedCallSitePrepareCycles = 0;
	uint64 GeneratedJournalSlowPathCycles = 0;
};

struct FAvidScriptBindingInvocationContext
{
	FAvidScriptObjectRegistry* ObjectRegistry = nullptr;
	IAvidScriptObjectOwnershipDomain* ObjectOwnership = nullptr;
	FAvidScriptUtf8ValueHeap* Utf8ValueHeap = nullptr;
	FAvidScriptArrayValueHeap* ArrayValueHeap = nullptr;
	FAvidScriptObjectHandle OwnerHandle;
	TWeakObjectPtr<UWorld> World;
	EAvidScriptActorWritePolicy WritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
	IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr;
	EAvidScriptBindingInvocationPolicy InvocationPolicy =
		EAvidScriptBindingInvocationPolicy::SemanticProcessEvent;
	// Optional caller-owned counters. The owner must outlive every dispatch using this context.
	FAvidScriptBindingInvocationInstrumentation* InvocationInstrumentation = nullptr;
};

struct FAvidScriptPreparedGeneratedBinding
{
	uint32 BindingOrdinal = MAX_uint32;
	FAvidScriptGeneratedBindingLease Lease;
	const FAvidScriptGeneratedBindingEntry* Entry = nullptr;
	UClass* ExpectedClass = nullptr;
	FProperty* ReflectedProperty = nullptr;
	EAvidScriptBindingReloadEffect ReloadEffect =
		EAvidScriptBindingReloadEffect::Unsupported;
	bool bPropertyWrite = false;
	bool bPropertyWriteHasFunction = false;
	bool bRequiresWriteAccess = false;
};

using FAvidScriptPreparedReflectionNativeGuard =
	bool (*)(const void* InvocationCell, UObject& Receiver);

using FAvidScriptPreparedReflectionI32PairCall =
	bool (*)(
		const void* InvocationCell,
		UObject& Receiver,
		int32 Left,
		int32 Right,
		bool bUseNative,
		int32& OutValue,
		FString& OutErrorCategory,
		FString& OutErrorDetails);

using FAvidScriptPreparedReflectionVectorCall =
	bool (*)(
		const void* InvocationCell,
		UObject& Receiver,
		const FVector& Input,
		bool bUseNative,
		FVector& OutValue,
		FString& OutErrorCategory,
		FString& OutErrorDetails);

using FAvidScriptPreparedReflectionObjectCall =
	bool (*)(
		const void* InvocationCell,
		UObject& Receiver,
		UObject* Input,
		bool bUseNative,
		UObject*& OutValue,
		FString& OutErrorCategory,
		FString& OutErrorDetails);

using FAvidScriptPreparedReflectionPropertyI32Get =
	bool (*)(const void* InvocationCell, UObject& Receiver, int32& OutValue);

using FAvidScriptPreparedReflectionPropertyI32Set =
	bool (*)(const void* InvocationCell, UObject& Receiver, int32 Value);

struct FAvidScriptPreparedReflectionBinding
{
	uint32 BindingOrdinal = MAX_uint32;
	FAvidScriptVmTypedHostImport TypedHostImport;
	UClass* ExpectedClass = nullptr;
	const void* ImmutablePlanIdentity = nullptr;
	FAvidScriptPreparedReflectionNativeGuard NativeGuard = nullptr;
	FAvidScriptPreparedReflectionI32PairCall I32PairCall = nullptr;
	FAvidScriptPreparedReflectionVectorCall VectorCall = nullptr;
	FAvidScriptPreparedReflectionObjectCall ObjectCall = nullptr;
	FAvidScriptPreparedReflectionPropertyI32Get PropertyI32Get = nullptr;
	FAvidScriptPreparedReflectionPropertyI32Set PropertyI32Set = nullptr;
	UClass* ExpectedObjectClass = nullptr;
	FProperty* ReflectedProperty = nullptr;
	EAvidScriptBindingReloadEffect ReloadEffect =
		EAvidScriptBindingReloadEffect::Unsupported;
	bool bAdaptiveNativeEligible = false;
	bool bQualifiedNativeEligible = false;
	bool bPropertyWrite = false;
	bool bRequiresWriteAccess = false;
};

using FAvidScriptPreparedDynamicInvoke =
	bool (*)(
		const void* InvocationCell,
		UObject& Receiver,
		TConstArrayView<uint64> Arguments,
		IAvidScriptVmGuestMemory* GuestMemory,
		const FAvidScriptBindingInvocationContext& InvocationContext,
		TArray<uint8>& InvocationScratch,
		FAvidScriptDynamicHostCallResult& OutResult);

struct FAvidScriptPreparedDynamicBinding
{
	uint32 BindingOrdinal = MAX_uint32;
	FString StableId;
	FString ModuleName;
	FString ImportName;
	FString Signature;
	const void* ImmutableInvocationCell = nullptr;
	UClass* ExpectedClass = nullptr;
	int32 ExpectedArgumentCount = 0;
	int32 RequiredScratchSize = 0;
	FAvidScriptPreparedDynamicInvoke Invoke = nullptr;
	bool bRequiresGuestMemory = false;
	bool bStatic = false;
};

using FAvidScriptPreparedDelegateEncode =
	bool (*)(
		const void* ImmutableCodecIdentity,
		const void* NativeParameters,
		const FAvidScriptBindingInvocationContext& InvocationContext,
		FAvidScriptVmCallFrame& OutFrame,
		TArray<FAvidScriptObjectHandle>& OutBorrowedHandles,
		FString& OutErrorCategory,
		FString& OutErrorDetails);

struct FAvidScriptPreparedDelegateEvent
{
	uint32 EventOrdinal = MAX_uint32;
	FString StableId;
	FString ExportName;
	UClass* ExpectedSourceClass = nullptr;
	FMulticastDelegateProperty* DelegateProperty = nullptr;
	UFunction* SignatureFunction = nullptr;
	uint32 ParameterCellCount = 0;
	const void* ImmutableCodecIdentity = nullptr;
	FAvidScriptPreparedDelegateEncode Encode = nullptr;
};

enum class EAvidScriptPreparedHostEffectMode : uint8
{
	Rejected,
	DirectRead,
	DirectWrite,
	Journaled
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
	static TSharedPtr<const FAvidScriptBindingPackage>
		MakePreparedDynamicPlanForTesting(
			const FString& StableId,
			const FString& ImportName,
			const FString& Signature,
			UClass* ExpectedClass,
			UFunction* Function,
			int32 ExpectedArgumentCount,
			int32 RequiredScratchSize,
			bool bRequiresGuestMemory,
			bool bStatic);
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
	bool TryGetInvocationMode(
		uint32 Ordinal,
		EAvidScriptBindingInvocationPolicy Policy,
		EAvidScriptBindingInvocationMode& OutMode) const;
	bool BuildTypedHostImports(
		TArray<FAvidScriptVmTypedHostImport>& OutImports,
		FString& OutError) const;
	bool BuildPreparedGeneratedBindings(
		TArray<FAvidScriptPreparedGeneratedBinding>& OutBindings,
		FString& OutError) const;
	bool BuildPreparedReflectionBindings(
		TArray<FAvidScriptPreparedReflectionBinding>& OutBindings,
		FString& OutError) const;
	bool BuildPreparedDynamicBindings(
		TArray<FAvidScriptPreparedDynamicBinding>& OutBindings,
		FString& OutError) const;
	bool BuildPreparedDelegateEvents(
		TArray<FAvidScriptPreparedDelegateEvent>& OutEvents,
		FString& OutError) const;
	bool InvokePreparedReflectionI32Pair(
		const FAvidScriptPreparedReflectionBinding& Binding,
		UObject& Receiver,
		int32 Left,
		int32 Right,
		const FAvidScriptBindingInvocationContext& Context,
		int32& OutValue,
		FString& OutErrorCategory,
		FString& OutErrorDetails) const;
	bool TryGetGeneratedBinding(
		uint32 Ordinal,
		const FAvidScriptGeneratedBindingEntry*& OutEntry,
		UClass*& OutExpectedClass,
		bool& bOutPropertyWrite,
		bool& bOutRequiresWriteAccess) const;
	bool TryGetGeneratedPropertyBinding(
		uint32 Ordinal,
		const FAvidScriptGeneratedBindingEntry*& OutEntry,
		UClass*& OutExpectedClass,
		FProperty*& OutProperty,
		bool& bOutRequiresWriteAccess) const;
	bool PrepareGeneratedHostEffect(
		uint32 Ordinal,
		const FAvidScriptObjectHandle& ReceiverHandle,
		UObject& Receiver,
		const FAvidScriptBindingInvocationContext& Context) const;
	bool PrepareGeneratedHostEffect(
		const FAvidScriptPreparedGeneratedBinding& Binding,
		const FAvidScriptObjectHandle& ReceiverHandle,
		UObject& Receiver,
		const FAvidScriptBindingInvocationContext& Context) const;
	EAvidScriptPreparedHostEffectMode ResolvePreparedHostEffectMode(
		const FAvidScriptPreparedGeneratedBinding& Binding,
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
