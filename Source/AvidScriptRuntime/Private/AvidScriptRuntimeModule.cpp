#include "AvidScriptRuntimeModule.h"

#include "Modules/ModuleManager.h"
#include "Tests/AvidScriptNetworkTopologyHarness.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptRuntime, Log, All);

void FAvidScriptRuntimeModule::StartupModule()
{
	FAvidScriptNetworkTopologyHarness::Startup();
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScriptRuntime composition module started."));
}

void FAvidScriptRuntimeModule::ShutdownModule()
{
	FAvidScriptNetworkTopologyHarness::Shutdown();
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScriptRuntime module shut down."));
}

IMPLEMENT_MODULE(FAvidScriptRuntimeModule, AvidScriptRuntime)
