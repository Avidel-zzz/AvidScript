#pragma once

#include "CoreMinimal.h"
#include "Engine/LatentActionManager.h"
#include "GameFramework/Actor.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Net/UnrealNetwork.h"
#include "UObject/Interface.h"
#include "UObject/Object.h"

#include "AvidScriptEditorCSharpBindingEmitterTestTypes.generated.h"

class UMaterialInterface;
class UTexture;

UENUM()
enum class EAvidScriptCSharpEmitterTestMode : uint8
{
	Primary,
	Secondary
};

UINTERFACE(BlueprintType)
class UAvidScriptCSharpEmitterCallableInterface : public UInterface
{
	GENERATED_BODY()
};

class IAvidScriptCSharpEmitterCallableInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "AvidScript")
	int32 TransformInterfaceValue(int32 Value);
};

UCLASS()
class UAvidScriptCSharpEmitterInterfaceConsumer : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, Category = "AvidScript")
	TScriptInterface<IAvidScriptCSharpEmitterCallableInterface> InterfaceValue;

	UFUNCTION(BlueprintCallable, Category = "AvidScript")
	TScriptInterface<IAvidScriptCSharpEmitterCallableInterface> RoundtripInterface(
		TScriptInterface<IAvidScriptCSharpEmitterCallableInterface> Value) const
	{
		return Value;
	}
};

USTRUCT(BlueprintType)
struct FAvidScriptStructWireNestedTestType
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	int32 Count = 0;

	UPROPERTY(BlueprintReadWrite)
	EAvidScriptCSharpEmitterTestMode Mode = EAvidScriptCSharpEmitterTestMode::Primary;

	UPROPERTY(BlueprintReadWrite)
	FVector Location = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite)
	UObject* Target = nullptr;
};

USTRUCT(BlueprintType)
struct FAvidScriptStructWireRootTestType
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	FAvidScriptStructWireNestedTestType Nested;

	UPROPERTY(BlueprintReadWrite)
	bool bEnabled = false;

	UPROPERTY(BlueprintReadWrite)
	uint8 Level = 0;

	UPROPERTY(BlueprintReadWrite)
	float Weight = 0.0f;
};

USTRUCT(BlueprintType)
struct FAvidScriptStructWireUnsafeTestType
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	FString Label;
};

USTRUCT(BlueprintType)
struct FAvidScriptStructWireObjectLeafTestType
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	UTexture* Texture = nullptr;

	UPROPERTY(BlueprintReadWrite)
	UMaterialInterface* Material = nullptr;
};

USTRUCT(BlueprintType)
struct FAvidScriptStructWireBaseTestType
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	int32 BaseCount = 0;
};

USTRUCT(BlueprintType)
struct FAvidScriptStructWireDerivedTestType : public FAvidScriptStructWireBaseTestType
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	float DerivedWeight = 0.0f;
};

USTRUCT(BlueprintType)
struct FAvidScriptStructWireFixedArrayTestType
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Values[2]{};
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FAvidScriptEditorDelegateSignal,
	AActor*, SourceActor,
	int32, Count,
	float, Scale);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FAvidScriptEditorDelegateStringSignal,
	FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_NineParams(
	FAvidScriptEditorDelegateLargeSignal,
	int32, Value0,
	int32, Value1,
	int32, Value2,
	int32, Value3,
	int32, Value4,
	int32, Value5,
	int32, Value6,
	int32, Value7,
	int32, Value8);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FAvidScriptEditorDelegateRefOutSignal,
	UPARAM(ref) int32&, Value,
	int32&, Doubled);

DECLARE_DYNAMIC_DELEGATE_RetVal_TwoParams(
	int32,
	FAvidScriptEditorSinglecastRefOutSignal,
	UPARAM(ref) int32&, Value,
	int32&, Doubled);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(
	FAvidScriptEditorAsyncActionOutcome);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FAvidScriptEditorAsyncActionPayloadCompleted,
	int32, Score,
	UObject*, Target);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FAvidScriptEditorAsyncActionPayloadFailed,
	int32, ErrorCode);

UCLASS()
class UAvidScriptEditorAsyncActionTestObject final
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvidScriptEditorAsyncActionOutcome Completed;

	UPROPERTY(BlueprintAssignable)
	FAvidScriptEditorAsyncActionOutcome Cancelled;

	UFUNCTION(
		BlueprintCallable,
		meta = (BlueprintInternalUseOnly = "true"))
	static UAvidScriptEditorAsyncActionTestObject* WaitForSignal()
	{
		return NewObject<UAvidScriptEditorAsyncActionTestObject>();
	}

	virtual void Activate() override
	{
	}
};

UCLASS()
class UAvidScriptEditorAsyncActionPayloadTestObject final
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	static TWeakObjectPtr<UAvidScriptEditorAsyncActionPayloadTestObject>
		LastCreatedForTesting;

	UPROPERTY(BlueprintAssignable)
	FAvidScriptEditorAsyncActionPayloadCompleted Completed;

	UPROPERTY(BlueprintAssignable)
	FAvidScriptEditorAsyncActionPayloadFailed Failed;

	UFUNCTION(
		BlueprintCallable,
		meta = (BlueprintInternalUseOnly = "true"))
	static UAvidScriptEditorAsyncActionPayloadTestObject* WaitForPayload()
	{
		UAvidScriptEditorAsyncActionPayloadTestObject* const Action =
			NewObject<UAvidScriptEditorAsyncActionPayloadTestObject>();
		LastCreatedForTesting = Action;
		return Action;
	}

	virtual void Activate() override
	{
		++ActivationCount;
	}

	int32 ActivationCount = 0;
};

UCLASS()
class AAvidScriptEditorDelegateEventTestActor : public AActor
{
	GENERATED_BODY()

public:
	int32 CapturedDelegateCount = 0;
	float CapturedDelegateScale = 0.0f;
	int32 SinglecastInvocationCount = 0;

	UPROPERTY(BlueprintAssignable)
	FAvidScriptEditorDelegateSignal OnScriptSignal;

	UPROPERTY(BlueprintAssignable)
	FAvidScriptEditorDelegateStringSignal OnStringSignal;

	UPROPERTY(BlueprintAssignable)
	FAvidScriptEditorDelegateLargeSignal OnLargeSignal;

	UPROPERTY(BlueprintAssignable)
	FAvidScriptEditorDelegateRefOutSignal OnRefOutSignal;

	UPROPERTY()
	FAvidScriptEditorSinglecastRefOutSignal OnSinglecastSignal;

	UFUNCTION()
	void CaptureScriptSignal(
		AActor* SourceActor,
		int32 Count,
		float Scale)
	{
		CapturedDelegateCount = Count;
		CapturedDelegateScale = Scale;
	}

	UFUNCTION()
	int32 CaptureSinglecastSignal(
		UPARAM(ref) int32& Value,
		int32& Doubled)
	{
		++SinglecastInvocationCount;
		Value += 3;
		Doubled = Value * 2;
		return Value + 10;
	}
};

UCLASS()
class UAvidScriptCSharpBindingEmitterTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite)
	FName ReadableFName = NAME_None;

	UPROPERTY(BlueprintReadWrite)
	FString ReadableFString;

	UPROPERTY(BlueprintReadWrite)
	FText ReadableFText;

	UPROPERTY(BlueprintReadWrite)
	TSoftObjectPtr<UObject> ReadableSoftObject;

	UPROPERTY(BlueprintReadWrite)
	TWeakObjectPtr<UObject> ReadableWeakObject;

	UPROPERTY(BlueprintReadWrite)
	TArray<FString> ReadableStringArray;

	UPROPERTY(BlueprintReadWrite)
	TSet<int32> ReadableIntSet;

	UPROPERTY(BlueprintReadWrite)
	TMap<FName, FString> ReadableNameStringMap;

	UPROPERTY(BlueprintReadWrite)
	TArray<TObjectPtr<UObject>> ReadableStrongObjectArray;

	UPROPERTY(BlueprintReadWrite)
	TMap<FName, TObjectPtr<UObject>> ReadableStrongObjectMap;

	UFUNCTION(BlueprintPure)
	int32 ReservedHandleNames(int32 Slot, int32 Generation) const
	{
		return Slot + Generation;
	}

	UFUNCTION(BlueprintPure)
	FVector GeneratedVectorValue(const FVector& Value) const
	{
		return Value;
	}

	UFUNCTION(BlueprintCallable, meta = (CPP_Default_bEnabled = "false", CPP_Default_Mode = "Primary"))
	void OptionalProjection(bool bEnabled, EAvidScriptCSharpEmitterTestMode Mode)
	{
	}

	UFUNCTION(BlueprintCallable, meta = (CPP_Default_Count = "1junk"))
	void InvalidScalarDefault(int32 Count)
	{
	}

	UFUNCTION(BlueprintCallable, meta = (CPP_Default_Count = "1.5"))
	void FractionalIntegerDefault(int32 Count)
	{
	}

	UFUNCTION(BlueprintCallable, meta = (CPP_Default_Ratio = "1.0junk"))
	void InvalidFloatDefault(float Ratio)
	{
	}

	UFUNCTION(BlueprintPure)
	bool IsValid() const
	{
		return true;
	}

	UFUNCTION(BlueprintPure)
	static FTransform StaticTransform(FTransform Input)
	{
		return Input;
	}

	UFUNCTION(BlueprintPure)
	FName ReturnFName() const
	{
		return NAME_None;
	}

	UFUNCTION(BlueprintCallable)
	void OutFName(FName& OutName)
	{
		OutName = NAME_None;
	}

	UFUNCTION(BlueprintCallable)
	void RefFName(UPARAM(ref) FName& InOutName)
	{
		InOutName = NAME_None;
	}

	UFUNCTION(BlueprintCallable)
	void ConstRefFName(const FName& InName)
	{
	}

	UFUNCTION(BlueprintPure)
	FString ReturnFString() const
	{
		return TEXT("Avid");
	}

	UFUNCTION(BlueprintCallable)
	void OutFString(FString& OutString)
	{
		OutString = TEXT("Avid");
	}

	UFUNCTION(BlueprintCallable)
	void RefFString(UPARAM(ref) FString& InOutString)
	{
		InOutString += TEXT("Avid");
	}

	UFUNCTION(BlueprintCallable)
	void ConstRefFString(const FString& InString)
	{
	}

	UFUNCTION(BlueprintPure, meta = (CPP_Default_Value = "Avid"))
	FString FStringValueDefault(FString Value) const
	{
		return Value;
	}

	UFUNCTION(BlueprintCallable)
	TArray<int32> IntArrayRoundTrip(
		const TArray<int32>& Input,
		UPARAM(ref) TArray<int32>& InOut,
		TArray<int32>& OutValue) const
	{
		OutValue = Input;
		InOut.Append(Input);
		return InOut;
	}

	UFUNCTION(BlueprintPure)
	FAvidScriptStructWireRootTestType StructWireRoundTrip(
		FAvidScriptStructWireRootTestType Value) const
	{
		return Value;
	}

	UFUNCTION(BlueprintPure)
	FAvidScriptStructWireUnsafeTestType StructWireUnsafe(
		FAvidScriptStructWireUnsafeTestType Value) const
	{
		return Value;
	}

	UFUNCTION(BlueprintPure)
	FAvidScriptStructWireObjectLeafTestType StructWireObjectLeaves(
		FAvidScriptStructWireObjectLeafTestType Value) const
	{
		return Value;
	}

	UFUNCTION(BlueprintPure)
	FAvidScriptStructWireDerivedTestType StructWireInheritedRoundTrip(
		FAvidScriptStructWireDerivedTestType Value) const
	{
		return Value;
	}

	UFUNCTION(BlueprintPure)
	FAvidScriptStructWireFixedArrayTestType StructWireFixedArray(
		FAvidScriptStructWireFixedArrayTestType Value) const
	{
		return Value;
	}
};

UCLASS()
class AAvidScriptCSharpNameStringTestActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite)
	FName ReadableFName = NAME_None;

	UPROPERTY(BlueprintReadWrite)
	FString ReadableFString;

	UPROPERTY(BlueprintReadWrite)
	TArray<int32> ReadableIntArray;

	UFUNCTION(BlueprintPure)
	FName ReturnFName() const
	{
		return NAME_None;
	}

	UFUNCTION(BlueprintCallable)
	void OutFName(FName& OutName)
	{
		OutName = NAME_None;
	}

	UFUNCTION(BlueprintCallable)
	void RefFName(UPARAM(ref) FName& InOutName)
	{
		InOutName = NAME_None;
	}

	UFUNCTION(BlueprintCallable)
	void ConstRefFName(const FName& InName)
	{
	}

	UFUNCTION(BlueprintPure)
	FString ReturnFString() const
	{
		return TEXT("Avid");
	}

	UFUNCTION(BlueprintCallable)
	void OutFString(FString& OutString)
	{
		OutString = TEXT("Avid");
	}

	UFUNCTION(BlueprintCallable)
	void RefFString(UPARAM(ref) FString& InOutString)
	{
		InOutString += TEXT("Avid");
	}

	UFUNCTION(BlueprintCallable)
	void ConstRefFString(const FString& InString)
	{
	}

	UFUNCTION(BlueprintCallable)
	TArray<int32> IntArrayRoundTrip(
		const TArray<int32>& Input,
		UPARAM(ref) TArray<int32>& InOut,
		TArray<int32>& OutValue) const
	{
		OutValue = Input;
		InOut.Append(Input);
		return InOut;
	}
};

UCLASS()
class UAvidScriptCSharpBindingEmitterStaticOwnerTestObject : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure)
	static bool HasValue(UAvidScriptCSharpBindingEmitterStaticOwnerTestObject* Value)
	{
		return Value != nullptr;
	}
};

UCLASS()
class UAvidScriptEditorLatentFunctionLibraryTestObject : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(
		BlueprintCallable,
		Category = "AvidScript|Tests",
		meta = (Latent, LatentInfo = "LatentInfo", WorldContext = "WorldContextObject"))
	static void WaitForFlag(
		UObject* WorldContextObject,
		bool bExpected,
		FLatentActionInfo LatentInfo)
	{
		(void)WorldContextObject;
		(void)bExpected;
		(void)LatentInfo;
	}

	UFUNCTION(
		BlueprintCallable,
		Category = "AvidScript|Tests",
		meta = (Latent, LatentInfo = "LatentInfo", WorldContext = "WorldContextObject"))
	static void WaitForScore(
		UObject* WorldContextObject,
		int32 Score,
		FLatentActionInfo LatentInfo)
	{
		(void)WorldContextObject;
		(void)Score;
		(void)LatentInfo;
	}

	UFUNCTION(
		BlueprintCallable,
		Category = "AvidScript|Tests",
		meta = (
			Latent,
			LatentInfo = "LatentInfo",
			WorldContext = "WorldContextObject",
			CPP_Default_Mode = "Primary"))
	static void WaitForMode(
		UObject* WorldContextObject,
		EAvidScriptCSharpEmitterTestMode Mode,
		FLatentActionInfo LatentInfo)
	{
		(void)WorldContextObject;
		(void)Mode;
		(void)LatentInfo;
	}

	UFUNCTION(
		BlueprintCallable,
		Category = "AvidScript|Tests",
		meta = (Latent, LatentInfo = "LatentInfo", WorldContext = "WorldContextObject"))
	static void WaitForTarget(
		UObject* WorldContextObject,
		UObject* Target,
		FLatentActionInfo LatentInfo)
	{
		(void)WorldContextObject;
		(void)Target;
		(void)LatentInfo;
	}

	UFUNCTION(
		BlueprintCallable,
		Category = "AvidScript|Tests",
		meta = (Latent, LatentInfo = "LatentInfo", WorldContext = "WorldContextObject"))
	static void WaitForLocation(
		UObject* WorldContextObject,
		FVector Location,
		FLatentActionInfo LatentInfo)
	{
		(void)WorldContextObject;
		(void)Location;
		(void)LatentInfo;
	}

	UFUNCTION(
		BlueprintCallable,
		Category = "AvidScript|Tests",
		meta = (Latent, LatentInfo = "LatentInfo", WorldContext = "WorldContextObject"))
	static void WaitForSettings(
		UObject* WorldContextObject,
		FAvidScriptStructWireRootTestType Settings,
		FLatentActionInfo LatentInfo)
	{
		(void)WorldContextObject;
		(void)Settings;
		(void)LatentInfo;
	}
};

UCLASS()
class UAvidScriptEditorLatentCallbackTestObject : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void OnLatentCompleted(int32 Linkage)
	{
		LastLinkage = Linkage;
		++CompletionCount;
	}

	int32 CompletionCount = 0;
	int32 LastLinkage = INDEX_NONE;
};

UCLASS()
class AAvidScriptBindingRuntimeProcessEventTestActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, Category = "AvidScript")
	int32 GeneratedPublicInt = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript")
	float GeneratedPublicFloat = 0.0f;

	UFUNCTION(BlueprintSetter)
	void SetGeneratedSetterInt(int32 Value)
	{
		GeneratedSetterInt = Value;
	}

	UPROPERTY(
		BlueprintReadWrite,
		BlueprintSetter = SetGeneratedSetterInt,
		Category = "AvidScript")
	int32 GeneratedSetterInt = 0;

	UFUNCTION(BlueprintSetter)
	void SetRoutedValue(float Value)
	{
		++BlueprintSetterCallCount;
		RoutedValue = Value + 1.0f;
	}

	UFUNCTION(BlueprintCallable)
	void SetAlternateRoutedValue(float Value)
	{
		RoutedValue = Value + 2.0f;
	}

	UFUNCTION(BlueprintCallable)
	void RecordBlueprintDeclaredCall()
	{
		++BlueprintDeclaredCallCount;
	}

	virtual void ProcessEvent(UFunction* Function, void* Parameters) override
	{
		++ProcessEventCallCount;
		Super::ProcessEvent(Function, Parameters);
	}

	UPROPERTY(BlueprintReadWrite, BlueprintSetter = SetRoutedValue, Category = "AvidScript")
	float RoutedValue = 0.0f;

	int32 ProcessEventCallCount = 0;
	int32 BlueprintSetterCallCount = 0;
	int32 BlueprintDeclaredCallCount = 0;

private:
	UPROPERTY(
		BlueprintReadOnly,
		Category = "AvidScript",
		meta = (AllowPrivateAccess = "true"))
	int32 GeneratedPrivateInt = 0;
};

UCLASS()
class AAvidScriptBindingRuntimeNetworkTestActor : public AActor
{
	GENERATED_BODY()

public:
	AAvidScriptBindingRuntimeNetworkTestActor()
	{
		bReplicates = true;
	}

	UFUNCTION(Server, Reliable, BlueprintCallable, Category = "AvidScript")
	void ServerSubmitValue(int32 Value);

	UFUNCTION(Client, Unreliable, BlueprintCallable, Category = "AvidScript")
	void ClientApplyValue(int32 Value);

	UFUNCTION(NetMulticast, Reliable, BlueprintCallable, Category = "AvidScript")
	void MulticastAnnounceValue(int32 Value);

	UPROPERTY(
		ReplicatedUsing = OnRep_ReplicatedScore,
		BlueprintReadWrite,
		Category = "AvidScript")
	int32 ReplicatedScore = 0;

	UFUNCTION(BlueprintSetter)
	void SetReplicatedRoutedValue(int32 Value)
	{
		++ReplicatedSetterCallCount;
		ReplicatedRoutedValue = Value + 1;
	}

	UPROPERTY(
		Replicated,
		BlueprintReadWrite,
		BlueprintSetter = SetReplicatedRoutedValue,
		Category = "AvidScript")
	int32 ReplicatedRoutedValue = 0;

	UPROPERTY(
		ReplicatedUsing = OnRep_BlueprintScore,
		BlueprintReadWrite,
		Category = "AvidScript")
	int32 BlueprintScore = 0;

	UFUNCTION(BlueprintNativeEvent)
	void OnRep_BlueprintScore();

	UFUNCTION()
	void OnRep_ReplicatedScore()
	{
		++RepNotifyCallCount;
	}

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override
	{
		Super::GetLifetimeReplicatedProps(OutLifetimeProps);
		DOREPLIFETIME(
			AAvidScriptBindingRuntimeNetworkTestActor,
			ReplicatedScore);
		DOREPLIFETIME(
			AAvidScriptBindingRuntimeNetworkTestActor,
			ReplicatedRoutedValue);
		DOREPLIFETIME(
			AAvidScriptBindingRuntimeNetworkTestActor,
			BlueprintScore);
	}

	virtual void ProcessEvent(UFunction* Function, void* Parameters) override
	{
		++ProcessEventCallCount;
		Super::ProcessEvent(Function, Parameters);
	}

	int32 LastServerValue = 0;
	int32 LastClientValue = 0;
	int32 LastMulticastValue = 0;
	int32 ServerInvocationCount = 0;
	int32 ClientInvocationCount = 0;
	int32 MulticastInvocationCount = 0;
	int32 RepNotifyCallCount = 0;
	int32 ReplicatedSetterCallCount = 0;
	int32 BlueprintRepNotifyCallCount = 0;
	int32 ProcessEventCallCount = 0;
};

inline void AAvidScriptBindingRuntimeNetworkTestActor::OnRep_BlueprintScore_Implementation()
{
	++BlueprintRepNotifyCallCount;
}

inline void AAvidScriptBindingRuntimeNetworkTestActor::ServerSubmitValue_Implementation(
	const int32 Value)
{
	LastServerValue = Value;
	++ServerInvocationCount;
}

inline void AAvidScriptBindingRuntimeNetworkTestActor::ClientApplyValue_Implementation(
	const int32 Value)
{
	LastClientValue = Value;
	++ClientInvocationCount;
}

inline void AAvidScriptBindingRuntimeNetworkTestActor::MulticastAnnounceValue_Implementation(
	const int32 Value)
{
	LastMulticastValue = Value;
	++MulticastInvocationCount;
}
