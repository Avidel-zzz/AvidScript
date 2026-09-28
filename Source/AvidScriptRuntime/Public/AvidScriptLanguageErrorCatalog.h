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

// Language-error tokens are local to one canonical execution profile,
// including profile 26 inside the exact IR 35 async capability set.
class AVIDSCRIPTRUNTIME_API FAvidScriptLanguageErrorCatalog final
{
public:
	// A missing section is valid only for modules without the language-error capability.
	static bool ReadFromCanonicalWasm(
		TConstArrayView<uint8> CanonicalWasm,
		const FString& ExpectedModuleId,
		TUniquePtr<FAvidScriptLanguageErrorCatalog>& OutCatalog,
		FString& OutError);

	const FString* FindType(int32 Token) const;
	const FAvidScriptLanguageErrorSource* FindSource(int32 Token) const;
	bool SupportsTaskLanguageErrorFault() const
	{
		return GuestIrSchemaVersion >= 20 && GuestIrSchemaVersion <= 26;
	}
	bool SupportsTaskCancellationError() const { return GuestIrSchemaVersion == 24 || bTaskLifetimeCancellation; }
	bool SupportsTaskCancellationIdentity() const { return bTaskCancellationIdentity; }
	bool SupportsExceptionCancellationToken() const { return bExceptionCancellationToken; }
	bool IsCancellationType(int32 Token) const;

private:
	int32 GuestIrSchemaVersion = 0;
	bool bTaskLifetimeCancellation = false;
	bool bTaskCancellationIdentity = false;
	bool bExceptionCancellationToken = false;
	TArray<FString> TypeIds;
	TArray<FAvidScriptLanguageErrorSource> Sources;
};
