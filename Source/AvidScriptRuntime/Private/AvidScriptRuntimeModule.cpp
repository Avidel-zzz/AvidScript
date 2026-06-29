#include "AvidScriptRuntimeModule.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptRuntime, Log, All);

#ifndef AVIDSCRIPT_WITH_WAMR
#define AVIDSCRIPT_WITH_WAMR 0
#endif

void FAvidScriptRuntimeModule::StartupModule()
{
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScriptRuntime module started."));

#if AVIDSCRIPT_WITH_WAMR
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScript WAMR backend is enabled."));
#else
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScript WAMR backend is disabled; ThirdParty WAMR artifacts are not available yet."));
#endif
}

void FAvidScriptRuntimeModule::ShutdownModule()
{
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScriptRuntime module shut down."));
}

IMPLEMENT_MODULE(FAvidScriptRuntimeModule, AvidScriptRuntime)
