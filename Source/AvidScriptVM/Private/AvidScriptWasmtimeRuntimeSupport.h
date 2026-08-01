#pragma once

#include "AvidScriptWasmtimeApi.h"
#include "AvidScriptVmBackend.h"

void InitializeAvidScriptWasmtimeRuntimeDescriptor(
	FAvidScriptVmBackendInfo& InOutInfo);

bool ResolveAvidScriptWasmtimeRuntimeIdentity(
	FAvidScriptVmBackendInfo& InOutInfo,
	FString& OutError);

bool ResolveAvidScriptWasmtimeCompilerProfile(
	FAvidScriptVmBackendInfo& InOutInfo,
	AvidScriptWasmtimeEngineProfile& OutProfile,
	FString& OutError,
	FString* OutErrorCategory = nullptr);
