#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

namespace AvidScript::UiSaveDemo
{
template<typename T>
T* ReadUiObject(UObject* Owner, FName Name)
{
	if (!IsValid(Owner)) { return nullptr; }
	FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Owner->GetClass(), Name);
	return Property ? Cast<T>(Property->GetObjectPropertyValue_InContainer(Owner)) : nullptr;
}
}
