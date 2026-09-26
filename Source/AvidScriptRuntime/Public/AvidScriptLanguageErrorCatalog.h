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

// Language-error tokens are local to one canonical IR 17 or 20-25 WASM module.
class AVIDSCRIPTRUNTIME_API FAvidScriptLanguageErrorCatalog final
{
public:
	// A missing section is valid for old modules. Catalog-bearing IR 17 and 20-24 require one.
	static bool ReadFromCanonicalWasm(
		TConstArrayView<uint8> CanonicalWasm,
		const FString& ExpectedModuleId,
		TUniquePtr<FAvidScriptLanguageErrorCatalog>& OutCatalog,
		FString& OutError);

	const FString* FindType(int32 Token) const;
	const FAvidScriptLanguageErrorSource* FindSource(int32 Token) const;
	bool SupportsTaskLanguageErrorFault() const
	{
		return GuestIrSchemaVersion >= 20 && GuestIrSchemaVersion <= 25;
	}
	bool SupportsTaskCancellationError() const { return GuestIrSchemaVersion == 24 || bTaskLifetimeCancellation; }
	bool IsCancellationType(int32 Token) const;

private:
	int32 GuestIrSchemaVersion = 0;
	bool bTaskLifetimeCancellation = false;
	TArray<FString> TypeIds;
	TArray<FAvidScriptLanguageErrorSource> Sources;
};
