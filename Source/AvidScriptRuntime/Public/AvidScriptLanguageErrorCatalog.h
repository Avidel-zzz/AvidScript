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

// IR 17 language-error tokens are local to one canonical WASM module.
class AVIDSCRIPTRUNTIME_API FAvidScriptLanguageErrorCatalog final
{
public:
	// A missing section is valid for old modules. IR 17 provenance requires one.
	static bool ReadFromCanonicalWasm(
		TConstArrayView<uint8> CanonicalWasm,
		const FString& ExpectedModuleId,
		TUniquePtr<FAvidScriptLanguageErrorCatalog>& OutCatalog,
		FString& OutError);

	const FString* FindType(int32 Token) const;
	const FAvidScriptLanguageErrorSource* FindSource(int32 Token) const;

private:
	TArray<FString> TypeIds;
	TArray<FAvidScriptLanguageErrorSource> Sources;
};
