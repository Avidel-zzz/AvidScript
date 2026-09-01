#pragma once

#include "AvidScriptDebug.h"

class FAvidScriptSessionDebugger final : public IAvidScriptDebugProbeHost
{
public:
	static constexpr int32 MaxBreakpointCount = 65536;

	bool Attach(TConstArrayView<uint64> InBreakpoints);
	void Detach();
	bool SetBreakpoints(TConstArrayView<uint64> InBreakpoints);
	bool RequestPause();
	bool ContinueExecution();
	bool StepInto();
	void OnRuntimeGenerationChanged();
	FAvidScriptDebugSessionSnapshot GetSnapshot() const;

	EAvidScriptDebugProbeAction EvaluateProbe(uint64 ProbeId) override;

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
};

