#include "AvidScriptLanguageErrorCatalog.h"

#include "AvidScriptWasmModuleLayout.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace AvidScriptLanguageErrorCatalogPrivate
{
constexpr uint32 MaxSectionBytes = 4 * 1024 * 1024;

bool Number(const FJsonObject& Object, const TCHAR* Name, int32 Minimum, int32 Maximum, int32& OutValue)
{
	const TSharedPtr<FJsonValue>* Value = Object.Values.Find(Name);
	double NumberValue = 0;
	if (!Value || !Value->IsValid() || !(*Value)->TryGetNumber(NumberValue) || !FMath::IsFinite(NumberValue)
		|| NumberValue < Minimum || NumberValue > Maximum) return false;
	OutValue = static_cast<int32>(NumberValue);
	return static_cast<double>(OutValue) == NumberValue;
}

bool IsLowerSha256(const FString& Value)
{
	if (Value.Len() != 64) return false;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
		if (!((Value[Index] >= '0' && Value[Index] <= '9')
			|| (Value[Index] >= 'a' && Value[Index] <= 'f'))) return false;
	return true;
}

bool IsTypeId(const FString& Value)
{
	if (!Value.StartsWith(TEXT("type:"), ESearchCase::CaseSensitive) || Value.Len() > 1024) return false;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
		if (Value[Index] < 0x20 || (Value[Index] >= 0x7f && Value[Index] <= 0x9f)) return false;
	return true;
}

bool IsSourceId(const FString& Value)
{
	if (Value.IsEmpty() || Value.Len() > 1024) return false;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
		if (Value[Index] == 0 || Value[Index] == '\r' || Value[Index] == '\n') return false;
	return true;
}

int32 ExecutionSchema(const FString& Profile)
{
	for (int32 Schema = 4; Schema <= 26; ++Schema)
		if (Profile == FString::Printf(TEXT("%d/1.%d"), Schema, Schema - 1)) return Schema;
	return 0;
}

FString ExecutionProfile(const TMap<FString, FString>& Fields)
{
	return Fields.FindRef(TEXT("guest_ir")) == TEXT("27/1.26")
		? Fields.FindRef(TEXT("guest_ir_base")) : Fields.FindRef(TEXT("guest_ir"));
}

bool ParseProvenance(TConstArrayView<uint8> Payload, TMap<FString, FString>& OutFields)
{
	if (Payload.IsEmpty()) return false;
	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
	TArray<FString> Lines;
	FString(Text.Length(), Text.Get()).ParseIntoArrayLines(Lines, false);
	for (const FString& Line : Lines)
	{
		int32 Separator = INDEX_NONE;
		if (!Line.FindChar('=', Separator) || Separator == 0) return false;
		FString Key = Line.Left(Separator);
		if (OutFields.Contains(Key)) return false;
		OutFields.Add(MoveTemp(Key), Line.Mid(Separator + 1));
	}
	const FString ArtifactProfile = OutFields.FindRef(TEXT("guest_ir"));
	const bool bStaticStorage = ArtifactProfile == TEXT("27/1.26");
	if ((ArtifactProfile.StartsWith(TEXT("27/"), ESearchCase::CaseSensitive) && !bStaticStorage)
		|| (bStaticStorage && ExecutionSchema(OutFields.FindRef(TEXT("guest_ir_base"))) == 0)
		|| (!bStaticStorage && OutFields.Contains(TEXT("guest_ir_base")))) return false;
	const FString Profile = ExecutionProfile(OutFields);
	const bool bRoutedThrow = Profile == TEXT("26/1.25");
	const bool bLifetime = bRoutedThrow || Profile == TEXT("25/1.24");
	const FString LifetimeModel = OutFields.FindRef(TEXT("task_local_exception_model"));
	const bool bLifetimeModelValid = LifetimeModel == TEXT("none") || LifetimeModel == TEXT("fault")
		|| LifetimeModel == TEXT("exception") || LifetimeModel == TEXT("cleanup")
		|| LifetimeModel == TEXT("cleanup_only") || LifetimeModel == TEXT("cancellation");
	return OutFields.Num() == (bLifetime ? 7 : 6) + (bStaticStorage ? 1 : 0)
		&& (!bLifetime || bLifetimeModelValid)
		&& (!bRoutedThrow || LifetimeModel == TEXT("cancellation"))
		&& OutFields.Contains(TEXT("module_id"))
		&& OutFields.Contains(TEXT("source_id"))
		&& OutFields.Contains(TEXT("source_sha256"))
		&& OutFields.Contains(TEXT("frontend_sha256"))
		&& OutFields.Contains(TEXT("semantic_sha256"))
		&& OutFields.Contains(TEXT("guest_ir"));
}

bool HasUniqueJsonKeys(const FString& Json)
{
	struct FFrame
	{
		bool bObject = false;
		TSet<FString> Keys;
	};
	TArray<FFrame> Frames;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	EJsonNotation Notation = EJsonNotation::Error;
	while (Reader->ReadNext(Notation))
	{
		if (Notation == EJsonNotation::Error) return false;
		if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd)
		{
			if (Frames.IsEmpty() || Frames.Last().bObject != (Notation == EJsonNotation::ObjectEnd))
				return false;
			Frames.Pop();
			continue;
		}
		if (!Frames.IsEmpty() && Frames.Last().bObject)
		{
			const FString& Key = Reader->GetIdentifier();
			if (Key.IsEmpty() || Frames.Last().Keys.Contains(Key)) return false;
			Frames.Last().Keys.Add(Key);
		}
		if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart)
		{
			if (Frames.Num() >= 8) return false;
			FFrame Frame;
			Frame.bObject = Notation == EJsonNotation::ObjectStart;
			Frames.Add(MoveTemp(Frame));
		}
	}
	return Frames.IsEmpty() && Reader->GetErrorMessage().IsEmpty();
}
}

bool FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
	TConstArrayView<uint8> CanonicalWasm,
	const FString& ExpectedModuleId,
	TUniquePtr<FAvidScriptLanguageErrorCatalog>& OutCatalog,
	FString& OutError)
{
	namespace CatalogPrivate = AvidScriptLanguageErrorCatalogPrivate;
	OutCatalog.Reset();
	OutError.Reset();
	TArray<uint8> Payload, Provenance;
	bool bFound = false, bProvenanceFound = false;
	if (!ReadAvidScriptWasmCustomSection(CanonicalWasm, TEXT("avidscript.language_errors"),
			CatalogPrivate::MaxSectionBytes, Payload, bFound, OutError)
		|| !ReadAvidScriptWasmCustomSection(CanonicalWasm, TEXT("avidscript.provenance"),
			CatalogPrivate::MaxSectionBytes, Provenance, bProvenanceFound, OutError))
		return false;

	TMap<FString, FString> ProvenanceFields;
	const bool bValidProvenance = bProvenanceFound
		&& CatalogPrivate::ParseProvenance(Provenance, ProvenanceFields);
	if (bProvenanceFound && !bValidProvenance)
	{
		OutError = TEXT("WASM provenance metadata is malformed");
		return false;
	}
	const FString ArtifactProfile = ProvenanceFields.FindRef(TEXT("guest_ir"));
	const FString Profile = CatalogPrivate::ExecutionProfile(ProvenanceFields);
	const int32 ProfileSchema = CatalogPrivate::ExecutionSchema(Profile);
	const bool bRoutedThrow = Profile == TEXT("26/1.25");
	const bool bLifetime = bRoutedThrow || Profile == TEXT("25/1.24");
	const FString LifetimeModel = ProvenanceFields.FindRef(TEXT("task_local_exception_model"));
	const bool bLifetimeErrors = bLifetime && LifetimeModel != TEXT("none") && LifetimeModel != TEXT("cleanup_only");
	if (!bFound)
	{
		if (bValidProvenance && (Profile == TEXT("17/1.16") || Profile == TEXT("20/1.19")
			|| Profile == TEXT("21/1.20") || Profile == TEXT("22/1.21")
			|| Profile == TEXT("23/1.22") || Profile == TEXT("24/1.23") || bLifetimeErrors))
		{
			OutError = TEXT("Catalog-bearing Guest IR WASM is missing language-error metadata");
			return false;
		}
		return true;
	}
	if (!bValidProvenance || ExpectedModuleId.IsEmpty()
		|| ProvenanceFields.FindRef(TEXT("module_id")) != ExpectedModuleId
		|| (Profile != TEXT("17/1.16") && Profile != TEXT("20/1.19")
			&& Profile != TEXT("21/1.20") && Profile != TEXT("22/1.21")
			&& Profile != TEXT("23/1.22") && Profile != TEXT("24/1.23") && !bLifetimeErrors))
	{
		OutError = TEXT("language-error metadata has no matching versioned provenance");
		return false;
	}
	for (const uint8 Byte : Payload)
		if (Byte < 0x20 || Byte > 0x7e)
		{
			OutError = TEXT("language-error metadata is not canonical JSON text");
			return false;
		}
	if (Payload.IsEmpty())
	{
		OutError = TEXT("language-error metadata is empty");
		return false;
	}
	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
	const FString Json(Text.Length(), Text.Get());
	TSharedPtr<FJsonObject> Document;
	if (!CatalogPrivate::HasUniqueJsonKeys(Json)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Document)
		|| !Document || Document->Values.Num() != 7)
	{
		OutError = TEXT("language-error metadata is not schema 1 JSON");
		return false;
	}
	int32 SectionVersion = 0, GuestSchema = 0;
	FString GuestVersion, ModuleId, SourceSha256;
	const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
	if (!CatalogPrivate::Number(*Document, TEXT("schema_version"), 1, 1, SectionVersion)
		|| !CatalogPrivate::Number(*Document, TEXT("guest_ir_schema_version"), 17, 27, GuestSchema)
		|| !Document->TryGetStringField(TEXT("guest_ir_version"), GuestVersion)
		|| FString::Printf(TEXT("%d/%s"), GuestSchema, *GuestVersion) != ArtifactProfile
		|| !Document->TryGetStringField(TEXT("module_id"), ModuleId) || ModuleId != ExpectedModuleId
		|| !Document->TryGetStringField(TEXT("source_sha256"), SourceSha256)
		|| !CatalogPrivate::IsLowerSha256(SourceSha256)
		|| SourceSha256 != ProvenanceFields.FindRef(TEXT("source_sha256"))
		|| !Document->TryGetArrayField(TEXT("types"), Types) || Types->Num() > 256
		|| !Document->TryGetArrayField(TEXT("sources"), Sources) || Sources->Num() > 1024
		|| ((Types->IsEmpty() || Sources->IsEmpty())
			&& ((ProfileSchema != 23 && !(ProfileSchema == 25 && LifetimeModel == TEXT("cleanup")))
				|| Types->Num() != Sources->Num())))
	{
		OutError = TEXT("language-error metadata identity or token counts are invalid");
		return false;
	}

	auto Candidate = MakeUnique<FAvidScriptLanguageErrorCatalog>();
	Candidate->GuestIrSchemaVersion = ProfileSchema;
	Candidate->bTaskLifetimeCancellation = (ProfileSchema == 25 || ProfileSchema == 26) && LifetimeModel == TEXT("cancellation");
	Candidate->TypeIds.Reserve(Types->Num());
	for (const TSharedPtr<FJsonValue>& Entry : *Types)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		int32 Token = 0;
		FString TypeId;
		if (!Entry || !Entry->TryGetObject(Object) || !Object || !*Object
			|| (*Object)->Values.Num() != 2
			|| !CatalogPrivate::Number(**Object, TEXT("token"), 1, 256, Token)
			|| Token != Candidate->TypeIds.Num() + 1
			|| !(*Object)->TryGetStringField(TEXT("type_id"), TypeId)
			|| !CatalogPrivate::IsTypeId(TypeId)
			|| (!Candidate->TypeIds.IsEmpty()
				&& Candidate->TypeIds.Last().Compare(TypeId, ESearchCase::CaseSensitive) >= 0))
		{
			OutError = TEXT("language-error type tokens are not canonical");
			return false;
		}
		Candidate->TypeIds.Add(MoveTemp(TypeId));
	}

	Candidate->Sources.Reserve(Sources->Num());
	TMap<FString, int32> SourceLengths;
	for (const TSharedPtr<FJsonValue>& Entry : *Sources)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		int32 Token = 0;
		FAvidScriptLanguageErrorSource Source;
		if (!Entry || !Entry->TryGetObject(Object) || !Object || !*Object
			|| (*Object)->Values.Num() != 9
			|| !CatalogPrivate::Number(**Object, TEXT("token"), 1, 1024, Token)
			|| Token != Candidate->Sources.Num() + 1
			|| !(*Object)->TryGetStringField(TEXT("source_id"), Source.SourceId)
			|| !CatalogPrivate::IsSourceId(Source.SourceId)
			|| !CatalogPrivate::Number(**Object, TEXT("source_length"), 1, 16000000, Source.SourceLength)
			|| !CatalogPrivate::Number(**Object, TEXT("start"), 0, Source.SourceLength, Source.Start)
			|| !CatalogPrivate::Number(**Object, TEXT("length"), 1, Source.SourceLength, Source.Length)
			|| Source.Start > Source.SourceLength - Source.Length
			|| !CatalogPrivate::Number(**Object, TEXT("line"), 0, Source.SourceLength, Source.Line)
			|| !CatalogPrivate::Number(**Object, TEXT("column"), 0, Source.SourceLength, Source.Column)
			|| !CatalogPrivate::Number(**Object, TEXT("end_line"), Source.Line, Source.SourceLength, Source.EndLine)
			|| !CatalogPrivate::Number(**Object, TEXT("end_column"), 0, Source.SourceLength, Source.EndColumn)
			|| (Source.EndLine == Source.Line && Source.EndColumn < Source.Column))
		{
			OutError = TEXT("language-error source token has an invalid span");
			return false;
		}
		if (const int32* PreviousLength = SourceLengths.Find(Source.SourceId))
		{
			if (*PreviousLength != Source.SourceLength)
			{
				OutError = TEXT("language-error source length changes within one source");
				return false;
			}
		}
		else SourceLengths.Add(Source.SourceId, Source.SourceLength);
		if (!Candidate->Sources.IsEmpty())
		{
			const FAvidScriptLanguageErrorSource& Previous = Candidate->Sources.Last();
			const int32 Compare = Previous.SourceId.Compare(Source.SourceId, ESearchCase::CaseSensitive);
			if (Compare > 0 || (Compare == 0
				&& (Previous.Start > Source.Start
					|| (Previous.Start == Source.Start && Previous.Length >= Source.Length))))
			{
				OutError = TEXT("language-error source tokens are not canonically ordered");
				return false;
			}
		}
		Candidate->Sources.Add(MoveTemp(Source));
	}
	OutCatalog = MoveTemp(Candidate);
	return true;
}

const FString* FAvidScriptLanguageErrorCatalog::FindType(int32 Token) const
{
	return Token > 0 && Token <= TypeIds.Num() ? &TypeIds[Token - 1] : nullptr;
}

bool FAvidScriptLanguageErrorCatalog::IsCancellationType(int32 Token) const
{
	const FString* Type = FindType(Token);
	return SupportsTaskCancellationError() && Type
		&& (Type->Equals(TEXT("type:global::System.Threading.Tasks.TaskCanceledException"),
			ESearchCase::CaseSensitive)
			|| Type->Equals(TEXT("type:global::System.OperationCanceledException"),
				ESearchCase::CaseSensitive));
}

const FAvidScriptLanguageErrorSource* FAvidScriptLanguageErrorCatalog::FindSource(int32 Token) const
{
	return Token > 0 && Token <= Sources.Num() ? &Sources[Token - 1] : nullptr;
}
