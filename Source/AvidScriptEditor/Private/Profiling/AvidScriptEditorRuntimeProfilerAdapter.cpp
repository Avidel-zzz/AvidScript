#include "Profiling/AvidScriptEditorProfiler.h"

#include "AvidScriptRuntimeSession.h"

FAvidScriptEditorRuntimeProfilerAdapter::FAvidScriptEditorRuntimeProfilerAdapter(
	FAvidScriptRuntimeSession& InSession)
	: Session(&InSession)
{
}

void FAvidScriptEditorRuntimeProfilerAdapter::Reset()
{
	Session = nullptr;
}

void FAvidScriptEditorRuntimeProfilerAdapter::SetProfilerEnabled(const bool bEnabled)
{
	if (Session != nullptr)
	{
		Session->SetProfilerEnabled(bEnabled);
	}
}

bool FAvidScriptEditorRuntimeProfilerAdapter::IsProfilerEnabled() const
{
	return Session != nullptr && Session->IsProfilerEnabled();
}

void FAvidScriptEditorRuntimeProfilerAdapter::ResetProfiler()
{
	if (Session != nullptr)
	{
		Session->ResetProfiler();
	}
}

FAvidScriptProfilerSnapshot FAvidScriptEditorRuntimeProfilerAdapter::GetProfilerSnapshot() const
{
	return Session != nullptr
		? Session->GetProfilerSnapshot()
		: FAvidScriptProfilerSnapshot();
}

bool FAvidScriptEditorRuntimeProfilerAdapter::GetProfilerSourceCatalog(
	TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
	FString& OutError) const
{
	if (Session == nullptr)
	{
		OutBreakpoints.Reset();
		OutError = TEXT("AvidScript profiler Runtime adapter is no longer bound.");
		return false;
	}
	return Session->GetDebugBreakpointCatalog(OutBreakpoints, OutError);
}
