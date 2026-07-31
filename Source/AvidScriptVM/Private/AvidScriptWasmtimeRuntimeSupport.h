#pragma once

#include "AvidScriptVmBackend.h"

void InitializeAvidScriptWasmtimeRuntimeDescriptor(
	FAvidScriptVmBackendInfo& InOutInfo);

bool ResolveAvidScriptWasmtimeRuntimeIdentity(
	FAvidScriptVmBackendInfo& InOutInfo,
	FString& OutError);
