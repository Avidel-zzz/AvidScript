#pragma once

#include "AvidScriptDebug.h"
#include "CoreMinimal.h"

class FAvidScriptRuntimeSession;

enum class EAvidScriptEditorBreakpointBindingState : uint8
{
	Unresolved,
	Bound
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorSourceBreakpoint
{
	FString SourceFile;
	int32 RequestedLine = 0;
	bool bEnabled = true;
	EAvidScriptEditorBreakpointBindingState BindingState =
		EAvidScriptEditorBreakpointBindingState::Unresolved;
	uint64 ProbeId = 0;
	FString SourceSha256;
	FString FunctionName;
	FString Kind;
	int32 ResolvedLine = 0;
	int32 ResolvedColumn = 0;

	bool IsBound() const
	{
		return BindingState == EAvidScriptEditorBreakpointBindingState::Bound
			&& ProbeId != 0;
	}
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorDebugSessionView
{
	bool bRuntimeBound = false;
	FAvidScriptDebugSessionSnapshot Runtime;
	FAvidScriptDebugVariablesSnapshot Variables;
	TArray<FAvidScriptEditorSourceBreakpoint> Breakpoints;
	FString LastError;
};

class AVIDSCRIPTEDITOR_API IAvidScriptEditorDebugRuntime
{
public:
	virtual ~IAvidScriptEditorDebugRuntime() = default;

	virtual bool GetBreakpointCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const = 0;
	virtual bool AttachDebugger(TConstArrayView<uint64> ProbeIds) = 0;
	virtual bool DetachDebugger() = 0;
	virtual bool SetBreakpoints(TConstArrayView<uint64> ProbeIds) = 0;
	virtual bool RequestPause() = 0;
	virtual bool ContinueExecution() = 0;
	virtual bool StepInto() = 0;
	virtual FAvidScriptDebugSessionSnapshot GetSnapshot() const = 0;
	virtual bool GetVariables(
		FAvidScriptDebugVariablesSnapshot& OutVariables,
		FString& OutError) const = 0;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorRuntimeDebugAdapter final
	: public IAvidScriptEditorDebugRuntime
{
public:
	explicit FAvidScriptEditorRuntimeDebugAdapter(FAvidScriptRuntimeSession& InSession);

	void Reset();
	bool IsValid() const { return Session != nullptr; }

	virtual bool GetBreakpointCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const override;
	virtual bool AttachDebugger(TConstArrayView<uint64> ProbeIds) override;
	virtual bool DetachDebugger() override;
	virtual bool SetBreakpoints(TConstArrayView<uint64> ProbeIds) override;
	virtual bool RequestPause() override;
	virtual bool ContinueExecution() override;
	virtual bool StepInto() override;
	virtual FAvidScriptDebugSessionSnapshot GetSnapshot() const override;
	virtual bool GetVariables(
		FAvidScriptDebugVariablesSnapshot& OutVariables,
		FString& OutError) const override;

private:
	FAvidScriptRuntimeSession* Session = nullptr;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorDebugSessionModel
{
public:
	bool BindRuntime(IAvidScriptEditorDebugRuntime& InRuntime, FString& OutError);
	bool UnbindRuntime(FString& OutError);
	void NotifyRuntimeDestroyed();

	bool SetSourceBreakpoint(
		const FString& SourceFile,
		int32 Line,
		bool bEnabled,
		FString& OutError);
	bool RemoveSourceBreakpoint(
		const FString& SourceFile,
		int32 Line,
		FString& OutError);

	bool AttachDebugger(FString& OutError);
	bool DetachDebugger(FString& OutError);
	bool RequestPause(FString& OutError);
	bool ContinueExecution(FString& OutError);
	bool StepInto(FString& OutError);
	bool Refresh(FString& OutError);

	const FAvidScriptEditorDebugSessionView& GetView() const { return View; }

private:
	bool RefreshCatalog(bool bSynchronizeAttachedRuntime, FString& OutError);
	bool SynchronizeBreakpoints(FString& OutError);
	void ResolveBreakpoints();
	void CollectEnabledProbeIds(TArray<uint64>& OutProbeIds) const;
	void ClearRuntimeState();
	bool Fail(const FString& Error, FString& OutError);
	void Succeed(FString& OutError);

	IAvidScriptEditorDebugRuntime* Runtime = nullptr;
	TArray<FAvidScriptDebugBreakpoint> Catalog;
	FAvidScriptEditorDebugSessionView View;
	uint64 CatalogEpoch = 0;
	bool bCatalogLoaded = false;
};
