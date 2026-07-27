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
	FAvidScriptGeneratedVectorValueCall VectorValueCall = nullptr;
	FAvidScriptGeneratedObjectRoundtripCall ObjectRoundtripCall = nullptr;
};

struct FAvidScriptGeneratedBindingPackageState;

class AVIDSCRIPTBINDINGS_API FAvidScriptGeneratedBindingLease
{
public:
	bool IsActive() const;
	const FAvidScriptGeneratedBindingEntry* GetEntry() const;

private:
	friend class FAvidScriptGeneratedBindingRegistry;

	TSharedPtr<FAvidScriptGeneratedBindingPackageState> State;
	int32 EntryIndex = INDEX_NONE;
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
