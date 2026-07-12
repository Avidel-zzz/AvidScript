#pragma once

#include "AvidScriptObjectRegistry.h"

#include "CoreMinimal.h"

enum class EAvidScriptGameplayEventType : uint8
{
	Generic = 0,
	BeginOverlap = 1,
	EndOverlap = 2,
	Hit = 3,
	Input = 4
};

struct FAvidScriptGameplayEvent
{
	EAvidScriptGameplayEventType Type = EAvidScriptGameplayEventType::Generic;
	int32 PrimaryId = 0;
	int32 SecondaryId = 0;
	FAvidScriptObjectHandle ObjectHandle;
	FVector3f VectorValue = FVector3f::ZeroVector;
};