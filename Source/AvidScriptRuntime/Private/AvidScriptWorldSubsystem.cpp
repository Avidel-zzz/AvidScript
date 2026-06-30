#include "AvidScriptWorldSubsystem.h"

#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWorldSubsystem, Log, All);

bool UAvidScriptWorldSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UAvidScriptWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	RuntimeStats = FAvidScriptWorldRuntimeStats();
	Runtime = MakeUnique<FAvidScriptWasmRuntimeInstance>();

	FAvidScriptWasmSmokeResult Result;
	if (!Runtime->LoadEmbeddedSmokeModule(Result) || !Runtime->BeginPlay(Result))
	{
		RecordFailure(Result);
		ReleaseRuntime();
		return;
	}

	bWorldPlayActive = true;
	RuntimeStats.bRuntimeLoaded = Runtime->IsLoaded();
	RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
	RuntimeStats.TickCallCount = Runtime->GetTickCallCount();

	UE_LOG(LogAvidScriptWorldSubsystem, Log, TEXT("AvidScript runtime started for world '%s'."), *InWorld.GetName());
}

void UAvidScriptWorldSubsystem::OnWorldEndPlay(UWorld& InWorld)
{
	if (bWorldPlayActive || Runtime.IsValid())
	{
		UE_LOG(LogAvidScriptWorldSubsystem, Log, TEXT("AvidScript runtime stopping for world '%s'."), *InWorld.GetName());
	}

	ReleaseRuntime();
	RuntimeStats.bEndPlayCalled = true;
}

void UAvidScriptWorldSubsystem::Tick(float DeltaTime)
{
	if (bWorldPlayActive && Runtime.IsValid())
	{
		FAvidScriptWasmSmokeResult Result;
		if (Runtime->Tick(DeltaTime, Result))
		{
			RuntimeStats.bRuntimeLoaded = Runtime->IsLoaded();
			RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
			RuntimeStats.TickCallCount = Runtime->GetTickCallCount();
		}
		else
		{
			RecordFailure(Result);
			ReleaseRuntime();
		}
	}

	Super::Tick(DeltaTime);
}

bool UAvidScriptWorldSubsystem::IsTickable() const
{
	return bWorldPlayActive && Runtime.IsValid() && Runtime->IsLoaded();
}

TStatId UAvidScriptWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UAvidScriptWorldSubsystem, STATGROUP_Tickables);
}

void UAvidScriptWorldSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		OnWorldEndPlay(*World);
	}
	else
	{
		ReleaseRuntime();
	}

	Super::Deinitialize();
}

void UAvidScriptWorldSubsystem::RecordFailure(const FAvidScriptWasmSmokeResult& Result)
{
	RuntimeStats.LastErrorMessage = Result.ErrorMessage;
	RuntimeStats.bRuntimeLoaded = Runtime.IsValid() && Runtime->IsLoaded();
	RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
	RuntimeStats.TickCallCount = Runtime.IsValid() ? Runtime->GetTickCallCount() : 0;

	UE_LOG(LogAvidScriptWorldSubsystem, Warning, TEXT("%s"), *Result.ErrorMessage);
}

void UAvidScriptWorldSubsystem::ReleaseRuntime()
{
	if (Runtime.IsValid())
	{
		Runtime->Unload();
		Runtime.Reset();
	}

	bWorldPlayActive = false;
	RuntimeStats.bRuntimeLoaded = false;
}
