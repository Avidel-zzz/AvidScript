#pragma once

#include "AvidScriptBindingInvocationKind.h"
#include "AvidScriptObjectFactoryPolicy.h"
#include "AvidScriptObjectRegistry.h"
#include "Containers/ArrayView.h"
#include "CoreMinimal.h"

class IAvidScriptObjectOwnershipDomain;
class UClass;

struct FAvidScriptObjectFactoryBindingSpec
{
	EAvidScriptBindingInvocationKind Kind =
		EAvidScriptBindingInvocationKind::ObjectConstruct;
	FString StableId;
	FString ModuleName;
	FString ImportName;
	FString Signature;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptObjectFactoryBinding
{
public:
	static TConstArrayView<FAvidScriptObjectFactoryBindingSpec> GetSpecs();

	static bool Construct(
		const FAvidScriptObjectFactoryPlan& Plan,
		FAvidScriptObjectRegistry& Registry,
		IAvidScriptObjectOwnershipDomain& Ownership,
		const FAvidScriptObjectHandle& OuterHandle,
		FAvidScriptObjectHandleResult& OutResult);

	static bool Release(
		FAvidScriptObjectRegistry& Registry,
		IAvidScriptObjectOwnershipDomain& Ownership,
		const FAvidScriptObjectHandle& Handle,
		FAvidScriptObjectHandleResult& OutResult);

	static bool FindComponent(
		FAvidScriptObjectRegistry& Registry,
		IAvidScriptObjectOwnershipDomain& Ownership,
		const FAvidScriptObjectHandle& ActorHandle,
		UClass& ComponentClass,
		FAvidScriptObjectHandleResult& OutResult);
};
