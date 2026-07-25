#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptObjectFactoryKind : uint8
{
	NewObject,
	ActorComponent
};

enum class EAvidScriptObjectOwnershipPolicy : uint8
{
	Session
};

enum class EAvidScriptComponentRegistrationPolicy : uint8
{
	None,
	RegisterInstance
};

AVIDSCRIPTBINDINGS_API const TCHAR* LexToString(
	EAvidScriptObjectFactoryKind Kind);

AVIDSCRIPTBINDINGS_API const TCHAR* LexToString(
	EAvidScriptObjectOwnershipPolicy Ownership);

AVIDSCRIPTBINDINGS_API const TCHAR* LexToString(
	EAvidScriptComponentRegistrationPolicy Registration);

AVIDSCRIPTBINDINGS_API bool TryParseAvidScriptObjectFactoryKind(
	const FString& Value,
	EAvidScriptObjectFactoryKind& OutKind);

AVIDSCRIPTBINDINGS_API bool TryParseAvidScriptObjectOwnershipPolicy(
	const FString& Value,
	EAvidScriptObjectOwnershipPolicy& OutOwnership);

AVIDSCRIPTBINDINGS_API bool TryParseAvidScriptComponentRegistrationPolicy(
	const FString& Value,
	EAvidScriptComponentRegistrationPolicy& OutRegistration);
