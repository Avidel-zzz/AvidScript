#pragma once

#include "AvidScriptVmBackend.h"

bool ParseAvidScriptWamrCallStack(
	const FString& Text,
	TArray<FAvidScriptVmStackFrame>& OutFrames);

void CaptureAvidScriptWamrCallStack(
	void* ExecEnv,
	TArray<FAvidScriptVmStackFrame>& OutFrames);
