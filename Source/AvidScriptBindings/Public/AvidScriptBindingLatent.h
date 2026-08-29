#pragma once

#include "CoreMinimal.h"

class UObject;

struct FAvidScriptBindingLatentReservation
{
	int64 Token = 0;
	UObject* CallbackTarget = nullptr;
	FName ExecutionFunction;
	int32 UUID = 0;
	int32 Linkage = INDEX_NONE;

	bool IsValid() const
	{
		return Token != 0
			&& CallbackTarget != nullptr
			&& !ExecutionFunction.IsNone()
			&& UUID > 0
			&& Linkage >= 0;
	}
};

class IAvidScriptBindingLatentHost
{
public:
	virtual ~IAvidScriptBindingLatentHost() = default;

	virtual bool BeginLatent(
		int32 CallbackId,
		FAvidScriptBindingLatentReservation& OutReservation) = 0;
	virtual bool CommitLatent(int64 Token) = 0;
	virtual bool AbortLatent(int64 Token) = 0;
};
