#pragma once

#include "CoreMinimal.h"

class FAvidScriptGeneratedTypeRegistrySnapshot;

// Per-instance endpoint. Runtime keeps a weak reference so retained contexts
// cannot extend the lifetime of an ended or destroyed Session.
class IAvidScriptGeneratedTypeAuthority
{
public:
	virtual ~IAvidScriptGeneratedTypeAuthority() = default;
	virtual UObject* ResolveGeneratedTypeReceiver(
		int64 PackedSelf, uint32 TypeOrdinal, const FAvidScriptGeneratedTypeRegistrySnapshot& Registry) const = 0;
};
