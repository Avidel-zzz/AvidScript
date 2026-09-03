#pragma once

#include "CoreMinimal.h"

namespace AvidScript::UiSaveDemo
{
struct FProbeStep
{
	FString Action;
	FName Button;
	FString Score;
	FString Status;
};

// Returns true only when the process explicitly requested the probe.
bool StartProbe();
void StopProbe();
}
