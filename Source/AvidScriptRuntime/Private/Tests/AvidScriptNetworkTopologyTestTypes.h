#pragma once

#include "AvidScriptComponent.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AvidScriptNetworkTopologyTestTypes.generated.h"

UCLASS()
class AVIDSCRIPTRUNTIME_API AAvidScriptNetworkTopologyTestActor final
	: public AActor
{
	GENERATED_BODY()

public:
	AAvidScriptNetworkTopologyTestActor();

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(
		Server,
		Reliable,
		BlueprintCallable,
		Category = "AvidScript|NetworkTopology")
	void ServerSubmitValue(int32 Value);

	UFUNCTION(
		Server,
		Reliable,
		BlueprintCallable,
		Category = "AvidScript|NetworkTopology")
	void ServerConfirmRepNotify(int32 Value);

	UFUNCTION()
	void OnRep_ReplicatedScore();

	UFUNCTION(BlueprintCallable, Category = "AvidScript|NetworkTopology")
	void RecordScriptServerHandler(int32 Value);

	UFUNCTION(BlueprintCallable, Category = "AvidScript|NetworkTopology")
	void RecordScriptRepNotify(int32 Value);

	UPROPERTY(
		ReplicatedUsing = OnRep_ReplicatedScore,
		BlueprintReadWrite,
		Category = "AvidScript|NetworkTopology")
	int32 ReplicatedScore = 0;

	UAvidScriptComponent* GetScriptComponent() const
	{
		return ScriptComponent;
	}

	int32 GetNativeServerRpcCount() const { return NativeServerRpcCount; }
	int32 GetScriptServerRpcCount() const { return ScriptServerRpcCount; }
	int32 GetNativeRepNotifyCount() const { return NativeRepNotifyCount; }
	int32 GetScriptRepNotifyCount() const { return ScriptRepNotifyCount; }
	int32 GetClientAckCount() const { return ClientAckCount; }
	int32 GetLastNativeServerValue() const { return LastNativeServerValue; }
	int32 GetLastScriptServerValue() const { return LastScriptServerValue; }
	int32 GetLastNativeRepNotifyValue() const
	{
		return LastNativeRepNotifyValue;
	}
	int32 GetLastScriptRepNotifyValue() const
	{
		return LastScriptRepNotifyValue;
	}
	int32 GetLastAckValue() const { return LastAckValue; }

private:
	UPROPERTY(VisibleAnywhere, Category = "AvidScript|NetworkTopology")
	TObjectPtr<UAvidScriptComponent> ScriptComponent;

	int32 NativeServerRpcCount = 0;
	int32 ScriptServerRpcCount = 0;
	int32 NativeRepNotifyCount = 0;
	int32 ScriptRepNotifyCount = 0;
	int32 ClientAckCount = 0;
	int32 LastNativeServerValue = 0;
	int32 LastScriptServerValue = 0;
	int32 LastNativeRepNotifyValue = 0;
	int32 LastScriptRepNotifyValue = 0;
	int32 LastAckValue = 0;
};
