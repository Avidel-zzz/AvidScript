#pragma once

#include "CoreMinimal.h"

class FAvidScriptWasmRuntimeInstance;

// Native ownership shared by asynchronous state owners. Never extends the
// Runtime lifetime or exposes Guest root tokens.
class AVIDSCRIPTRUNTIME_API IAvidScriptManagedStateLease
{
public:
	virtual ~IAvidScriptManagedStateLease() = default;
	virtual bool IsValidForRuntime(const FAvidScriptWasmRuntimeInstance& Runtime) const = 0;
};
