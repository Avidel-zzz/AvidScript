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
	virtual bool Borrow(
		FAvidScriptObjectRegistry& Registry,
		UObject& Object,
		FAvidScriptObjectHandleResult& OutResult) = 0;
	virtual bool Release(
		const FAvidScriptObjectHandle& Handle,
		FAvidScriptObjectRegistry& Registry,
		FAvidScriptObjectHandleResult& OutResult) = 0;
	virtual bool Owns(
		const FAvidScriptObjectHandle& Handle,
		const UObject* ExpectedObject = nullptr) const = 0;
	virtual bool HasCapability(
		const FAvidScriptObjectHandle& Handle,
		const UObject* ExpectedObject = nullptr) const
	{
		return Owns(Handle, ExpectedObject);
	}
	virtual void Cleanup(FAvidScriptObjectRegistry& Registry) = 0;
};
