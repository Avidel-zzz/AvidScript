#pragma once

#include "AvidScriptDebug.h"

class FAvidScriptSessionDebugger final : public IAvidScriptDebugProbeHost
{
public:
	static constexpr int32 MaxBreakpointCount = 65536;
	static constexpr int32 MaxFrameByteCount = 4096;

	bool Attach(TConstArrayView<uint64> InBreakpoints);
	void Detach();
	bool SetBreakpoints(TConstArrayView<uint64> InBreakpoints);
	bool RequestPause();
	bool ContinueExecution();
	bool StepInto();
	void OnRuntimeGenerationChanged();
	FAvidScriptDebugSessionSnapshot GetSnapshot() const;
	bool CopySuspensionFrame(TArray<uint8>& OutFrameBytes) const;

	EAvidScriptDebugProbeAction EvaluateProbe(uint64 ProbeId) override;
	int64 CommitSuspension(
		uint64 ProbeId,
		uint32 ResumeRoute,
		TConstArrayView<uint8> FrameBytes) override;
	bool ReadSuspensionFrame(
		int64 SuspensionToken,
		TArrayView<uint8> OutFrameBytes) override;
	bool IsExecutionSuspended() const override;

private:
	bool IsAttached() const;
	void Resume(EAvidScriptDebugRunMode NextMode);

	TSet<uint64> Breakpoints;
	EAvidScriptDebugSessionState State = EAvidScriptDebugSessionState::Detached;
	EAvidScriptDebugRunMode RunMode = EAvidScriptDebugRunMode::Continue;
	TOptional<uint64> SuppressedProbeId;
	uint64 Epoch = 0;
	uint64 PauseSequence = 0;
	uint64 ActiveProbeId = 0;
	int64 NextSuspensionToken = 0;
	int64 SuspensionToken = 0;
	uint32 ResumeRoute = 0;
	TArray<uint8> SuspensionFrame;
};
