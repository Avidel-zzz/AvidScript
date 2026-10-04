#pragma once
#include "CoreMinimal.h"
#include "Continuation/AvidScriptTaskValueCatalog.h"

// Published only with its successfully loaded VM. Never an import capability.
class FAvidScriptWasmTaskValueCatalog final
{
public:
    static bool ReadFromCanonicalWasm(TConstArrayView<uint8> CanonicalWasm, const FString& ExpectedModuleId,
        TUniquePtr<FAvidScriptWasmTaskValueCatalog>& OutCatalog, FString& OutError);
    const AvidScript::TaskResult::FValueCatalog& GetValues() const { return Values; }
    const FString& GetCanonicalSha256() const { return CanonicalSha256; }
private:
    AvidScript::TaskResult::FValueCatalog Values;
    FString CanonicalSha256;
};
