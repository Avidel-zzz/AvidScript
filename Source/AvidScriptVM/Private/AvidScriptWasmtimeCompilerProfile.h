#pragma once

#include "AvidScriptWasmtimeApi.h"
#include "CoreMinimal.h"

struct FAvidScriptWasmtimeCompilerProfile
{
	FString Id;
	FString TargetTriple;
	FString CpuProfile;
	AvidScriptWasmtimeEngineProfile EngineProfile = {};
};

const FAvidScriptWasmtimeCompilerProfile&
GetAvidScriptWasmtimeCompilerProfile();

const FAvidScriptWasmtimeCompilerProfile*
FindAvidScriptWasmtimeCompilerProfile(const FString& TargetTriple);

bool ValidateAvidScriptWasmtimeCompilerCpuProfile(FString& OutError);

FString BuildAvidScriptWasmtimeCompilerIdentity(
	const FString& RuntimeVersion,
	const FString& RuntimeArtifactSha256,
	const FAvidScriptWasmtimeCompilerProfile& CompilerProfile);
