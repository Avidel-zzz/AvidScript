#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptProfilerEventKind : uint8
{
	RuntimeLoad,
	GuestCall,
	HostCall,
	Continuation,
	Reload,
	Compile,
	Cache
};

enum class EAvidScriptProfilerOperation : uint32
{
	LoadEmbedded = 1,
	LoadInitial = 2,
	Reload = 3,
	Tick = 10,
	Event = 11,
	GameplayEvent = 12,
	DelegateEvent = 13,
	ContinuationDispatch = 14,
	DebugResume = 15,
	EndPlay = 16,
	DynamicHostCall = 20
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptProfilerEvent
{
	uint64 Sequence = 0;
	uint64 StartCycles = 0;
	uint64 DurationCycles = 0;
	uint64 Epoch = 0;
	uint64 ProbeId = 0;
	uint64 CorrelationId = 0;
	uint32 ThreadId = 0;
	uint32 OperationId = 0;
	int64 Value = 0;
	EAvidScriptProfilerEventKind Kind = EAvidScriptProfilerEventKind::GuestCall;
	bool bSucceeded = true;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptProfilerSnapshot
{
	TArray<FAvidScriptProfilerEvent> Events;
	uint64 Revision = 0;
	uint64 DroppedEventCount = 0;
	uint64 RejectedThreadEventCount = 0;
	double SecondsPerCycle = 0.0;
	bool bBufferEnabled = false;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptProfilerEventBuffer
{
public:
	static constexpr int32 DefaultCapacity = 4096;
	static constexpr int32 MaximumCapacity = 16384;

	explicit FAvidScriptProfilerEventBuffer(int32 InCapacity = DefaultCapacity);

	void SetBufferEnabled(bool bEnabled);
	bool IsBufferEnabled() const { return bBufferEnabled; }
	bool IsCaptureEnabled() const;
	void Reset();

	void Record(
		EAvidScriptProfilerEventKind Kind,
		uint32 OperationId,
		uint64 StartCycles,
		uint64 DurationCycles,
		uint64 Epoch = 0,
		uint64 ProbeId = 0,
		uint64 CorrelationId = 0,
		int64 Value = 0,
		bool bSucceeded = true);

	FAvidScriptProfilerSnapshot Snapshot() const;
	int32 GetCapacity() const { return Capacity; }
	uint64 GetRevision() const { return Revision; }

private:
	TArray<FAvidScriptProfilerEvent> Events;
	int32 Capacity = 0;
	int32 Count = 0;
	int32 WriteIndex = 0;
	uint32 OwnerThreadId = 0;
	uint64 NextSequence = 1;
	uint64 Revision = 0;
	uint64 DroppedEventCount = 0;
	uint64 RejectedThreadEventCount = 0;
	bool bBufferEnabled = false;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptProfilerScope
{
public:
	FAvidScriptProfilerScope(
		FAvidScriptProfilerEventBuffer* InBuffer,
		EAvidScriptProfilerEventKind InKind,
		uint32 InOperationId,
		uint64 InEpoch = 0,
		uint64 InProbeId = 0,
		uint64 InCorrelationId = 0,
		int64 InValue = 0);
	~FAvidScriptProfilerScope();

	FAvidScriptProfilerScope(const FAvidScriptProfilerScope&) = delete;
	FAvidScriptProfilerScope& operator=(const FAvidScriptProfilerScope&) = delete;

	void SetSucceeded(bool bInSucceeded) { bSucceeded = bInSucceeded; }
	bool IsCapturing() const { return bCapturing; }

private:
	FAvidScriptProfilerEventBuffer* Buffer = nullptr;
	uint64 StartCycles = 0;
	uint64 Epoch = 0;
	uint64 ProbeId = 0;
	uint64 CorrelationId = 0;
	int64 Value = 0;
	uint32 OperationId = 0;
	EAvidScriptProfilerEventKind Kind = EAvidScriptProfilerEventKind::GuestCall;
	bool bSucceeded = true;
	bool bCapturing = false;
};
