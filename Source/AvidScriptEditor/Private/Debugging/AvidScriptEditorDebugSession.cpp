#include "Debugging/AvidScriptEditorDebugSession.h"

#include "Misc/Paths.h"

namespace
{
bool NormalizeAvidScriptBreakpointSource(const FString& SourceFile, FString& OutSourceFile)
{
	OutSourceFile = SourceFile;
	FPaths::NormalizeFilename(OutSourceFile);
	if (OutSourceFile.IsEmpty() || !FPaths::IsRelative(OutSourceFile))
	{
		return false;
	}

	TArray<FString> Segments;
	OutSourceFile.ParseIntoArray(Segments, TEXT("/"), false);
	return !Segments.IsEmpty()
		&& !Segments.ContainsByPredicate([](const FString& Segment)
		{
			return Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT("..");
		});
}

bool IsAvidScriptDebuggerAttached(const FAvidScriptDebugSessionSnapshot& Snapshot)
{
	return Snapshot.State != EAvidScriptDebugSessionState::Detached;
}
} // namespace

bool FAvidScriptEditorDebugSessionModel::BindRuntime(
	IAvidScriptEditorDebugRuntime& InRuntime,
	FString& OutError)
{
	check(IsInGameThread());
	if (Runtime != nullptr && Runtime != &InRuntime)
	{
		return Fail(TEXT("unbind the current Runtime Session before binding another one"), OutError);
	}

	Runtime = &InRuntime;
	View.bRuntimeBound = true;
	View.Runtime = Runtime->GetSnapshot();
	if (!RefreshCatalog(false, OutError))
	{
		Runtime = nullptr;
		ClearRuntimeState();
		return false;
	}
	return Refresh(OutError);
}

bool FAvidScriptEditorDebugSessionModel::UnbindRuntime(FString& OutError)
{
	check(IsInGameThread());
	if (Runtime != nullptr)
	{
		View.Runtime = Runtime->GetSnapshot();
		if (IsAvidScriptDebuggerAttached(View.Runtime) && !Runtime->DetachDebugger())
		{
			return Fail(TEXT("the Runtime Session rejected debugger detach"), OutError);
		}
	}

	Runtime = nullptr;
	ClearRuntimeState();
	Succeed(OutError);
	return true;
}

void FAvidScriptEditorDebugSessionModel::NotifyRuntimeDestroyed()
{
	check(IsInGameThread());
	Runtime = nullptr;
	ClearRuntimeState();
}

bool FAvidScriptEditorDebugSessionModel::SetSourceBreakpoint(
	const FString& SourceFile,
	const int32 Line,
	const bool bEnabled,
	FString& OutError)
{
	check(IsInGameThread());
	FString NormalizedSourceFile;
	if (Line <= 0 || !NormalizeAvidScriptBreakpointSource(SourceFile, NormalizedSourceFile))
	{
		return Fail(TEXT("source breakpoints require a project-relative source id and a one-based line"), OutError);
	}

	const TArray<FAvidScriptEditorSourceBreakpoint> PreviousBreakpoints = View.Breakpoints;
	FAvidScriptEditorSourceBreakpoint* Existing = View.Breakpoints.FindByPredicate(
		[&NormalizedSourceFile, Line](const FAvidScriptEditorSourceBreakpoint& Breakpoint)
		{
			return Breakpoint.RequestedLine == Line
				&& Breakpoint.SourceFile.Equals(NormalizedSourceFile, ESearchCase::IgnoreCase);
		});
	if (Existing != nullptr)
	{
		Existing->bEnabled = bEnabled;
	}
	else
	{
		FAvidScriptEditorSourceBreakpoint& Breakpoint = View.Breakpoints.AddDefaulted_GetRef();
		Breakpoint.SourceFile = MoveTemp(NormalizedSourceFile);
		Breakpoint.RequestedLine = Line;
		Breakpoint.bEnabled = bEnabled;
	}
	ResolveBreakpoints();
	if (!SynchronizeBreakpoints(OutError))
	{
		View.Breakpoints = PreviousBreakpoints;
		return false;
	}
	Succeed(OutError);
	return true;
}

bool FAvidScriptEditorDebugSessionModel::RemoveSourceBreakpoint(
	const FString& SourceFile,
	const int32 Line,
	FString& OutError)
{
	check(IsInGameThread());
	FString NormalizedSourceFile;
	if (Line <= 0 || !NormalizeAvidScriptBreakpointSource(SourceFile, NormalizedSourceFile))
	{
		return Fail(TEXT("source breakpoints require a project-relative source id and a one-based line"), OutError);
	}

	const TArray<FAvidScriptEditorSourceBreakpoint> PreviousBreakpoints = View.Breakpoints;
	View.Breakpoints.RemoveAll(
		[&NormalizedSourceFile, Line](const FAvidScriptEditorSourceBreakpoint& Breakpoint)
		{
			return Breakpoint.RequestedLine == Line
				&& Breakpoint.SourceFile.Equals(NormalizedSourceFile, ESearchCase::IgnoreCase);
		});
	if (!SynchronizeBreakpoints(OutError))
	{
		View.Breakpoints = PreviousBreakpoints;
		return false;
	}
	Succeed(OutError);
	return true;
}

bool FAvidScriptEditorDebugSessionModel::AttachDebugger(FString& OutError)
{
	check(IsInGameThread());
	if (Runtime == nullptr)
	{
		return Fail(TEXT("no Runtime Session is bound"), OutError);
	}
	if (IsAvidScriptDebuggerAttached(View.Runtime))
	{
		return Fail(TEXT("the debugger is already attached"), OutError);
	}

	TArray<uint64> ProbeIds;
	CollectEnabledProbeIds(ProbeIds);
	if (!Runtime->AttachDebugger(ProbeIds))
	{
		return Fail(TEXT("the Runtime Session rejected debugger attach"), OutError);
	}
	return Refresh(OutError);
}

bool FAvidScriptEditorDebugSessionModel::DetachDebugger(FString& OutError)
{
	check(IsInGameThread());
	if (Runtime == nullptr)
	{
		return Fail(TEXT("no Runtime Session is bound"), OutError);
	}
	if (!IsAvidScriptDebuggerAttached(View.Runtime))
	{
		Succeed(OutError);
		return true;
	}
	if (!Runtime->DetachDebugger())
	{
		return Fail(TEXT("the Runtime Session rejected debugger detach"), OutError);
	}
	return Refresh(OutError);
}

bool FAvidScriptEditorDebugSessionModel::RequestPause(FString& OutError)
{
	check(IsInGameThread());
	if (Runtime == nullptr || !Runtime->RequestPause())
	{
		return Fail(TEXT("the Runtime Session rejected pause-next"), OutError);
	}
	return Refresh(OutError);
}

bool FAvidScriptEditorDebugSessionModel::ContinueExecution(FString& OutError)
{
	check(IsInGameThread());
	if (Runtime == nullptr || !Runtime->ContinueExecution())
	{
		return Fail(TEXT("the Runtime Session rejected continue"), OutError);
	}
	return Refresh(OutError);
}

bool FAvidScriptEditorDebugSessionModel::StepInto(FString& OutError)
{
	check(IsInGameThread());
	if (Runtime == nullptr || !Runtime->StepInto())
	{
		return Fail(TEXT("the Runtime Session rejected step-into"), OutError);
	}
	return Refresh(OutError);
}

bool FAvidScriptEditorDebugSessionModel::Refresh(FString& OutError)
{
	check(IsInGameThread());
	if (Runtime == nullptr)
	{
		return Fail(TEXT("no Runtime Session is bound"), OutError);
	}

	View.Runtime = Runtime->GetSnapshot();
	if (!bCatalogLoaded || CatalogEpoch != View.Runtime.Epoch)
	{
		if (!RefreshCatalog(true, OutError))
		{
			return false;
		}
	}

	if (View.Runtime.State != EAvidScriptDebugSessionState::Paused)
	{
		View.Variables = FAvidScriptDebugVariablesSnapshot();
		Succeed(OutError);
		return true;
	}

	FAvidScriptDebugVariablesSnapshot Variables;
	FString VariablesError;
	if (!Runtime->GetVariables(Variables, VariablesError))
	{
		View.Variables = FAvidScriptDebugVariablesSnapshot();
		return Fail(VariablesError.IsEmpty()
			? TEXT("the Runtime Session could not provide paused variables")
			: VariablesError, OutError);
	}
	if (Variables.Epoch != View.Runtime.Epoch
		|| Variables.PauseSequence != View.Runtime.PauseSequence
		|| Variables.ActiveProbeId != View.Runtime.ActiveProbeId)
	{
		View.Variables = FAvidScriptDebugVariablesSnapshot();
		return Fail(TEXT("the variable snapshot does not match the active pause"), OutError);
	}

	View.Variables = MoveTemp(Variables);
	Succeed(OutError);
	return true;
}

bool FAvidScriptEditorDebugSessionModel::RefreshCatalog(
	const bool bSynchronizeAttachedRuntime,
	FString& OutError)
{
	TArray<FAvidScriptDebugBreakpoint> NextCatalog;
	FString CatalogError;
	if (Runtime == nullptr || !Runtime->GetBreakpointCatalog(NextCatalog, CatalogError))
	{
		Catalog.Reset();
		bCatalogLoaded = false;
		CatalogEpoch = 0;
		ResolveBreakpoints();
		return Fail(CatalogError.IsEmpty()
			? TEXT("the Runtime Session has no breakpoint catalog")
			: CatalogError, OutError);
	}

	Catalog = MoveTemp(NextCatalog);
	bCatalogLoaded = true;
	CatalogEpoch = View.Runtime.Epoch;
	ResolveBreakpoints();
	if (bSynchronizeAttachedRuntime && IsAvidScriptDebuggerAttached(View.Runtime))
	{
		return SynchronizeBreakpoints(OutError);
	}
	return true;
}

bool FAvidScriptEditorDebugSessionModel::SynchronizeBreakpoints(FString& OutError)
{
	if (Runtime == nullptr || !IsAvidScriptDebuggerAttached(View.Runtime))
	{
		return true;
	}

	TArray<uint64> ProbeIds;
	CollectEnabledProbeIds(ProbeIds);
	if (!Runtime->SetBreakpoints(ProbeIds))
	{
		return Fail(TEXT("the Runtime Session rejected the updated breakpoint set"), OutError);
	}
	View.Runtime.BreakpointCount = ProbeIds.Num();
	return true;
}

void FAvidScriptEditorDebugSessionModel::ResolveBreakpoints()
{
	for (FAvidScriptEditorSourceBreakpoint& Breakpoint : View.Breakpoints)
	{
		Breakpoint.BindingState = EAvidScriptEditorBreakpointBindingState::Unresolved;
		Breakpoint.ProbeId = 0;
		Breakpoint.FunctionName.Reset();
		Breakpoint.Kind.Reset();
		Breakpoint.ResolvedLine = 0;
		Breakpoint.ResolvedColumn = 0;

		const FAvidScriptDebugBreakpoint* Best = nullptr;
		for (const FAvidScriptDebugBreakpoint& Candidate : Catalog)
		{
			if (Candidate.Line != Breakpoint.RequestedLine
				|| !Candidate.SourceFile.Equals(Breakpoint.SourceFile, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (Best == nullptr
				|| Candidate.Column < Best->Column
				|| (Candidate.Column == Best->Column && Candidate.ProbeId < Best->ProbeId))
			{
				Best = &Candidate;
			}
		}

		if (Best != nullptr)
		{
			Breakpoint.BindingState = EAvidScriptEditorBreakpointBindingState::Bound;
			Breakpoint.ProbeId = Best->ProbeId;
			Breakpoint.FunctionName = Best->FunctionName;
			Breakpoint.Kind = Best->Kind;
			Breakpoint.ResolvedLine = Best->Line;
			Breakpoint.ResolvedColumn = Best->Column;
		}
	}
}

void FAvidScriptEditorDebugSessionModel::CollectEnabledProbeIds(
	TArray<uint64>& OutProbeIds) const
{
	OutProbeIds.Reset(View.Breakpoints.Num());
	for (const FAvidScriptEditorSourceBreakpoint& Breakpoint : View.Breakpoints)
	{
		if (Breakpoint.bEnabled && Breakpoint.IsBound())
		{
			OutProbeIds.AddUnique(Breakpoint.ProbeId);
		}
	}
	OutProbeIds.Sort();
}

void FAvidScriptEditorDebugSessionModel::ClearRuntimeState()
{
	Catalog.Reset();
	bCatalogLoaded = false;
	CatalogEpoch = 0;
	View.bRuntimeBound = false;
	View.Runtime = FAvidScriptDebugSessionSnapshot();
	View.Variables = FAvidScriptDebugVariablesSnapshot();
	ResolveBreakpoints();
}

bool FAvidScriptEditorDebugSessionModel::Fail(
	const FString& Error,
	FString& OutError)
{
	View.LastError = Error;
	OutError = Error;
	return false;
}

void FAvidScriptEditorDebugSessionModel::Succeed(FString& OutError)
{
	View.LastError.Reset();
	OutError.Reset();
}
