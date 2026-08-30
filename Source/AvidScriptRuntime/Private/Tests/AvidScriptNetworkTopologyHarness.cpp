#include "Tests/AvidScriptNetworkTopologyHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/AvidScriptNetworkTopologyTestTypes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptNetworkTopology, Log, All);

namespace
{
constexpr int32 ExpectedReplicatedValue = 41;

struct FAvidScriptNetworkTopologyState
{
	FString Topology;
	FString Role;
	FString ResultPath;
	int32 ClientId = 0;
	int32 ExpectedClients = 0;
	double TimeoutSeconds = 45.0;
	double StartedAtSeconds = 0.0;
	TWeakObjectPtr<UWorld> World;
	FDelegateHandle WorldInitializedHandle;
	FDelegateHandle PostLoginHandle;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bEnabled = false;
	bool bResultWritten = false;
};

FAvidScriptNetworkTopologyState& GetState()
{
	static FAvidScriptNetworkTopologyState State;
	return State;
}

const TCHAR* LexNetMode(const ENetMode NetMode)
{
	switch (NetMode)
	{
	case NM_Standalone:
		return TEXT("standalone");
	case NM_DedicatedServer:
		return TEXT("dedicated_server");
	case NM_ListenServer:
		return TEXT("listen_server");
	case NM_Client:
		return TEXT("client");
	default:
		return TEXT("unknown");
	}
}

TSharedRef<FJsonObject> MakeActorSnapshot(
	const AAvidScriptNetworkTopologyTestActor& Actor)
{
	const UAvidScriptComponent* const Component = Actor.GetScriptComponent();
	const FAvidScriptComponentRuntimeStats* const Stats = Component != nullptr
		? &Component->GetRuntimeStats()
		: nullptr;
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("object"), Actor.GetPathName());
	Json->SetStringField(
		TEXT("owner"),
		Actor.GetOwner() == nullptr
			? FString()
			: Actor.GetOwner()->GetPathName());
	Json->SetNumberField(TEXT("replicated_score"), Actor.ReplicatedScore);
	Json->SetNumberField(
		TEXT("native_server_rpc_count"),
		Actor.GetNativeServerRpcCount());
	Json->SetNumberField(
		TEXT("script_server_rpc_count"),
		Actor.GetScriptServerRpcCount());
	Json->SetNumberField(
		TEXT("native_rep_notify_count"),
		Actor.GetNativeRepNotifyCount());
	Json->SetNumberField(
		TEXT("script_rep_notify_count"),
		Actor.GetScriptRepNotifyCount());
	Json->SetNumberField(TEXT("client_ack_count"), Actor.GetClientAckCount());
	Json->SetNumberField(
		TEXT("last_native_server_value"),
		Actor.GetLastNativeServerValue());
	Json->SetNumberField(
		TEXT("last_script_server_value"),
		Actor.GetLastScriptServerValue());
	Json->SetNumberField(
		TEXT("last_native_rep_notify_value"),
		Actor.GetLastNativeRepNotifyValue());
	Json->SetNumberField(
		TEXT("last_script_rep_notify_value"),
		Actor.GetLastScriptRepNotifyValue());
	Json->SetNumberField(TEXT("last_ack_value"), Actor.GetLastAckValue());
	Json->SetBoolField(
		TEXT("runtime_loaded"),
		Stats != nullptr && Stats->bRuntimeLoaded);
	Json->SetBoolField(
		TEXT("begin_play_called"),
		Stats != nullptr && Stats->bBeginPlayCalled);
	Json->SetStringField(
		TEXT("runtime_error"),
		Stats == nullptr ? FString() : Stats->LastErrorMessage);
	return Json;
}

TArray<AAvidScriptNetworkTopologyTestActor*> GetActors(UWorld& World)
{
	TArray<AAvidScriptNetworkTopologyTestActor*> Actors;
	for (TActorIterator<AAvidScriptNetworkTopologyTestActor> It(&World); It; ++It)
	{
		Actors.Add(*It);
	}
	return Actors;
}

bool IsServerActorComplete(
	const AAvidScriptNetworkTopologyTestActor& Actor)
{
	const UAvidScriptComponent* const Component = Actor.GetScriptComponent();
	if (Component == nullptr)
	{
		return false;
	}
	const FAvidScriptComponentRuntimeStats& Stats = Component->GetRuntimeStats();
	return Actor.ReplicatedScore == ExpectedReplicatedValue
		&& Actor.GetNativeServerRpcCount() == 1
		&& Actor.GetScriptServerRpcCount() == 1
		&& Actor.GetClientAckCount() == 1
		&& Actor.GetLastNativeServerValue() == ExpectedReplicatedValue
		&& Actor.GetLastScriptServerValue() == ExpectedReplicatedValue
		&& Actor.GetLastAckValue() == ExpectedReplicatedValue
		&& Stats.bRuntimeLoaded
		&& Stats.bBeginPlayCalled;
}

bool IsClientActorComplete(
	const AAvidScriptNetworkTopologyTestActor& Actor)
{
	const UAvidScriptComponent* const Component = Actor.GetScriptComponent();
	if (Component == nullptr)
	{
		return false;
	}
	const FAvidScriptComponentRuntimeStats& Stats = Component->GetRuntimeStats();
	return Actor.ReplicatedScore == ExpectedReplicatedValue
		&& Actor.GetNativeServerRpcCount() == 0
		&& Actor.GetScriptServerRpcCount() == 0
		&& Actor.GetNativeRepNotifyCount() == 1
		&& Actor.GetScriptRepNotifyCount() == 1
		&& Actor.GetClientAckCount() == 0
		&& Actor.GetLastNativeRepNotifyValue() == ExpectedReplicatedValue
		&& Actor.GetLastScriptRepNotifyValue() == ExpectedReplicatedValue
		&& Stats.bRuntimeLoaded
		&& Stats.bBeginPlayCalled;
}

bool WriteResult(
	const bool bSucceeded,
	const FString& Category,
	const FString& Details)
{
	FAvidScriptNetworkTopologyState& State = GetState();
	if (State.bResultWritten)
	{
		return true;
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("topology"), State.Topology);
	Root->SetStringField(TEXT("role"), State.Role);
	Root->SetNumberField(TEXT("client_id"), State.ClientId);
	Root->SetNumberField(TEXT("expected_clients"), State.ExpectedClients);
	Root->SetNumberField(
		TEXT("process_id"),
		static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	Root->SetBoolField(TEXT("succeeded"), bSucceeded);
	Root->SetStringField(TEXT("error_category"), Category);
	Root->SetStringField(TEXT("details"), Details);
	UWorld* const World = State.World.Get();
	Root->SetStringField(
		TEXT("net_mode"),
		World == nullptr ? TEXT("unavailable") : LexNetMode(World->GetNetMode()));
	TArray<TSharedPtr<FJsonValue>> ActorValues;
	if (World != nullptr)
	{
		for (const AAvidScriptNetworkTopologyTestActor* Actor : GetActors(*World))
		{
			ActorValues.Add(MakeShared<FJsonValueObject>(MakeActorSnapshot(*Actor)));
		}
	}
	Root->SetArrayField(TEXT("actors"), MoveTemp(ActorValues));

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		return false;
	}
	const FString Directory = FPaths::GetPath(State.ResultPath);
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString TemporaryPath = State.ResultPath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Json, *TemporaryPath)
		|| !IFileManager::Get().Move(
			*State.ResultPath,
			*TemporaryPath,
			true,
			true,
			false,
			true))
	{
		return false;
	}
	State.bResultWritten = true;
	UE_LOG(
		LogAvidScriptNetworkTopology,
		Display,
		TEXT("AVID_NET_TOPOLOGY_RESULT topology=%s role=%s client=%d succeeded=%s path=%s"),
		*State.Topology,
		*State.Role,
		State.ClientId,
		bSucceeded ? TEXT("true") : TEXT("false"),
		*State.ResultPath);
	return true;
}

bool TickHarness(float)
{
	FAvidScriptNetworkTopologyState& State = GetState();
	if (!State.bEnabled || State.bResultWritten)
	{
		return false;
	}
	UWorld* const World = State.World.Get();
	if (World != nullptr)
	{
		const TArray<AAvidScriptNetworkTopologyTestActor*> Actors =
			GetActors(*World);
		if (State.Role == TEXT("server"))
		{
			int32 CompleteCount = 0;
			for (const AAvidScriptNetworkTopologyTestActor* Actor : Actors)
			{
				if (Actor != nullptr && IsServerActorComplete(*Actor))
				{
					++CompleteCount;
				}
			}
			if (CompleteCount == State.ExpectedClients)
			{
				WriteResult(true, FString(), TEXT("all_remote_clients_confirmed"));
				return false;
			}
		}
		else if (State.Role == TEXT("client"))
		{
			const bool bComplete = Actors.ContainsByPredicate(
					[](const AAvidScriptNetworkTopologyTestActor* Actor)
					{
						return Actor != nullptr && IsClientActorComplete(*Actor);
					});
			if (bComplete)
			{
				WriteResult(true, FString(), TEXT("rep_notify_and_ack_dispatched"));
				return false;
			}
		}
	}
	if (FPlatformTime::Seconds() - State.StartedAtSeconds
		>= State.TimeoutSeconds)
	{
		WriteResult(
			false,
			TEXT("network_topology_timeout"),
			TEXT("The process did not satisfy its role contract before timeout."));
		return false;
	}
	return true;
}

void HandleWorldInitialized(
	UWorld* World,
	const UWorld::InitializationValues)
{
	if (World != nullptr
		&& (World->WorldType == EWorldType::Game
			|| World->WorldType == EWorldType::PIE))
	{
		GetState().World = World;
	}
}

void HandlePostLogin(
	AGameModeBase* GameMode,
	APlayerController* NewPlayer)
{
	FAvidScriptNetworkTopologyState& State = GetState();
	if (!State.bEnabled
		|| State.Role != TEXT("server")
		|| GameMode == nullptr
		|| NewPlayer == nullptr)
	{
		return;
	}
	UWorld* const World = GameMode->GetWorld();
	if (World == nullptr || World->GetNetMode() == NM_Client)
	{
		return;
	}
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = NewPlayer;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AAvidScriptNetworkTopologyTestActor* const Actor =
		World->SpawnActor<AAvidScriptNetworkTopologyTestActor>(
			AAvidScriptNetworkTopologyTestActor::StaticClass(),
			FTransform::Identity,
			SpawnParameters);
	if (Actor != nullptr)
	{
		Actor->ForceNetUpdate();
		UE_LOG(
			LogAvidScriptNetworkTopology,
			Display,
			TEXT("AVID_NET_TOPOLOGY_ACTOR owner=%s actor=%s"),
			*NewPlayer->GetPathName(),
			*Actor->GetPathName());
	}
}
} // namespace

void FAvidScriptNetworkTopologyHarness::Startup()
{
	FAvidScriptNetworkTopologyState& State = GetState();
	if (State.bEnabled
		|| !FParse::Value(
			FCommandLine::Get(),
			TEXT("AvidScriptNetworkTopologyRole="),
			State.Role))
	{
		return;
	}
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptNetworkTopology="),
		State.Topology);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptNetworkTopologyResult="),
		State.ResultPath);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptNetworkTopologyClientId="),
		State.ClientId);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptNetworkTopologyExpectedClients="),
		State.ExpectedClients);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptNetworkTopologyTimeout="),
		State.TimeoutSeconds);
	FPaths::NormalizeFilename(State.ResultPath);
	if ((State.Role != TEXT("server") && State.Role != TEXT("client"))
		|| State.Topology.IsEmpty()
		|| State.ResultPath.IsEmpty()
		|| State.ExpectedClients < 1
		|| State.TimeoutSeconds <= 0.0)
	{
		UE_LOG(
			LogAvidScriptNetworkTopology,
			Error,
			TEXT("AVID_NET_TOPOLOGY_INVALID_COMMANDLINE"));
		return;
	}
	State.StartedAtSeconds = FPlatformTime::Seconds();
	State.bEnabled = true;
	State.WorldInitializedHandle =
		FWorldDelegates::OnPostWorldInitialization.AddStatic(
			&HandleWorldInitialized);
	State.PostLoginHandle = FGameModeEvents::OnGameModePostLoginEvent()
		.AddStatic(&HandlePostLogin);
	State.TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&TickHarness));
	UE_LOG(
		LogAvidScriptNetworkTopology,
		Display,
		TEXT("AVID_NET_TOPOLOGY_READY topology=%s role=%s client=%d expected=%d"),
		*State.Topology,
		*State.Role,
		State.ClientId,
		State.ExpectedClients);
}

void FAvidScriptNetworkTopologyHarness::Shutdown()
{
	FAvidScriptNetworkTopologyState& State = GetState();
	if (!State.bEnabled)
	{
		return;
	}
	FWorldDelegates::OnPostWorldInitialization.Remove(
		State.WorldInitializedHandle);
	FGameModeEvents::OnGameModePostLoginEvent().Remove(
		State.PostLoginHandle);
	if (State.TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(State.TickerHandle);
	}
	State = FAvidScriptNetworkTopologyState();
}

#else

void FAvidScriptNetworkTopologyHarness::Startup()
{
}

void FAvidScriptNetworkTopologyHarness::Shutdown()
{
}

#endif
