#include "Tests/AvidScriptGeneratedNetworkTopologyHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && AVIDSCRIPT_WITH_GENERATED_TYPES

#include "AvidScriptGeneratedTypes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
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
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptGeneratedNetworkTopology, Log, All);

namespace
{
constexpr float ExpectedDamage = 41.0f;

struct FAvidScriptGeneratedNetworkTopologyState
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
	TSet<TWeakObjectPtr<AProjectile>> SubmittedClientActors;
	TSet<TWeakObjectPtr<AProjectile>> RespondedServerActors;
	bool bEnabled = false;
	bool bResultWritten = false;
};

FAvidScriptGeneratedNetworkTopologyState& GetState()
{
	static FAvidScriptGeneratedNetworkTopologyState State;
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

TArray<AProjectile*> GetActors(UWorld& World)
{
	TArray<AProjectile*> Actors;
	for (TActorIterator<AProjectile> It(&World); It; ++It)
	{
		Actors.Add(*It);
	}
	return Actors;
}

TSharedRef<FJsonObject> MakeActorSnapshot(const AProjectile& Actor)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("object"), Actor.GetPathName());
	Json->SetStringField(
		TEXT("owner"),
		Actor.GetOwner() == nullptr ? FString() : Actor.GetOwner()->GetPathName());
	Json->SetNumberField(TEXT("damage"), Actor.Damage);
	Json->SetNumberField(TEXT("damage_rep_notify_count"), Actor.DamageRepNotifyCount);
	Json->SetNumberField(TEXT("last_replicated_damage"), Actor.LastReplicatedDamage);
	Json->SetNumberField(TEXT("server_rpc_count"), Actor.ServerRpcCount);
	Json->SetNumberField(TEXT("last_server_damage"), Actor.LastServerDamage);
	Json->SetNumberField(TEXT("client_ack_count"), Actor.ClientAckCount);
	Json->SetNumberField(TEXT("last_client_ack_damage"), Actor.LastClientAckDamage);
	Json->SetNumberField(TEXT("multicast_count"), Actor.MulticastCount);
	Json->SetNumberField(TEXT("last_multicast_damage"), Actor.LastMulticastDamage);
	Json->SetBoolField(TEXT("has_authority"), Actor.HasAuthority());
	Json->SetBoolField(TEXT("has_begun_play"), Actor.HasBegunPlay);
	Json->SetBoolField(TEXT("actor_begun_play"), Actor.HasActorBegunPlay());
	Json->SetBoolField(
		TEXT("active_instance"),
		FAvidScriptGeneratedTypeRuntimeHost::Get().IsInstanceActive(Actor));
	return Json;
}

bool IsServerActorComplete(const AProjectile& Actor)
{
	return Actor.HasAuthority()
		&& Actor.HasBegunPlay
		&& Actor.HasActorBegunPlay()
		&& Actor.Damage == ExpectedDamage
		&& Actor.ServerRpcCount == 1
		&& Actor.LastServerDamage == ExpectedDamage
		&& Actor.DamageRepNotifyCount == 0
		&& Actor.ClientAckCount == 0
		&& Actor.MulticastCount == 1
		&& Actor.LastMulticastDamage == ExpectedDamage
		&& FAvidScriptGeneratedTypeRuntimeHost::Get().IsInstanceActive(Actor);
}

bool IsClientActorComplete(const AProjectile& Actor)
{
	return !Actor.HasAuthority()
		&& Actor.HasBegunPlay
		&& Actor.HasActorBegunPlay()
		&& Actor.Damage == ExpectedDamage
		&& Actor.DamageRepNotifyCount >= 1
		&& Actor.LastReplicatedDamage == ExpectedDamage
		&& Actor.ServerRpcCount == 0
		&& Actor.LastServerDamage == 0.0f
		&& Actor.ClientAckCount == 1
		&& Actor.LastClientAckDamage == ExpectedDamage
		&& Actor.MulticastCount == 1
		&& Actor.LastMulticastDamage == ExpectedDamage
		&& FAvidScriptGeneratedTypeRuntimeHost::Get().IsInstanceActive(Actor);
}

bool WriteResult(
	const bool bSucceeded,
	const FString& Category,
	const FString& Details)
{
	FAvidScriptGeneratedNetworkTopologyState& State = GetState();
	if (State.bResultWritten)
	{
		return true;
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(TEXT("contract"), TEXT("generated_types"));
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
		for (const AProjectile* Actor : GetActors(*World))
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
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(State.ResultPath), true);
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
		LogAvidScriptGeneratedNetworkTopology,
		Display,
		TEXT("AVID_GENERATED_NET_RESULT topology=%s role=%s client=%d succeeded=%s path=%s"),
		*State.Topology,
		*State.Role,
		State.ClientId,
		bSucceeded ? TEXT("true") : TEXT("false"),
		*State.ResultPath);
	return true;
}

void DriveClientActor(AProjectile& Actor)
{
	FAvidScriptGeneratedNetworkTopologyState& State = GetState();
	const TWeakObjectPtr<AProjectile> Key(&Actor);
	if (!Actor.HasBegunPlay
		|| !Actor.HasActorBegunPlay()
		|| !FAvidScriptGeneratedTypeRuntimeHost::Get().IsInstanceActive(Actor)
		|| State.SubmittedClientActors.Contains(Key))
	{
		return;
	}
	State.SubmittedClientActors.Add(Key);
	Actor.ServerSubmitDamage(ExpectedDamage);
}

void DriveServerActor(AProjectile& Actor)
{
	FAvidScriptGeneratedNetworkTopologyState& State = GetState();
	const TWeakObjectPtr<AProjectile> Key(&Actor);
	if (Actor.ServerRpcCount != 1
		|| Actor.LastServerDamage != ExpectedDamage
		|| State.RespondedServerActors.Contains(Key))
	{
		return;
	}
	State.RespondedServerActors.Add(Key);
	Actor.ClientConfirmDamage(ExpectedDamage);
	Actor.MulticastObserveDamage(ExpectedDamage);
	Actor.ForceNetUpdate();
}

bool TickHarness(float)
{
	FAvidScriptGeneratedNetworkTopologyState& State = GetState();
	if (!State.bEnabled || State.bResultWritten)
	{
		return false;
	}
	UWorld* const World = State.World.Get();
	if (World != nullptr)
	{
		const TArray<AProjectile*> Actors = GetActors(*World);
		if (State.Role == TEXT("server"))
		{
			for (AProjectile* Actor : Actors)
			{
				if (Actor != nullptr)
				{
					DriveServerActor(*Actor);
				}
			}
			int32 CompleteCount = 0;
			for (const AProjectile* Actor : Actors)
			{
				if (Actor != nullptr && IsServerActorComplete(*Actor))
				{
					++CompleteCount;
				}
			}
			if (CompleteCount == State.ExpectedClients)
			{
				WriteResult(true, FString(), TEXT("all_generated_type_clients_completed"));
				return false;
			}
		}
		else
		{
			for (AProjectile* Actor : Actors)
			{
				if (Actor != nullptr)
				{
					DriveClientActor(*Actor);
				}
			}
			if (Actors.ContainsByPredicate(
					[](const AProjectile* Actor)
					{
						return Actor != nullptr && IsClientActorComplete(*Actor);
					}))
			{
				WriteResult(true, FString(), TEXT("generated_type_rpc_replication_completed"));
				return false;
			}
		}
	}
	if (FPlatformTime::Seconds() - State.StartedAtSeconds >= State.TimeoutSeconds)
	{
		WriteResult(
			false,
			TEXT("generated_network_topology_timeout"),
			TEXT("The process did not satisfy its generated type role contract before timeout."));
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
	FAvidScriptGeneratedNetworkTopologyState& State = GetState();
	if (!State.bEnabled
		|| State.Role != TEXT("server")
		|| GameMode == nullptr
		|| NewPlayer == nullptr
		|| NewPlayer->GetNetConnection() == nullptr)
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
	AProjectile* const Actor = World->SpawnActor<AProjectile>(
		AProjectile::StaticClass(),
		FTransform::Identity,
		SpawnParameters);
	if (Actor != nullptr)
	{
		Actor->bOnlyRelevantToOwner = true;
		Actor->SetNetUpdateFrequency(30.0f);
		Actor->SetMinNetUpdateFrequency(10.0f);
		Actor->ForceNetUpdate();
		UE_LOG(
			LogAvidScriptGeneratedNetworkTopology,
			Display,
			TEXT("AVID_GENERATED_NET_ACTOR owner=%s actor=%s"),
			*NewPlayer->GetPathName(),
			*Actor->GetPathName());
	}
}
} // namespace

void FAvidScriptGeneratedNetworkTopologyHarness::Startup()
{
	FAvidScriptGeneratedNetworkTopologyState& State = GetState();
	if (State.bEnabled
		|| !FParse::Value(
			FCommandLine::Get(),
			TEXT("AvidScriptGeneratedNetworkTopologyRole="),
			State.Role))
	{
		return;
	}
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptGeneratedNetworkTopology="),
		State.Topology);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptGeneratedNetworkTopologyResult="),
		State.ResultPath);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptGeneratedNetworkTopologyClientId="),
		State.ClientId);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptGeneratedNetworkTopologyExpectedClients="),
		State.ExpectedClients);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptGeneratedNetworkTopologyTimeout="),
		State.TimeoutSeconds);
	FPaths::NormalizeFilename(State.ResultPath);
	if ((State.Role != TEXT("server") && State.Role != TEXT("client"))
		|| State.Topology.IsEmpty()
		|| State.ResultPath.IsEmpty()
		|| State.ExpectedClients < 1
		|| State.TimeoutSeconds <= 0.0)
	{
		UE_LOG(
			LogAvidScriptGeneratedNetworkTopology,
			Error,
			TEXT("AVID_GENERATED_NET_INVALID_COMMANDLINE"));
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
		LogAvidScriptGeneratedNetworkTopology,
		Display,
		TEXT("AVID_GENERATED_NET_READY topology=%s role=%s client=%d expected=%d"),
		*State.Topology,
		*State.Role,
		State.ClientId,
		State.ExpectedClients);
}

void FAvidScriptGeneratedNetworkTopologyHarness::Shutdown()
{
	FAvidScriptGeneratedNetworkTopologyState& State = GetState();
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
	State = FAvidScriptGeneratedNetworkTopologyState();
}

#else

void FAvidScriptGeneratedNetworkTopologyHarness::Startup()
{
}

void FAvidScriptGeneratedNetworkTopologyHarness::Shutdown()
{
}

#endif
