#include "AvidScriptObjectFactoryPolicy.h"

const TCHAR* LexToString(const EAvidScriptObjectFactoryKind Kind)
{
	switch (Kind)
	{
	case EAvidScriptObjectFactoryKind::NewObject:
		return TEXT("new_object");
	case EAvidScriptObjectFactoryKind::ActorComponent:
		return TEXT("actor_component");
	default:
		return TEXT("invalid");
	}
}

const TCHAR* LexToString(const EAvidScriptObjectOwnershipPolicy Ownership)
{
	switch (Ownership)
	{
	case EAvidScriptObjectOwnershipPolicy::Session:
		return TEXT("session");
	default:
		return TEXT("invalid");
	}
}

const TCHAR* LexToString(
	const EAvidScriptComponentRegistrationPolicy Registration)
{
	switch (Registration)
	{
	case EAvidScriptComponentRegistrationPolicy::None:
		return TEXT("none");
	case EAvidScriptComponentRegistrationPolicy::RegisterInstance:
		return TEXT("register_instance");
	default:
		return TEXT("invalid");
	}
}

bool TryParseAvidScriptObjectFactoryKind(
	const FString& Value,
	EAvidScriptObjectFactoryKind& OutKind)
{
	if (Value == TEXT("new_object"))
	{
		OutKind = EAvidScriptObjectFactoryKind::NewObject;
		return true;
	}
	if (Value == TEXT("actor_component"))
	{
		OutKind = EAvidScriptObjectFactoryKind::ActorComponent;
		return true;
	}
	return false;
}

bool TryParseAvidScriptObjectOwnershipPolicy(
	const FString& Value,
	EAvidScriptObjectOwnershipPolicy& OutOwnership)
{
	if (Value != TEXT("session"))
	{
		return false;
	}
	OutOwnership = EAvidScriptObjectOwnershipPolicy::Session;
	return true;
}

bool TryParseAvidScriptComponentRegistrationPolicy(
	const FString& Value,
	EAvidScriptComponentRegistrationPolicy& OutRegistration)
{
	if (Value == TEXT("none"))
	{
		OutRegistration = EAvidScriptComponentRegistrationPolicy::None;
		return true;
	}
	if (Value == TEXT("register_instance"))
	{
		OutRegistration =
			EAvidScriptComponentRegistrationPolicy::RegisterInstance;
		return true;
	}
	return false;
}
