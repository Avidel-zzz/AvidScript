#include "AvidScriptPerfRunCommandlet.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Modules/ModuleManager.h"

class FAvidScriptPerfHarnessModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RunCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("AvidScript.PerformanceComparison.Run"),
			TEXT("Run the warm four-lane AvidScript/Puerts performance comparison."),
			FConsoleCommandDelegate::CreateRaw(
				this,
				&FAvidScriptPerfHarnessModule::RunWarmBenchmark),
			ECVF_Default);
	}

	virtual void ShutdownModule() override
	{
		if (RunCommand != nullptr)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunCommand);
			RunCommand = nullptr;
		}
	}

private:
	void RunWarmBenchmark()
	{
		const int32 ExitCode =
			UAvidScriptPerfRunCommandlet::RunFromCommandLine(FCommandLine::Get());
		FPlatformMisc::RequestExitWithStatus(
			true,
			static_cast<uint8>(ExitCode));
	}

	IConsoleObject* RunCommand = nullptr;
};

IMPLEMENT_MODULE(FAvidScriptPerfHarnessModule, AvidScriptPerfHarness)
