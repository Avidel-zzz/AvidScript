#pragma once

#include "CoreMinimal.h"

// Native stack-owned loan for one VM entry. It is never serialized, and nested
// entries cannot inherit it implicitly even when their heap frame depth is equal.
struct FAvidScriptManagedRootTransfer
{
	uint32 InvocationDepth = 0;
	uint32 FrameFloor = 0;
	TArray<uint64, TInlineAllocator<16>> Roots;
};
