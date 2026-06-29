#pragma once

#include "Modules/ModuleInterface.h"

class FAvidScriptRuntimeModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

