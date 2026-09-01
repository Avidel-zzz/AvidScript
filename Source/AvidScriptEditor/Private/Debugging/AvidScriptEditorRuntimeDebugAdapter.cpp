#include "Debugging/AvidScriptEditorDebugSession.h"

#include "AvidScriptRuntimeSession.h"

FAvidScriptEditorRuntimeDebugAdapter::FAvidScriptEditorRuntimeDebugAdapter(
	FAvidScriptRuntimeSession& InSession)
	: Session(&InSession)
{
}

void FAvidScriptEditorRuntimeDebugAdapter::Reset()
{
	Session = nullptr;
}

bool FAvidScriptEditorRuntimeDebugAdapter::GetBreakpointCatalog(
	TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
	FString& OutError) const
{
	if (Session == nullptr)
	{
		OutBreakpoints.Reset();
		OutError = TEXT("the Runtime Session adapter is no longer valid");
		return false;
	}
	return Session->GetDebugBreakpointCatalog(OutBreakpoints, OutError);
}

bool FAvidScriptEditorRuntimeDebugAdapter::AttachDebugger(
	const TConstArrayView<uint64> ProbeIds)
{
	return Session != nullptr && Session->AttachDebugger(ProbeIds);
}

bool FAvidScriptEditorRuntimeDebugAdapter::DetachDebugger()
{
	return Session != nullptr && Session->DetachDebugger();
}

bool FAvidScriptEditorRuntimeDebugAdapter::SetBreakpoints(
	const TConstArrayView<uint64> ProbeIds)
{
	return Session != nullptr && Session->SetDebugBreakpoints(ProbeIds);
}

bool FAvidScriptEditorRuntimeDebugAdapter::RequestPause()
{
	return Session != nullptr && Session->RequestDebugPause();
}

bool FAvidScriptEditorRuntimeDebugAdapter::ContinueExecution()
{
	return Session != nullptr && Session->ContinueDebugExecution();
}

bool FAvidScriptEditorRuntimeDebugAdapter::StepInto()
{
	return Session != nullptr && Session->StepIntoDebugExecution();
}

FAvidScriptDebugSessionSnapshot FAvidScriptEditorRuntimeDebugAdapter::GetSnapshot() const
{
	return Session != nullptr
		? Session->GetDebugSnapshot()
		: FAvidScriptDebugSessionSnapshot();
}

bool FAvidScriptEditorRuntimeDebugAdapter::GetVariables(
	FAvidScriptDebugVariablesSnapshot& OutVariables,
	FString& OutError) const
{
	if (Session == nullptr)
	{
		OutVariables = FAvidScriptDebugVariablesSnapshot();
		OutError = TEXT("the Runtime Session adapter is no longer valid");
		return false;
	}
	return Session->GetDebugVariables(OutVariables, OutError);
}
