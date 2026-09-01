#pragma once

#include "Debugging/AvidScriptEditorDebugSession.h"

#include "CoreMinimal.h"

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorDebugTarget
{
	FString TargetId;
	FString DisplayName;
	FString WorldName;
	FString ModuleId;
	const void* RuntimeIdentity = nullptr;
	TFunction<TSharedPtr<IAvidScriptEditorDebugRuntime>()> CreateRuntime;

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

	bool IsPIEActive() const { return bPIEActive; }
	const FString& GetSelectedTargetId() const { return SelectedTargetId; }
	TConstArrayView<FAvidScriptEditorDebugTarget> GetTargets() const { return Targets; }
	FAvidScriptEditorDebugSessionModel& GetSessionModel() { return SessionModel; }
	const FAvidScriptEditorDebugSessionModel& GetSessionModel() const { return SessionModel; }

	static void DiscoverComponentTargets(TArray<FAvidScriptEditorDebugTarget>& OutTargets);

private:
	bool BindTarget(const FAvidScriptEditorDebugTarget& Target, FString& OutError);
	void InvalidateBinding();

	FDiscoverTargets DiscoverTargets;
	TArray<FAvidScriptEditorDebugTarget> Targets;
	FString SelectedTargetId;
	const void* BoundRuntimeIdentity = nullptr;
	TSharedPtr<IAvidScriptEditorDebugRuntime> BoundRuntime;
	FAvidScriptEditorDebugSessionModel SessionModel;
	bool bPIEActive = false;
};
