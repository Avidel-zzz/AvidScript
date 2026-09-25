#pragma once

#include "CoreMinimal.h"

struct FAvidScriptLanguageErrorSource
{
	FString SourceId;
	int32 SourceLength = 0;
	int32 Start = 0;
	int32 Length = 0;
	int32 Line = 0;
	int32 Column = 0;
	int32 EndLine = 0;
	int32 EndColumn = 0;
};

// Language-error tokens are local to one canonical IR 17 or combined IR 20 WASM module.
class AVIDSCRIPTRUNTIME_API FAvidScriptLanguageErrorCatalog final
{
public:
	// A missing section is valid for old modules. IR 17 and IR 20 require one.
	static bool ReadFromCanonicalWasm(
		TConstArrayView<uint8> CanonicalWasm,
		const FString& ExpectedModuleId,
		TUniquePtr<FAvidScriptLanguageErrorCatalog>& OutCatalog,
		FString& OutError);

	const FString* FindType(int32 Token) const;
	const FAvidScriptLanguageErrorSource* FindSource(int32 Token) const;
	bool SupportsTaskLanguageErrorFault() const { return GuestIrSchemaVersion == 20; }

private:
	int32 GuestIrSchemaVersion = 0;
	TArray<FString> TypeIds;
	TArray<FAvidScriptLanguageErrorSource> Sources;
};
