#pragma once

#include "AvidScriptVmBackend.h"
#include "CoreMinimal.h"

enum class EAvidScriptDebugSessionState : uint8
{
	Detached,
	Running,
	Paused
};

enum class EAvidScriptDebugRunMode : uint8
{
	Continue,
	PauseNext,
	StepInto
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptDebugSessionSnapshot
{
	EAvidScriptDebugSessionState State = EAvidScriptDebugSessionState::Detached;
	EAvidScriptDebugRunMode RunMode = EAvidScriptDebugRunMode::Continue;
	uint64 Epoch = 0;
	uint64 PauseSequence = 0;
	uint64 ActiveProbeId = 0;
	int32 BreakpointCount = 0;
};

class AVIDSCRIPTRUNTIME_API IAvidScriptDebugProbeHost
{
public:
	virtual ~IAvidScriptDebugProbeHost() = default;
	virtual EAvidScriptDebugProbeAction EvaluateProbe(uint64 ProbeId) = 0;
};

