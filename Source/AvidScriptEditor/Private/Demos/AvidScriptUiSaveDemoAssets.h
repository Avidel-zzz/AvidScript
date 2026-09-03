#pragma once

#include "CoreMinimal.h"

namespace AvidScript::UiSaveDemo
{
bool Prepare(FString& OutError, bool& bOutReused,
	const FString& Root = TEXT("/AvidScript/Demos/UiSave"));
void ConsoleCommand(const TArray<FString>& Arguments);
}
