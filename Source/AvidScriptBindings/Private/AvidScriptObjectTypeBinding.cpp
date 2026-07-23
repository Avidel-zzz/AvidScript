#include "AvidScriptObjectTypeBinding.h"

#include "AvidScriptHash.h"

namespace
{
FAvidScriptObjectTypeBindingSpec MakeObjectTypeSpec(
	const EAvidScriptBindingInvocationKind Kind,
	const TCHAR* CanonicalIdentity,
	const TCHAR* ImportName,
	const TCHAR* Signature)
{
	FAvidScriptObjectTypeBindingSpec Spec;
	Spec.Kind = Kind;
	Spec.StableId = FAvidScriptHash::Sha256HexUtf8(CanonicalIdentity);
	Spec.ModuleName = TEXT("avidscript");
	Spec.ImportName = ImportName;
	Spec.Signature = Signature;
	return Spec;
}
} // namespace

TConstArrayView<FAvidScriptObjectTypeBindingSpec> FAvidScriptObjectTypeBindings::GetSpecs()
{
	static const TArray<FAvidScriptObjectTypeBindingSpec> Specs = {
		MakeObjectTypeSpec(
			EAvidScriptBindingInvocationKind::ObjectTypeIsA,
			TEXT("avidscript.object_type.v1|is_a|object_handle,object_type_ordinal->i32"),
			TEXT("avid_object_type_is_a"),
			TEXT("(iii)i"))
	};
	return Specs;
}
