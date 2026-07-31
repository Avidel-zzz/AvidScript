#include "AvidScriptGeneratedBindingRegistry.h"

#include "Algo/Sort.h"
#include "Generated/AvidScriptGeneratedCallSite.h"

struct FAvidScriptGeneratedBindingPackageState
{
	FString PackageHash;
	TArray<FAvidScriptGeneratedBindingEntry> Entries;
	bool bActive = true;
};

namespace
{
bool IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		const bool bIsAsciiDigit =
			Character >= TEXT('0') && Character <= TEXT('9');
		const bool bIsLowerHexLetter =
			Character >= TEXT('a') && Character <= TEXT('f');
		if (!bIsAsciiDigit && !bIsLowerHexLetter)
		{
			return false;
		}
	}
	return true;
}
} // namespace

bool FAvidScriptGeneratedBindingLease::IsActive() const
{
	return IsInGameThread()
		&& State.IsValid()
		&& ActivityFlag != nullptr
		&& *ActivityFlag
		&& CachedEntry != nullptr;
}

const FAvidScriptGeneratedBindingEntry*
FAvidScriptGeneratedBindingLease::GetEntry() const
{
	return IsActive() ? CachedEntry : nullptr;
}

FAvidScriptGeneratedBindingRegistry&
FAvidScriptGeneratedBindingRegistry::Get()
{
	static FAvidScriptGeneratedBindingRegistry Registry;
	return Registry;
}

bool FAvidScriptGeneratedBindingRegistry::RegisterPackage(
	const FString& PackageHash,
	const TConstArrayView<FAvidScriptGeneratedBindingEntry> Entries,
	FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Generated binding registration requires the Game Thread.");
		return false;
	}
	if (!IsCanonicalSha256(PackageHash) || Entries.IsEmpty())
	{
		OutError = TEXT("Generated binding package identity and entries are required.");
		return false;
	}
	if (Packages.Contains(PackageHash))
	{
		OutError = TEXT("Generated binding package identity is already registered.");
		return false;
	}

	TSharedPtr<FAvidScriptGeneratedBindingPackageState> State =
		MakeShared<FAvidScriptGeneratedBindingPackageState>();
	State->PackageHash = PackageHash;
	State->Entries.Append(Entries.GetData(), Entries.Num());
	State->Entries.Sort(
		[](const FAvidScriptGeneratedBindingEntry& Left,
			const FAvidScriptGeneratedBindingEntry& Right)
		{
			if (Left.StableId != Right.StableId)
			{
				return Left.StableId < Right.StableId;
			}
			return Left.DescriptorIdentity < Right.DescriptorIdentity;
		});

	TSet<FString> StableIds;
	TSet<FString> DescriptorIdentities;
	StableIds.Reserve(State->Entries.Num());
	DescriptorIdentities.Reserve(State->Entries.Num());
	for (const FAvidScriptGeneratedBindingEntry& Entry : State->Entries)
	{
		if (Entry.PackageHash != PackageHash
			|| !IsCanonicalSha256(Entry.StableId)
			|| Entry.DescriptorIdentity.IsEmpty()
			|| !IsAvidScriptGeneratedCallSiteValid(Entry))
		{
			OutError = TEXT("Generated binding entry identity or call pointer is invalid.");
			return false;
		}
		if (StableIds.Contains(Entry.StableId)
			|| DescriptorIdentities.Contains(Entry.DescriptorIdentity))
		{
			OutError = TEXT("Generated binding stable or descriptor identity is duplicated.");
			return false;
		}
		StableIds.Add(Entry.StableId);
		DescriptorIdentities.Add(Entry.DescriptorIdentity);
	}

	Packages.Add(PackageHash, MoveTemp(State));
	return true;
}

void FAvidScriptGeneratedBindingRegistry::UnregisterPackage(
	const FString& PackageHash)
{
	if (!IsInGameThread())
	{
		return;
	}
	if (TSharedPtr<FAvidScriptGeneratedBindingPackageState>* State =
			Packages.Find(PackageHash))
	{
		(*State)->bActive = false;
		Packages.Remove(PackageHash);
	}
}

bool FAvidScriptGeneratedBindingRegistry::IsPackageActive(
	const FString& PackageHash) const
{
	if (!IsInGameThread())
	{
		return false;
	}
	const TSharedPtr<FAvidScriptGeneratedBindingPackageState>* State =
		Packages.Find(PackageHash);
	return State != nullptr && (*State)->bActive;
}

bool FAvidScriptGeneratedBindingRegistry::Acquire(
	const FString& PackageHash,
	const FString& StableId,
	const FString& DescriptorIdentity,
	const EAvidScriptGeneratedBindingShape Shape,
	FAvidScriptGeneratedBindingLease& OutLease,
	FString& OutError) const
{
	OutLease = FAvidScriptGeneratedBindingLease();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Generated binding acquisition requires the Game Thread.");
		return false;
	}

	const TSharedPtr<FAvidScriptGeneratedBindingPackageState>* State =
		Packages.Find(PackageHash);
	if (State == nullptr || !(*State)->bActive)
	{
		OutError = TEXT("Generated binding package is not active.");
		return false;
	}

	for (int32 Index = 0; Index < (*State)->Entries.Num(); ++Index)
	{
		const FAvidScriptGeneratedBindingEntry& Entry = (*State)->Entries[Index];
		if (Entry.StableId != StableId)
		{
			continue;
		}
		if (Entry.DescriptorIdentity != DescriptorIdentity || Entry.Shape != Shape)
		{
			OutError = TEXT("Generated binding descriptor identity or shape mismatch.");
			return false;
		}
		OutLease.State = *State;
		OutLease.ActivityFlag = &(*State)->bActive;
		OutLease.CachedEntry = &Entry;
		return true;
	}

	OutError = TEXT("Generated binding stable identity was not found.");
	return false;
}
