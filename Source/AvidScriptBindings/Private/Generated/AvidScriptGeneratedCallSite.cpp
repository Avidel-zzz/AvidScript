#include "Generated/AvidScriptGeneratedCallSite.h"

#include "AvidScriptGeneratedBindingRegistry.h"

bool IsAvidScriptGeneratedCallSiteValid(
	const FAvidScriptGeneratedBindingEntry& Entry)
{
	const int32 PointerCount =
		(Entry.I32PairCall != nullptr ? 1 : 0)
		+ (Entry.PropertyI32Call != nullptr ? 1 : 0)
		+ (Entry.VectorValueCall != nullptr ? 1 : 0)
		+ (Entry.ObjectRoundtripCall != nullptr ? 1 : 0);
	if (PointerCount != 1)
	{
		return false;
	}

	switch (Entry.Shape)
	{
	case EAvidScriptGeneratedBindingShape::I32PairToI32:
		return Entry.I32PairCall != nullptr;
	case EAvidScriptGeneratedBindingShape::PropertyI32GetSet:
		return Entry.PropertyI32Call != nullptr;
	case EAvidScriptGeneratedBindingShape::VectorValue:
		return Entry.VectorValueCall != nullptr;
	case EAvidScriptGeneratedBindingShape::StableObjectRoundtrip:
		return Entry.ObjectRoundtripCall != nullptr;
	default:
		return false;
	}
}
