#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptContinuationStatus : int32
{
	Completed = 1
};

struct FAvidScriptContinuationCompletion
{
	int32 CallbackId = 0;
	int64 Token = 0;
	EAvidScriptContinuationStatus Status =
		EAvidScriptContinuationStatus::Completed;
	uint64 RegistrationSerial = 0;
};

class AVIDSCRIPTRUNTIME_API IAvidScriptContinuationHost
{
public:
	virtual ~IAvidScriptContinuationHost() = default;

	virtual int64 ScheduleDelay(float DelaySeconds, int32 CallbackId) = 0;
	virtual bool Cancel(int64 Token) = 0;
};
