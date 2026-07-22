#include "AvidScriptObjectLifecycleBinding.h"

#include "AvidScriptHash.h"

namespace
{
FAvidScriptObjectLifecycleBindingSpec MakeLifecycleSpec(
	const EAvidScriptBindingInvocationKind Kind,
	const TCHAR* CanonicalIdentity,
	const TCHAR* ImportName,
	const TCHAR* Signature)
{
	FAvidScriptObjectLifecycleBindingSpec Spec;
	Spec.Kind = Kind;
	Spec.StableId = FAvidScriptHash::Sha256HexUtf8(CanonicalIdentity);
	Spec.ModuleName = TEXT("avidscript");
	Spec.ImportName = ImportName;
	Spec.Signature = Signature;
	return Spec;
}
} // namespace

TConstArrayView<FAvidScriptObjectLifecycleBindingSpec> FAvidScriptObjectLifecycleBindings::GetSpecs()
{
	static const TArray<FAvidScriptObjectLifecycleBindingSpec> Specs = {
		MakeLifecycleSpec(
			EAvidScriptBindingInvocationKind::ObjectSpawnActor,
			TEXT("avidscript.lifecycle.v1|spawn_actor|class_ref,transform_ptr,handle_ptr->i32"),
			TEXT("avid_object_spawn_actor"),
			TEXT("(iii)i")),
		MakeLifecycleSpec(
			EAvidScriptBindingInvocationKind::ObjectDestroyActor,
			TEXT("avidscript.lifecycle.v1|destroy_actor|object_handle->i32"),
			TEXT("avid_object_destroy_actor"),
			TEXT("(ii)i")),
		MakeLifecycleSpec(
			EAvidScriptBindingInvocationKind::ObjectIsA,
			TEXT("avidscript.lifecycle.v1|is_a|object_handle,class_ref->i32"),
			TEXT("avid_object_is_a"),
			TEXT("(iii)i"))
	};
	return Specs;
}
