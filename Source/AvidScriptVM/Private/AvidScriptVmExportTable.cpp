#include "AvidScriptVmExportTable.h"

bool FAvidScriptVmExportTable::TryGet(
	const FAvidScriptVmExportHandle& Handle,
	void*& OutFunction,
	FAvidScriptVmError& OutError) const
{
	OutFunction = nullptr;
	OutError.Reset();

	if (!Handle.IsValid())
	{
		OutError.Category = TEXT("invalid_export");
		OutError.Details = TEXT("The export handle is empty.");
		return false;
	}

	if (Handle.Generation != Generation)
	{
		OutError.Category = TEXT("stale_export");
		OutError.Details = TEXT("The export handle belongs to an unloaded VM generation.");
		return false;
	}

	const int32 FunctionIndex = static_cast<int32>(Handle.Slot - 1);
	if (!Functions.IsValidIndex(FunctionIndex) || Functions[FunctionIndex] == nullptr)
	{
		OutError.Category = TEXT("invalid_export");
		OutError.Details = TEXT("The export handle slot is outside the active VM export table.");
		return false;
	}

	OutFunction = Functions[FunctionIndex];
	return true;
}

void FAvidScriptVmExportTable::Reset()
{
	Names.Reset();
	Functions.Reset();
	NameToSlot.Reset();
	LookupCount = 0;
	Generation = AdvanceGeneration(Generation);
}

uint32 FAvidScriptVmExportTable::AdvanceGeneration(uint32 CurrentGeneration)
{
	const uint32 NextGeneration = CurrentGeneration + 1;
	return NextGeneration != 0 ? NextGeneration : 1;
}
