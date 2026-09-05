#include "AvidScriptBindingReceiver.h"

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectOwnership.h"
#include "Engine/World.h"
#include "Invocation/AvidScriptBindingCodecProgram.h"

bool ResolveAvidScriptBindingReceiver(
	const FAvidScriptObjectHandle& Handle,
	UClass* ExpectedClass,
	const FAvidScriptBindingInvocationContext& Context,
	UObject*& OutObject,
	FString& OutDetails)
{
	OutObject = nullptr;
	OutDetails.Reset();
	if (!IsInGameThread())
	{
		OutDetails = TEXT("binding_receiver_thread_invalid: receiver authorization requires the game thread.");
		return false;
	}
	if (Context.World.IsStale())
	{
		OutDetails = TEXT("binding_receiver_world_stale: the host world is no longer valid.");
		return false;
	}

	UObject* Object = nullptr;
	if (!UE::AvidScript::BindingPrivate::ResolveObjectHandle(
		Handle.Slot,
		Handle.Generation,
		ExpectedClass,
		Context,
		false,
		Object,
		OutDetails))
	{
		return false;
	}

	UWorld* const ObjectWorld = Object->GetWorld();
	if (ObjectWorld != nullptr && ObjectWorld != Context.World.Get())
	{
		OutDetails = TEXT("binding_receiver_world_mismatch: the receiver belongs to a different world.");
		return false;
	}
	const bool bScopedCapability =
		Context.ScopedObjectCapabilities.Contains(Handle);
	if (Handle != Context.OwnerHandle
		&& !bScopedCapability
		&& (Context.ObjectOwnership == nullptr
			|| !Context.ObjectOwnership->HasCapability(Handle, Object)))
	{
		OutDetails = TEXT("binding_receiver_capability_denied: the receiver has no capability in this session.");
		return false;
	}

	OutObject = Object;
	return true;
}
