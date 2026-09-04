#pragma once

#include "CoreMinimal.h"

namespace AvidScript::Validation
{
bool IsHexIdentity(const FString& Value, int32 Length);
FString BuildConfiguration();
bool IsCookedGameProcess();

struct FUiSavePaths
{
	FString UserDir;
	FString EffectiveSavedDir;
	FString SavePath;
	FString ReportPath;
	FString InitialSaveHash;
	FString SaveHash;
	int64 SaveBytes = 0;
	bool bSaveExists = false;
	bool bInitialized = false;

	bool Initialize(FString& Error);
	bool CheckSaveDirectory(FString& Error) const;
	bool ReadSave(FString& Error);
	bool WriteNewReport(const FString& Json, FString& Error) const;
};
}
