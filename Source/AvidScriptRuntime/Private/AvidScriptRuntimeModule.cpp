#include "AvidScriptRuntimeModule.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptRuntime, Log, All);

void FAvidScriptRuntimeModule::StartupModule()
{
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScriptRuntime module started."));
}

void FAvidScriptRuntimeModule::ShutdownModule()
{
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScriptRuntime module shut down."));
}

IMPLEMENT_MODULE(FAvidScriptRuntimeModule, AvidScriptRuntime)

