#pragma once

#include "AvidScriptGeneratedBindingTypes.h"
#include "AvidScriptVmTypedHostImport.h"
#include "CoreMinimal.h"

class UObject;

using FAvidScriptGeneratedI32PairCall = EAvidScriptVmTypedHostStatus (*)(
	UObject& Receiver,
	int32 Left,
	int32 Right,
	int32& OutValue);

using FAvidScriptGeneratedPropertyI32Call = EAvidScriptVmTypedHostStatus (*)(
	UObject& Receiver,
	bool bWrite,
	int32& InOutValue);

using FAvidScriptGeneratedPropertyI32GetCall =
	EAvidScriptVmTypedHostStatus (*)(
		UObject& Receiver,
		int32& OutValue);

using FAvidScriptGeneratedPropertyI32SetCall =
	EAvidScriptVmTypedHostStatus (*)(
		UObject& Receiver,
		int32 Value);

using FAvidScriptGeneratedVectorValueCall = EAvidScriptVmTypedHostStatus (*)(
	UObject& Receiver,
	const FVector& InValue,
	FVector& OutValue);

using FAvidScriptGeneratedObjectRoundtripCall = EAvidScriptVmTypedHostStatus (*)(
	UObject& Receiver,
	UObject* InValue,
	UObject*& OutValue);

struct FAvidScriptGeneratedBindingEntry
{
	FString StableId;
	FString PackageHash;
	FString DescriptorIdentity;
	EAvidScriptGeneratedBindingShape Shape =
		EAvidScriptGeneratedBindingShape::I32PairToI32;
	EAvidScriptGeneratedReceiverMode ReceiverMode =
		EAvidScriptGeneratedReceiverMode::SelfBound;
	FAvidScriptGeneratedI32PairCall I32PairCall = nullptr;
	FAvidScriptGeneratedPropertyI32Call PropertyI32Call = nullptr;
	FAvidScriptGeneratedPropertyI32GetCall PropertyI32GetCall = nullptr;
	FAvidScriptGeneratedPropertyI32SetCall PropertyI32SetCall = nullptr;
	FAvidScriptGeneratedVectorValueCall VectorValueCall = nullptr;
	FAvidScriptGeneratedObjectRoundtripCall ObjectRoundtripCall = nullptr;
	FAvidScriptGeneratedI32PairCall PreparedI32PairCall = nullptr;
	FAvidScriptGeneratedPropertyI32GetCall
		PreparedPropertyI32GetCall = nullptr;
	FAvidScriptGeneratedPropertyI32SetCall
		PreparedPropertyI32SetCall = nullptr;
};

struct FAvidScriptGeneratedBindingPackageState;

class AVIDSCRIPTBINDINGS_API FAvidScriptGeneratedBindingLease
{
public:
	bool IsActive() const;
	const FAvidScriptGeneratedBindingEntry* GetEntry() const;
	FORCEINLINE const FAvidScriptGeneratedBindingEntry*
	GetEntryGameThreadFast() const
	{
		checkSlow(IsInGameThread());
		return ActivityFlag != nullptr
				&& *ActivityFlag
				&& CachedEntry != nullptr
			? CachedEntry
			: nullptr;
	}

private:
	friend class FAvidScriptGeneratedBindingRegistry;

	TSharedPtr<FAvidScriptGeneratedBindingPackageState> State;
	const bool* ActivityFlag = nullptr;
	const FAvidScriptGeneratedBindingEntry* CachedEntry = nullptr;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptGeneratedBindingRegistry
{
public:
	static FAvidScriptGeneratedBindingRegistry& Get();

	bool RegisterPackage(
		const FString& PackageHash,
		TConstArrayView<FAvidScriptGeneratedBindingEntry> Entries,
		FString& OutError);
	void UnregisterPackage(const FString& PackageHash);
	bool IsPackageActive(const FString& PackageHash) const;
	bool Acquire(
		const FString& PackageHash,
		const FString& StableId,
		const FString& DescriptorIdentity,
		EAvidScriptGeneratedBindingShape Shape,
		FAvidScriptGeneratedBindingLease& OutLease,
		FString& OutError) const;

private:
	TMap<FString, TSharedPtr<FAvidScriptGeneratedBindingPackageState>> Packages;
};
