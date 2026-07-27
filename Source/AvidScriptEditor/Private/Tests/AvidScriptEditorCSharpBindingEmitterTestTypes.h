#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"

#include "AvidScriptEditorCSharpBindingEmitterTestTypes.generated.h"

UENUM()
enum class EAvidScriptCSharpEmitterTestMode : uint8
{
	Primary,
	Secondary
};

UCLASS()
class UAvidScriptCSharpBindingEmitterTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadOnly, VisibleAnywhere)
	FName ReadableFName = NAME_None;

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

	virtual void ProcessEvent(UFunction* Function, void* Parameters) override
	{
		++ProcessEventCallCount;
		Super::ProcessEvent(Function, Parameters);
	}

	UPROPERTY(BlueprintReadWrite, BlueprintSetter = SetRoutedValue, Category = "AvidScript")
	float RoutedValue = 0.0f;

	int32 ProcessEventCallCount = 0;
	int32 BlueprintSetterCallCount = 0;

private:
	UPROPERTY(
		BlueprintReadOnly,
		Category = "AvidScript",
		meta = (AllowPrivateAccess = "true"))
	int32 GeneratedPrivateInt = 0;
};
