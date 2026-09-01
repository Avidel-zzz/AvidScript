#pragma once

#include "AvidScriptVmBackend.h"
#include "CoreMinimal.h"

enum class EAvidScriptDebugSessionState : uint8
{
	Detached,
	Running,
	Suspending,
	Paused,
	Resuming
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
	int64 SuspensionToken = 0;
	uint32 ResumeRoute = 0;
	int32 FrameByteCount = 0;
	int32 BreakpointCount = 0;
};

class AVIDSCRIPTRUNTIME_API IAvidScriptDebugProbeHost
{
public:
	virtual ~IAvidScriptDebugProbeHost() = default;
	virtual EAvidScriptDebugProbeAction EvaluateProbe(uint64 ProbeId) = 0;
	virtual int64 CommitSuspension(
		uint64 ProbeId,
		uint32 ResumeRoute,
		TConstArrayView<uint8> FrameBytes) = 0;
	virtual bool ReadSuspensionFrame(
		int64 SuspensionToken,
		TArrayView<uint8> OutFrameBytes) = 0;
	virtual bool IsExecutionSuspended() const { return false; }
};
