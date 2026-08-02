#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AvidScriptBindingsTestTypes.generated.h"

UENUM(BlueprintType)
enum class EAvidScriptBindingsStructMode : uint8
{
	Primary,
	Secondary
};

USTRUCT(BlueprintType)
struct FAvidScriptBindingsNestedStruct
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	int32 Count = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	float Ratio = 0.0f;
};

USTRUCT(BlueprintType)
struct FAvidScriptBindingsRecursiveStruct
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	bool bEnabled = false;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	EAvidScriptBindingsStructMode Mode =
		EAvidScriptBindingsStructMode::Primary;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	TObjectPtr<UObject> Target;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	FAvidScriptBindingsNestedStruct Nested;
};

UCLASS()
class UAvidScriptBindingsTestObject : public UObject
{
    GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	int32 FastPathInt32Property = 0;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	int32 FastPathAddInt32(int32 Left, int32 Right) const;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	int32 FastPathMaxInt32(int32 Left, int32 Right) const;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	float ReflectionFallbackAddFloat(float Left, float Right) const;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	FVector FastPathVectorValue(const FVector& Value) const;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	UObject* FastPathObjectRoundtrip(UObject* Value) const;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	FAvidScriptBindingsRecursiveStruct RecursiveStructProperty;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	FName Utf8NameProperty;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	FString Utf8StringProperty;

	UPROPERTY()
	int32 Utf8InvocationCount = 0;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Tests")
	FAvidScriptBindingsRecursiveStruct RecursiveStructRoundtrip(
		const FAvidScriptBindingsRecursiveStruct& Input,
		UPARAM(ref) FAvidScriptBindingsRecursiveStruct& InOut,
		FAvidScriptBindingsRecursiveStruct& OutValue) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Tests")
	FString Utf8Roundtrip(
		const FName& InputName,
		const FString& InputString,
		UPARAM(ref) FName& InOutName,
		UPARAM(ref) FString& InOutString,
		FName& OutName);
};

UCLASS()
class UAvidScriptBindingsDerivedTestObject final
	: public UAvidScriptBindingsTestObject
{
	GENERATED_BODY()
};
