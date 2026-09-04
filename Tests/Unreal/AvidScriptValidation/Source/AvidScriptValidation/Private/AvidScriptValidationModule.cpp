#include "AvidScriptUiSavePackagedProbe.h"
#include "AvidScriptUiSavePackagedWorldProbe.h"

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
		if (Mode == TEXT("world"))
		{
			Probe = MakeUnique<AvidScript::Validation::FUiSavePackagedWorldProbe>();
		}
		else
		{
			Probe = MakeUnique<AvidScript::Validation::FUiSavePackagedProbe>();
		}
		Probe->Start();
	}

	virtual void ShutdownModule() override
	{
		Probe.Reset();
	}

private:
	TUniquePtr<AvidScript::Validation::IUiSavePackagedProbe> Probe;
};

IMPLEMENT_MODULE(FAvidScriptValidationModule, AvidScriptValidation)
