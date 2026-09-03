#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

class FJsonValue;
class IFileHandle;

namespace AvidScript::UiSaveDemo
{
class FSaveFixtureController
{
public:
	FSaveFixtureController(const FString& SavePath, TFunction<bool()> CheckSafe);
	~FSaveFixtureController();

	FSaveFixtureController(const FSaveFixtureController&) = delete;
	FSaveFixtureController& operator=(const FSaveFixtureController&) = delete;

	bool ObserveScriptSave(FString& Error);
	bool Prepare(const FString& Kind, FString& Error);
	bool LockForSaveFailure(FString& Error);
	bool CheckUnchanged(FString& Error) const;
	void Unlock();
	bool IsLocked() const;
	const FString& GetExpectedHash() const;
	const TArray<TSharedPtr<FJsonValue>>& GetEvidence() const;

private:
	bool CheckPath(FString& Error) const;
	bool ReadCurrent(TArray<uint8>& Bytes, FString& Error) const;

	FString SavePath;
	TFunction<bool()> CheckSafe;
	FString ExpectedHash;
	TArray<TSharedPtr<FJsonValue>> Evidence;
	TUniquePtr<IFileHandle> ReadLock;
};
}
