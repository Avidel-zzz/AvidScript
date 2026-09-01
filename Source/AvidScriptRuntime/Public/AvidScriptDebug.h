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

struct AVIDSCRIPTRUNTIME_API FAvidScriptDebugBreakpoint
{
	uint64 ProbeId = 0;
	uint32 FunctionIndex = MAX_uint32;
	uint32 FunctionOffset = 0;
	FString SourceFile;
	FString SourceSha256;
	FString FunctionName;
	FString Kind;
	int32 Start = 0;
	int32 Length = 0;
	int32 Line = 0;
	int32 Column = 0;
	int32 EndLine = 0;
	int32 EndColumn = 0;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptDebugVariableSnapshot
{
	FString SymbolId;
	FString Name;
	FString Kind;
	FString TypeId;
	FString ValueKind;
	FString Value;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptDebugVariablesSnapshot
{
	uint64 Epoch = 0;
	uint64 PauseSequence = 0;
	uint64 ActiveProbeId = 0;
	FString SourceFile;
	FString SourceSha256;
	FString FunctionName;
	int32 Line = 0;
	int32 Column = 0;
	bool bTruncated = false;
	TArray<FAvidScriptDebugVariableSnapshot> Variables;
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
