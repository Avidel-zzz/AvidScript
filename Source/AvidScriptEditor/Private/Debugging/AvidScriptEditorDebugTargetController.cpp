#include "Debugging/AvidScriptEditorDebugTargetController.h"

#include "AvidScriptComponent.h"
#include "AvidScriptRuntimeSession.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectIterator.h"

FAvidScriptEditorDebugTargetController::FAvidScriptEditorDebugTargetController()
	: FAvidScriptEditorDebugTargetController(
		[](TArray<FAvidScriptEditorDebugTarget>& OutTargets)
		{
			DiscoverComponentTargets(OutTargets);
		})
{
}

FAvidScriptEditorDebugTargetController::FAvidScriptEditorDebugTargetController(
	FDiscoverTargets InDiscoverTargets)
	: DiscoverTargets(MoveTemp(InDiscoverTargets))
{
	check(DiscoverTargets);
}

void FAvidScriptEditorDebugTargetController::HandleBeginPIE()
{
	check(IsInGameThread());
	bPIEActive = true;
}

void FAvidScriptEditorDebugTargetController::HandleEndPIE()
{
	check(IsInGameThread());
	bPIEActive = false;
	Targets.Reset();
	SelectedTargetId.Reset();
	InvalidateBinding();
}

bool FAvidScriptEditorDebugTargetController::RefreshTargets(FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (!bPIEActive)
	{
		Targets.Reset();
		SelectedTargetId.Reset();
		InvalidateBinding();
		return true;
	}

	TArray<FAvidScriptEditorDebugTarget> NextTargets;
	DiscoverTargets(NextTargets);
	NextTargets.RemoveAll([](const FAvidScriptEditorDebugTarget& Target)
	{
		return !Target.IsValid();
	});
	NextTargets.Sort([](
		const FAvidScriptEditorDebugTarget& Left,
		const FAvidScriptEditorDebugTarget& Right)
	{
		return Left.TargetId < Right.TargetId;
	});
	for (int32 Index = NextTargets.Num() - 1; Index > 0; --Index)
	{
		if (NextTargets[Index].TargetId == NextTargets[Index - 1].TargetId)
		{
			NextTargets.RemoveAt(Index);
		}
	}

	const FAvidScriptEditorDebugTarget* NextSelected = NextTargets.FindByPredicate(
		[this](const FAvidScriptEditorDebugTarget& Target)
		{
			return Target.TargetId == SelectedTargetId;
		});
	const bool bSelectedRuntimeChanged = NextSelected != nullptr
		&& NextSelected->RuntimeIdentity != BoundRuntimeIdentity;
	if (NextSelected == nullptr || bSelectedRuntimeChanged)
	{
		InvalidateBinding();
	}

	Targets = MoveTemp(NextTargets);
	if (Targets.IsEmpty())
	{
		SelectedTargetId.Reset();
		return true;
	}

	if (NextSelected == nullptr)
	{
		SelectedTargetId = Targets[0].TargetId;
	}
	const FAvidScriptEditorDebugTarget* Selected = Targets.FindByPredicate(
		[this](const FAvidScriptEditorDebugTarget& Target)
		{
			return Target.TargetId == SelectedTargetId;
		});
	if (Selected == nullptr)
	{
		OutError = TEXT("the selected debug target disappeared during refresh");
		return false;
	}
	if (!BoundRuntime.IsValid())
	{
		return BindTarget(*Selected, OutError);
	}
	return true;
}

bool FAvidScriptEditorDebugTargetController::SelectTarget(
	const FString& TargetId,
	FString& OutError)
{
	check(IsInGameThread());
	const FAvidScriptEditorDebugTarget* Target = Targets.FindByPredicate(
		[&TargetId](const FAvidScriptEditorDebugTarget& Candidate)
		{
			return Candidate.TargetId == TargetId;
		});
	if (Target == nullptr)
	{
		OutError = TEXT("the requested debug target is not available");
		return false;
	}
	if (TargetId == SelectedTargetId
		&& BoundRuntime.IsValid()
		&& BoundRuntimeIdentity == Target->RuntimeIdentity)
	{
		OutError.Reset();
		return true;
	}

	if (BoundRuntime.IsValid() && !SessionModel.UnbindRuntime(OutError))
	{
		return false;
	}
	BoundRuntime.Reset();
	BoundRuntimeIdentity = nullptr;
	SelectedTargetId = TargetId;
	return BindTarget(*Target, OutError);
}

bool FAvidScriptEditorDebugTargetController::Tick(FString& OutError)
{
	check(IsInGameThread());
	if (!RefreshTargets(OutError))
	{
		return false;
	}
	if (!BoundRuntime.IsValid())
	{
		OutError.Reset();
		return true;
	}
	return SessionModel.Refresh(OutError);
}

bool FAvidScriptEditorDebugTargetController::SetSourceBreakpoint(
	const FString& SourceFile,
	const int32 Line,
	const bool bEnabled,
	FString& OutError)
{
	check(IsInGameThread());
	if (bPIEActive && !RefreshTargets(OutError))
	{
		return false;
	}
	return SessionModel.SetSourceBreakpoint(SourceFile, Line, bEnabled, OutError);
}

bool FAvidScriptEditorDebugTargetController::RemoveSourceBreakpoint(
	const FString& SourceFile,
	const int32 Line,
	FString& OutError)
{
	check(IsInGameThread());
	if (bPIEActive && !RefreshTargets(OutError))
	{
		return false;
	}
	return SessionModel.RemoveSourceBreakpoint(SourceFile, Line, OutError);
}

bool FAvidScriptEditorDebugTargetController::AttachDebugger(FString& OutError)
{
	return RefreshBeforeCommand(OutError)
		&& SessionModel.AttachDebugger(OutError);
}

bool FAvidScriptEditorDebugTargetController::DetachDebugger(FString& OutError)
{
	return RefreshBeforeCommand(OutError)
		&& SessionModel.DetachDebugger(OutError);
}

bool FAvidScriptEditorDebugTargetController::RequestPause(FString& OutError)
{
	return RefreshBeforeCommand(OutError)
		&& SessionModel.RequestPause(OutError);
}

bool FAvidScriptEditorDebugTargetController::ContinueExecution(FString& OutError)
{
	return RefreshBeforeCommand(OutError)
		&& SessionModel.ContinueExecution(OutError);
}

bool FAvidScriptEditorDebugTargetController::StepInto(FString& OutError)
{
	return RefreshBeforeCommand(OutError)
		&& SessionModel.StepInto(OutError);
}

void FAvidScriptEditorDebugTargetController::DiscoverComponentTargets(
	TArray<FAvidScriptEditorDebugTarget>& OutTargets)
{
	check(IsInGameThread());
	OutTargets.Reset();
	for (TObjectIterator<UAvidScriptComponent> It; It; ++It)
	{
		UAvidScriptComponent* Component = *It;
		UWorld* World = Component != nullptr ? Component->GetWorld() : nullptr;
		if (Component == nullptr
			|| Component->IsTemplate()
			|| World == nullptr
			|| (World->WorldType != EWorldType::PIE
				&& World->WorldType != EWorldType::GamePreview))
		{
			continue;
		}

		FAvidScriptRuntimeSession* RuntimeSession =
			Component->GetRuntimeSessionForEditorDebugging();
		if (RuntimeSession == nullptr || !RuntimeSession->IsLiveLoaded())
		{
			continue;
		}

		const TWeakObjectPtr<UAvidScriptComponent> WeakComponent(Component);
		FAvidScriptEditorDebugTarget& Target = OutTargets.AddDefaulted_GetRef();
		Target.TargetId = Component->GetPathName();
		Target.DisplayName = Component->GetOwner() != nullptr
			? FString::Printf(TEXT("%s / %s"), *Component->GetOwner()->GetName(), *Component->GetName())
			: Component->GetName();
		Target.WorldName = World->GetName();
		Target.ModuleId = RuntimeSession->GetLiveModuleId();
		Target.RuntimeIdentity = RuntimeSession;
		Target.CreateRuntime = [WeakComponent, RuntimeSession]()
			-> TSharedPtr<IAvidScriptEditorDebugRuntime>
		{
			UAvidScriptComponent* LiveComponent = WeakComponent.Get();
			if (LiveComponent == nullptr
				|| LiveComponent->GetRuntimeSessionForEditorDebugging() != RuntimeSession)
			{
				return nullptr;
			}
			return MakeShared<FAvidScriptEditorRuntimeDebugAdapter>(*RuntimeSession);
		};
	}
}

bool FAvidScriptEditorDebugTargetController::BindTarget(
	const FAvidScriptEditorDebugTarget& Target,
	FString& OutError)
{
	TSharedPtr<IAvidScriptEditorDebugRuntime> NextRuntime = Target.CreateRuntime();
	if (!NextRuntime.IsValid())
	{
		OutError = TEXT("the selected debug target no longer owns its Runtime Session");
		return false;
	}
	if (!SessionModel.BindRuntime(*NextRuntime, OutError))
	{
		return false;
	}

	BoundRuntimeIdentity = Target.RuntimeIdentity;
	BoundRuntime = MoveTemp(NextRuntime);
	return true;
}

bool FAvidScriptEditorDebugTargetController::RefreshBeforeCommand(FString& OutError)
{
	check(IsInGameThread());
	if (!Tick(OutError))
	{
		return false;
	}
	if (!SessionModel.GetView().bRuntimeBound)
	{
		OutError = TEXT("no live PIE AvidScript debug target is selected");
		return false;
	}
	return true;
}

void FAvidScriptEditorDebugTargetController::InvalidateBinding()
{
	SessionModel.NotifyRuntimeDestroyed();
	BoundRuntime.Reset();
	BoundRuntimeIdentity = nullptr;
}
