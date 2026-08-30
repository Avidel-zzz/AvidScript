#pragma once

#include "CoreMinimal.h"

struct FAvidScriptValueCapabilityImportSpec
{
	const TCHAR* StableId = nullptr;
	const TCHAR* ModuleName = nullptr;
	const TCHAR* ImportName = nullptr;
	const TCHAR* Signature = nullptr;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptValueCapability
{
public:
	static constexpr uint32 TokenTag = 0x80000000u;
	static constexpr int32 LegacyArrayImportCount = 4;

	static bool IsToken(uint32 ValueReference);
	static uint32 AllocateToken();
	static TConstArrayView<FAvidScriptValueCapabilityImportSpec> GetArrayImportSpecs();
	static TConstArrayView<FAvidScriptValueCapabilityImportSpec> GetCompositeImportSpecs();
	static TConstArrayView<FAvidScriptValueCapabilityImportSpec> GetCompositeContainerImportSpecs();
};
