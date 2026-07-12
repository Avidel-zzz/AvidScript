#pragma once

#include "AvidScriptVmBackend.h"

class FAvidScriptVmExportTable
{
public:
	template <typename TResolver>
	bool ResolveOrCache(const FString& ExportName, TResolver&& Resolver, FAvidScriptVmExportHandle& OutHandle)
	{
		if (const uint32* ExistingSlot = NameToSlot.Find(ExportName))
		{
			OutHandle = { *ExistingSlot, Generation };
			return true;
		}

		void* Function = Resolver();
		++LookupCount;
		if (Function == nullptr)
		{
			OutHandle = {};
			return false;
		}

		const uint32 Slot = static_cast<uint32>(Functions.Add(Function) + 1);
		Names.Add(ExportName);
		NameToSlot.Add(ExportName, Slot);
		OutHandle = { Slot, Generation };
		return true;
	}

	bool TryGet(const FAvidScriptVmExportHandle& Handle, void*& OutFunction, FAvidScriptVmError& OutError) const;
	void Reset();
	uint32 GetLookupCount() const { return LookupCount; }

private:
	static uint32 AdvanceGeneration(uint32 CurrentGeneration);

	TArray<FString> Names;
	TArray<void*> Functions;
	TMap<FString, uint32> NameToSlot;
	uint32 Generation = 1;
	uint32 LookupCount = 0;
};
