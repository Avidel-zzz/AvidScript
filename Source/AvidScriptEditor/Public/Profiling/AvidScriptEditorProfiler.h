#pragma once

#include "AvidScriptDebug.h"
#include "CoreMinimal.h"
#include "Profiling/AvidScriptProfiler.h"

class FAvidScriptRuntimeSession;

constexpr uint32 AvidScriptEditorProfilerKindBit(const EAvidScriptProfilerEventKind Kind)
{
	return 1u << static_cast<uint8>(Kind);
}

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorProfilerFilter
{
	uint32 KindMask = MAX_uint32;
	double MinimumDurationMicroseconds = 0.0;
	FString SearchText;
	bool bOnlyFailed = false;
	int32 MaximumRows = 2000;
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorProfilerEventRow
{
	uint64 Sequence = 0;
	uint64 Epoch = 0;
	uint64 ProbeId = 0;
	uint64 CorrelationId = 0;
	uint32 OperationId = 0;
	double DurationMicroseconds = 0.0;
	int64 Value = 0;
	EAvidScriptProfilerEventKind Kind = EAvidScriptProfilerEventKind::GuestCall;
	bool bSucceeded = true;
	FString KindLabel;
	FString SourceFile;
	FString FunctionName;
	int32 SourceLine = 0;
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorProfilerAggregate
{
	EAvidScriptProfilerEventKind Kind = EAvidScriptProfilerEventKind::GuestCall;
	uint32 OperationId = 0;
	uint64 ProbeId = 0;
	int32 CallCount = 0;
	double TotalMicroseconds = 0.0;
	double AverageMicroseconds = 0.0;
	double MaximumMicroseconds = 0.0;
	FString KindLabel;
	FString SourceFile;
	FString FunctionName;
	int32 SourceLine = 0;
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorProfilerView
{
	bool bRuntimeBound = false;
	bool bCaptureEnabled = false;
	uint64 Revision = 0;
	uint64 DroppedEventCount = 0;
	uint64 RejectedThreadEventCount = 0;
	int32 SourceEventCount = 0;
	int32 FilteredEventCount = 0;
	TArray<FAvidScriptEditorProfilerEventRow> Events;
	TArray<FAvidScriptEditorProfilerAggregate> Hotspots;
	FString LastError;
};

class AVIDSCRIPTEDITOR_API IAvidScriptEditorProfilerRuntime
{
public:
	virtual ~IAvidScriptEditorProfilerRuntime() = default;

	virtual void SetProfilerEnabled(bool bEnabled) = 0;
	virtual bool IsProfilerEnabled() const = 0;
	virtual void ResetProfiler() = 0;
	virtual FAvidScriptProfilerSnapshot GetProfilerSnapshot() const = 0;
	virtual bool GetProfilerSourceCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const = 0;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorRuntimeProfilerAdapter final
	: public IAvidScriptEditorProfilerRuntime
{
public:
	explicit FAvidScriptEditorRuntimeProfilerAdapter(FAvidScriptRuntimeSession& InSession);

	void Reset();
	bool IsValid() const { return Session != nullptr; }

	virtual void SetProfilerEnabled(bool bEnabled) override;
	virtual bool IsProfilerEnabled() const override;
	virtual void ResetProfiler() override;
	virtual FAvidScriptProfilerSnapshot GetProfilerSnapshot() const override;
	virtual bool GetProfilerSourceCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const override;

private:
	FAvidScriptRuntimeSession* Session = nullptr;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorProfilerModel
{
public:
	bool BindRuntime(IAvidScriptEditorProfilerRuntime& InRuntime, FString& OutError);
	void UnbindRuntime();
	void NotifyRuntimeDestroyed();

	bool SetCaptureEnabled(bool bEnabled, FString& OutError);
	bool ResetCapture(FString& OutError);
	void SetFilter(const FAvidScriptEditorProfilerFilter& InFilter);
	bool Refresh(FString& OutError);
	bool ExportJson(const FString& OutputPath, FString& OutError) const;

	const FAvidScriptEditorProfilerFilter& GetFilter() const { return Filter; }
	const FAvidScriptEditorProfilerView& GetView() const { return View; }

private:
	bool Fail(const FString& Error, FString& OutError);
	void Succeed(FString& OutError);
	void RebuildView(const FAvidScriptProfilerSnapshot& Snapshot);

	IAvidScriptEditorProfilerRuntime* Runtime = nullptr;
	FAvidScriptEditorProfilerFilter Filter;
	FAvidScriptEditorProfilerView View;
	TMap<uint64, FAvidScriptDebugBreakpoint> SourceCatalog;
	FString SourceCatalogWarning;
};
