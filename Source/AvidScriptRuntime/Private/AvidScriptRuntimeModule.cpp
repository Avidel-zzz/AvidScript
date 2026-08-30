#include "AvidScriptRuntimeModule.h"

#include "Modules/ModuleManager.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRouter.h"
#include "Tests/AvidScriptNetworkTopologyHarness.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptRuntime, Log, All);

void FAvidScriptRuntimeModule::StartupModule()
{
	if (!FAvidScriptGeneratedTypeRouter::Get().Startup())
	{
		UE_LOG(LogAvidScriptRuntime, Error, TEXT("AvidScript generated type router failed to install."));
	}
	FAvidScriptNetworkTopologyHarness::Startup();
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScriptRuntime composition module started."));
}

void FAvidScriptRuntimeModule::ShutdownModule()
{
	FAvidScriptNetworkTopologyHarness::Shutdown();
	FAvidScriptGeneratedTypeRouter::Get().Shutdown();
	UE_LOG(LogAvidScriptRuntime, Log, TEXT("AvidScriptRuntime module shut down."));
}

IMPLEMENT_MODULE(FAvidScriptRuntimeModule, AvidScriptRuntime)
