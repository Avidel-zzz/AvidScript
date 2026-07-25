#pragma once

#include "CoreMinimal.h"

class UObject;
class FAvidScriptObjectRegistry;
struct FAvidScriptObjectHandle;
struct FAvidScriptObjectHandleResult;
enum class EAvidScriptObjectFactoryKind : uint8;

class AVIDSCRIPTBINDINGS_API IAvidScriptObjectOwnershipDomain
{
public:
	IAvidScriptObjectOwnershipDomain();
	virtual ~IAvidScriptObjectOwnershipDomain();

	virtual bool Adopt(
		FAvidScriptObjectRegistry& Registry,
		UObject& Object,
		const FAvidScriptObjectHandle& Handle,
		EAvidScriptObjectFactoryKind Kind,
		FAvidScriptObjectHandleResult& OutResult) = 0;
	virtual bool Release(
		UObject& Object,
		FAvidScriptObjectRegistry& Registry,
		FAvidScriptObjectHandleResult& OutResult) = 0;
	virtual bool Owns(const UObject& Object) const = 0;
	virtual void Cleanup(FAvidScriptObjectRegistry& Registry) = 0;
};
