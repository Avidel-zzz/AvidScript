#include "AvidScriptUiSavePackagedProbe.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

class FAvidScriptValidationModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Mode;
		if (!FParse::Value(FCommandLine::Get(), TEXT("AvidScriptUiSavePackagedProbe="), Mode)
			&& !FParse::Param(FCommandLine::Get(), TEXT("AvidScriptUiSavePackagedProbe")))
		{
			return;
		}
		Probe = MakeUnique<AvidScript::Validation::FUiSavePackagedProbe>();
		Probe->Start();
	}

	virtual void ShutdownModule() override
	{
		Probe.Reset();
	}

private:
	TUniquePtr<AvidScript::Validation::FUiSavePackagedProbe> Probe;
};

IMPLEMENT_MODULE(FAvidScriptValidationModule, AvidScriptValidation)
