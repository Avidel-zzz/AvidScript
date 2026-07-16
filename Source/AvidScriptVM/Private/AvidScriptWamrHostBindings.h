#pragma once

#include "AvidScriptVmBackend.h"

struct FAvidScriptWamrRawImportAttachment;

class IAvidScriptWamrHostBridge
{
public:
	virtual ~IAvidScriptWamrHostBridge() = default;
	virtual bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) = 0;
	virtual bool DispatchDynamicHostCall(
		const FAvidScriptWamrRawImportAttachment& Attachment,
		TConstArrayView<uint64> Arguments,
		int32& OutReturnValue,
		FString& OutFailureDetails) = 0;
	virtual void RecordHostImportFailure(const char* ImportName, const FString& Details) = 0;
};

bool IsAvidScriptWamrStaticHostImport(const FString& ModuleName, const FString& ImportName);
bool RegisterAvidScriptWamrHostBindings();
void UnregisterAvidScriptWamrHostBindings();
