#pragma once

#include "Debugging/AvidScriptEditorDebugSession.h"
#include "Profiling/AvidScriptEditorProfiler.h"

#include "CoreMinimal.h"

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorDebugTarget
{
	FString TargetId;
	FString DisplayName;
	FString WorldName;
	FString ModuleId;
	const void* RuntimeIdentity = nullptr;
	TFunction<TSharedPtr<IAvidScriptEditorDebugRuntime>()> CreateRuntime;
	TFunction<TSharedPtr<IAvidScriptEditorProfilerRuntime>()> CreateProfilerRuntime;

	bool IsValid() const
	{
		return !TargetId.IsEmpty()
			&& RuntimeIdentity != nullptr
			&& static_cast<bool>(CreateRuntime);
	}
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorDebugTargetController
{
public:
	using FDiscoverTargets = TFunction<void(TArray<FAvidScriptEditorDebugTarget>&)>;

	FAvidScriptEditorDebugTargetController();
	explicit FAvidScriptEditorDebugTargetController(FDiscoverTargets InDiscoverTargets);

	void HandleBeginPIE();
	void HandleEndPIE();
	bool RefreshTargets(FString& OutError);
	bool SelectTarget(const FString& TargetId, FString& OutError);
	bool Tick(FString& OutError);
	bool SetSourceBreakpoint(const FString& SourceFile, int32 Line, bool bEnabled, FString& OutError);
	bool RemoveSourceBreakpoint(const FString& SourceFile, int32 Line, FString& OutError);
	bool AttachDebugger(FString& OutError);
	bool DetachDebugger(FString& OutError);
	bool RequestPause(FString& OutError);
	bool ContinueExecution(FString& OutError);
	bool StepInto(FString& OutError);
	bool SetProfilerCaptureEnabled(bool bEnabled, FString& OutError);
	bool ResetProfiler(FString& OutError);
	bool ExportProfilerJson(const FString& OutputPath, FString& OutError);

	bool IsPIEActive() const { return bPIEActive; }
	const FString& GetSelectedTargetId() const { return SelectedTargetId; }
	TConstArrayView<FAvidScriptEditorDebugTarget> GetTargets() const { return Targets; }
	FAvidScriptEditorDebugSessionModel& GetSessionModel() { return SessionModel; }
	const FAvidScriptEditorDebugSessionModel& GetSessionModel() const { return SessionModel; }
	FAvidScriptEditorProfilerModel& GetProfilerModel() { return ProfilerModel; }
	const FAvidScriptEditorProfilerModel& GetProfilerModel() const { return ProfilerModel; }

	static void DiscoverComponentTargets(TArray<FAvidScriptEditorDebugTarget>& OutTargets);

private:
	bool BindTarget(const FAvidScriptEditorDebugTarget& Target, FString& OutError);
	bool RefreshBeforeCommand(FString& OutError);
	void InvalidateBinding();

	FDiscoverTargets DiscoverTargets;
	TArray<FAvidScriptEditorDebugTarget> Targets;
	FString SelectedTargetId;
	const void* BoundRuntimeIdentity = nullptr;
	TSharedPtr<IAvidScriptEditorDebugRuntime> BoundRuntime;
	TSharedPtr<IAvidScriptEditorProfilerRuntime> BoundProfilerRuntime;
	FAvidScriptEditorDebugSessionModel SessionModel;
	FAvidScriptEditorProfilerModel ProfilerModel;
	bool bPIEActive = false;
};
