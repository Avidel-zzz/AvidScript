#include "Generated/AvidScriptGeneratedCallSite.h"

#include "AvidScriptGeneratedBindingRegistry.h"

bool IsAvidScriptGeneratedCallSiteValid(
	const FAvidScriptGeneratedBindingEntry& Entry)
{
	const int32 PointerCount =
		(Entry.I32PairCall != nullptr ? 1 : 0)
		+ (Entry.PropertyI32Call != nullptr ? 1 : 0)
		+ (Entry.PropertyI32GetCall != nullptr ? 1 : 0)
		+ (Entry.PropertyI32SetCall != nullptr ? 1 : 0)
		+ (Entry.VectorValueCall != nullptr ? 1 : 0)
		+ (Entry.ObjectRoundtripCall != nullptr ? 1 : 0);
	if (PointerCount != 1)
	{
		return false;
	}
	if ((Entry.PreparedI32PairCall != nullptr
			&& Entry.I32PairCall == nullptr)
		|| (Entry.PreparedPropertyI32GetCall != nullptr
			&& Entry.PropertyI32GetCall == nullptr)
		|| (Entry.PreparedPropertyI32SetCall != nullptr
			&& Entry.PropertyI32SetCall == nullptr))
	{
		return false;
	}

	switch (Entry.Shape)
	{
	case EAvidScriptGeneratedBindingShape::I32PairToI32:
		return Entry.I32PairCall != nullptr
			&& Entry.PreparedPropertyI32GetCall == nullptr
			&& Entry.PreparedPropertyI32SetCall == nullptr;
	case EAvidScriptGeneratedBindingShape::PropertyI32GetSet:
		return Entry.PropertyI32Call != nullptr
			&& Entry.PreparedI32PairCall == nullptr
			&& Entry.PreparedPropertyI32GetCall == nullptr
			&& Entry.PreparedPropertyI32SetCall == nullptr;
	case EAvidScriptGeneratedBindingShape::PropertyI32Get:
		return Entry.PropertyI32GetCall != nullptr
			&& Entry.PreparedI32PairCall == nullptr
			&& Entry.PreparedPropertyI32SetCall == nullptr;
	case EAvidScriptGeneratedBindingShape::PropertyI32Set:
		return Entry.PropertyI32SetCall != nullptr
			&& Entry.PreparedI32PairCall == nullptr
			&& Entry.PreparedPropertyI32GetCall == nullptr;
	case EAvidScriptGeneratedBindingShape::VectorValue:
		return Entry.VectorValueCall != nullptr
			&& Entry.PreparedI32PairCall == nullptr
			&& Entry.PreparedPropertyI32GetCall == nullptr
			&& Entry.PreparedPropertyI32SetCall == nullptr;
	case EAvidScriptGeneratedBindingShape::StableObjectRoundtrip:
		return Entry.ObjectRoundtripCall != nullptr
			&& Entry.PreparedI32PairCall == nullptr
			&& Entry.PreparedPropertyI32GetCall == nullptr
			&& Entry.PreparedPropertyI32SetCall == nullptr;
	default:
		return false;
	}
}
