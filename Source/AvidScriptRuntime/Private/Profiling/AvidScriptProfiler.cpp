#include "Profiling/AvidScriptProfiler.h"

#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Trace/Trace.h"

UE_TRACE_CHANNEL_DEFINE(
	AvidScriptProfilerChannel,
	"AvidScript Runtime, guest, host, continuation, reload, compile and cache profiling events.");

UE_TRACE_EVENT_BEGIN(AvidScript, ProfilerEvent, NoSync)
	UE_TRACE_EVENT_FIELD(uint64, Sequence)
	UE_TRACE_EVENT_FIELD(uint64, StartCycles)
	UE_TRACE_EVENT_FIELD(uint64, DurationCycles)
	UE_TRACE_EVENT_FIELD(uint64, Epoch)
	UE_TRACE_EVENT_FIELD(uint64, ProbeId)
	UE_TRACE_EVENT_FIELD(uint64, CorrelationId)
	UE_TRACE_EVENT_FIELD(uint32, ThreadId)
	UE_TRACE_EVENT_FIELD(uint32, OperationId)
	UE_TRACE_EVENT_FIELD(int64, Value)
	UE_TRACE_EVENT_FIELD(uint8, Kind)
	UE_TRACE_EVENT_FIELD(bool, Succeeded)
UE_TRACE_EVENT_END()

FAvidScriptProfilerEventBuffer::FAvidScriptProfilerEventBuffer(
	const int32 InCapacity)
	: Capacity(FMath::Clamp(InCapacity, 1, MaximumCapacity))
{
	Events.SetNumZeroed(Capacity);
}

void FAvidScriptProfilerEventBuffer::SetBufferEnabled(const bool bEnabled)
{
	check(IsInGameThread());
	if (bBufferEnabled == bEnabled)
	{
		return;
	}
	bBufferEnabled = bEnabled;
	if (bEnabled)
	{
		OwnerThreadId = FPlatformTLS::GetCurrentThreadId();
		Reset();
	}
}

bool FAvidScriptProfilerEventBuffer::IsCaptureEnabled() const
{
	return bBufferEnabled
		|| UE_TRACE_CHANNELEXPR_IS_ENABLED(AvidScriptProfilerChannel);
}

void FAvidScriptProfilerEventBuffer::Reset()
{
	check(IsInGameThread());
	Count = 0;
	WriteIndex = 0;
	NextSequence = 1;
	DroppedEventCount = 0;
	RejectedThreadEventCount = 0;
	++Revision;
}

void FAvidScriptProfilerEventBuffer::Record(
	const EAvidScriptProfilerEventKind Kind,
	const uint32 OperationId,
	const uint64 StartCycles,
	const uint64 DurationCycles,
	const uint64 Epoch,
	const uint64 ProbeId,
	const uint64 CorrelationId,
	const int64 Value,
	const bool bSucceeded)
{
	const bool bTraceEnabled =
		UE_TRACE_CHANNELEXPR_IS_ENABLED(AvidScriptProfilerChannel);
	if (!bBufferEnabled && !bTraceEnabled)
	{
		return;
	}

	FAvidScriptProfilerEvent Event;
	Event.Sequence = NextSequence++;
	Event.StartCycles = StartCycles;
	Event.DurationCycles = DurationCycles;
	Event.Epoch = Epoch;
	Event.ProbeId = ProbeId;
	Event.CorrelationId = CorrelationId;
	Event.ThreadId = FPlatformTLS::GetCurrentThreadId();
	Event.OperationId = OperationId;
	Event.Value = Value;
	Event.Kind = Kind;
	Event.bSucceeded = bSucceeded;

	if (bTraceEnabled)
	{
		UE_TRACE_LOG(AvidScript, ProfilerEvent, AvidScriptProfilerChannel)
			<< ProfilerEvent.Sequence(Event.Sequence)
			<< ProfilerEvent.StartCycles(Event.StartCycles)
			<< ProfilerEvent.DurationCycles(Event.DurationCycles)
			<< ProfilerEvent.Epoch(Event.Epoch)
			<< ProfilerEvent.ProbeId(Event.ProbeId)
			<< ProfilerEvent.CorrelationId(Event.CorrelationId)
			<< ProfilerEvent.ThreadId(Event.ThreadId)
			<< ProfilerEvent.OperationId(Event.OperationId)
			<< ProfilerEvent.Value(Event.Value)
			<< ProfilerEvent.Kind(static_cast<uint8>(Event.Kind))
			<< ProfilerEvent.Succeeded(Event.bSucceeded);
	}

	if (!bBufferEnabled)
	{
		return;
	}
	if (Event.ThreadId != OwnerThreadId)
	{
		++RejectedThreadEventCount;
		++Revision;
		return;
	}

	Events[WriteIndex] = Event;
	WriteIndex = (WriteIndex + 1) % Capacity;
	if (Count < Capacity)
	{
		++Count;
	}
	else
	{
		++DroppedEventCount;
	}
	++Revision;
}

FAvidScriptProfilerSnapshot FAvidScriptProfilerEventBuffer::Snapshot() const
{
	check(IsInGameThread());
	FAvidScriptProfilerSnapshot Result;
	Result.Events.Reserve(Count);
	const int32 FirstIndex = Count == Capacity ? WriteIndex : 0;
	for (int32 Offset = 0; Offset < Count; ++Offset)
	{
		Result.Events.Add(Events[(FirstIndex + Offset) % Capacity]);
	}
	Result.Revision = Revision;
	Result.DroppedEventCount = DroppedEventCount;
	Result.RejectedThreadEventCount = RejectedThreadEventCount;
	Result.SecondsPerCycle = FPlatformTime::GetSecondsPerCycle64();
	Result.bBufferEnabled = bBufferEnabled;
	return Result;
}

FAvidScriptProfilerScope::FAvidScriptProfilerScope(
	FAvidScriptProfilerEventBuffer* InBuffer,
	const EAvidScriptProfilerEventKind InKind,
	const uint32 InOperationId,
	const uint64 InEpoch,
	const uint64 InProbeId,
	const uint64 InCorrelationId,
	const int64 InValue)
	: Buffer(InBuffer)
	, Epoch(InEpoch)
	, ProbeId(InProbeId)
	, CorrelationId(InCorrelationId)
	, Value(InValue)
	, OperationId(InOperationId)
	, Kind(InKind)
{
	bCapturing = Buffer != nullptr && Buffer->IsCaptureEnabled();
	if (bCapturing)
	{
		StartCycles = FPlatformTime::Cycles64();
	}
}

FAvidScriptProfilerScope::~FAvidScriptProfilerScope()
{
	if (!bCapturing || Buffer == nullptr)
	{
		return;
	}
	const uint64 EndCycles = FPlatformTime::Cycles64();
	Buffer->Record(
		Kind,
		OperationId,
		StartCycles,
		EndCycles - StartCycles,
		Epoch,
		ProbeId,
		CorrelationId,
		Value,
		bSucceeded);
}
