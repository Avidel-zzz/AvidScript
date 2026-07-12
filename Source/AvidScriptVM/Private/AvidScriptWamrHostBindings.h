#pragma once

#include "AvidScriptVmBackend.h"

class IAvidScriptWamrHostBridge
{
public:
	virtual ~IAvidScriptWamrHostBridge() = default;
	virtual bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) = 0;
	virtual void RecordHostImportFailure(const char* ImportName, const FString& Details) = 0;
};

bool RegisterAvidScriptWamrHostBindings();
void UnregisterAvidScriptWamrHostBindings();
