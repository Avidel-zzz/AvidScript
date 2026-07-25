#pragma once

#include "CoreMinimal.h"

// Values are part of the package invocation ABI. Append only.
enum class EAvidScriptBindingInvocationKind : uint8
{
	ReflectedFunction = 0,
	ReflectedPropertyRead = 1,
	ObjectSpawnActor = 2,
	ObjectDestroyActor = 3,
	ObjectIsA = 4,
	ObjectTypeIsA = 5,
	ObjectConstruct = 6,
	ObjectRelease = 7,
	ActorFindComponent = 8
};
