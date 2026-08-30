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
struct FAvidScriptBindingsDelegatePayload
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	bool bEnabled = false;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	EAvidScriptBindingsStructMode Mode =
		EAvidScriptBindingsStructMode::Primary;

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	FAvidScriptBindingsNestedStruct Nested;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FAvidScriptBindingsPreparedDelegate,
	UObject*,
	Target,
	FAvidScriptBindingsDelegatePayload,
	Payload,
	int64,
	Sequence);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FAvidScriptBindingsPreparedWideDelegate,
	double,
	Precision,
	int64,
	Token);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FAvidScriptBindingsPreparedOutputDelegate,
	UPARAM(ref) int32&,
	Value,
	int32&,
	Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FAvidScriptBindingsUnsupportedDelegate,
	const FString&,
	Text);

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
	UPROPERTY(BlueprintAssignable, Category = "AvidScript|Tests")
	FAvidScriptBindingsPreparedDelegate PreparedDelegate;

	UPROPERTY(BlueprintAssignable, Category = "AvidScript|Tests")
	FAvidScriptBindingsPreparedWideDelegate PreparedWideDelegate;

	UPROPERTY(BlueprintAssignable, Category = "AvidScript|Tests")
	FAvidScriptBindingsPreparedOutputDelegate PreparedOutputDelegate;

	UPROPERTY(BlueprintAssignable, Category = "AvidScript|Tests")
	FAvidScriptBindingsUnsupportedDelegate UnsupportedDelegate;

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

	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	TArray<int32> IntArrayProperty;

	UPROPERTY()
	TArray<FString> CompositeStringArrayProperty;

	UPROPERTY()
	TArray<FAvidScriptBindingsNestedStruct> CompositeStructArrayProperty;

	UPROPERTY()
	TSet<FString> CompositeStringSetProperty;

	UPROPERTY()
	TMap<FName, FString> CompositeNameStringMapProperty;

	UPROPERTY()
	FText CompositeTextProperty;

	UPROPERTY()
	TSoftObjectPtr<UObject> CompositeSoftObjectProperty;

	UPROPERTY()
	TWeakObjectPtr<UObject> CompositeWeakObjectProperty;

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

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Tests")
	TArray<int32> IntArrayRoundtrip(
		const TArray<int32>& Input,
		UPARAM(ref) TArray<int32>& InOut,
		TArray<int32>& OutValue) const;
};

UCLASS()
class UAvidScriptBindingsDerivedTestObject final
	: public UAvidScriptBindingsTestObject
{
	GENERATED_BODY()
};
