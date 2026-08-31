#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"

namespace UE::AvidScript::BindingPrivate
{
inline bool IsObjectCompatibleWithReflectedType(
	const UObject* Object,
	const UClass* ExpectedClass)
{
	if (Object == nullptr || ExpectedClass == nullptr)
	{
		return false;
	}
	return ExpectedClass->HasAnyClassFlags(CLASS_Interface)
		? Object->GetClass()->ImplementsInterface(ExpectedClass)
		: Object->IsA(ExpectedClass);
}
} // namespace UE::AvidScript::BindingPrivate
