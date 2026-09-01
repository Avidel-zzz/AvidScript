#include "Diagnostics/AvidScriptWasmDebugMap.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Ssl.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
constexpr int64 MaxDebugMapByteSize = 4 * 1024 * 1024;
constexpr int32 MaxDebugFunctionCount = 65536;
constexpr int32 MaxDebugSequencePointsPerFunction = 16384;
constexpr int32 MaxDebugSequencePointCount = 262144;
constexpr int32 MaxDebugVariablesPerFunction = 256;
constexpr int32 MaxDisplayNameLength = 512;
constexpr int32 MaxIdentityLength = 2048;
constexpr int32 MaxDebugFrameByteCount = 4096;
constexpr int32 MaxDebugSnapshotVariables = 128;
constexpr int32 MaxDebugSnapshotValueCharacters = 16384;

void SetDebugMapFailure(
	TSharedPtr<const FAvidScriptWasmDebugMap>& OutMap,
	FString& OutErrorCategory,
	FString& OutErrorSource,
	const TCHAR* Category,
	const FString& Source)
{
	OutMap.Reset();
	OutErrorCategory = Category;
	OutErrorSource = Source;
}

bool IsDebugMapLowercaseSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

FString DebugMapBytesToLowerHex(const uint8* Bytes, int32 ByteCount)
{
	FString Hex;
	Hex.Reserve(ByteCount * 2);
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Hex += FString::Printf(TEXT("%02x"), Bytes[Index]);
	}
	return Hex;
}

FString ComputeDebugMapSha256Hex(TConstArrayView<uint8> Bytes)
{
	if (Bytes.IsEmpty())
	{
		return FString();
	}
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest);
	return DebugMapBytesToLowerHex(Digest, UE_ARRAY_COUNT(Digest));
}

bool TryGetDebugMapStrictInt32(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	int32 Minimum,
	int32 Maximum,
	int32& OutValue)
{
	double Number = 0.0;
	if (!Object.TryGetNumberField(FieldName, Number)
		|| !FMath::IsFinite(Number)
		|| Number < static_cast<double>(Minimum)
		|| Number > static_cast<double>(Maximum)
		|| Number != FMath::TruncToDouble(Number))
	{
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

bool TryGetDebugMapStrictUint32(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	uint32& OutValue)
{
	double Number = 0.0;
	if (!Object.TryGetNumberField(FieldName, Number)
		|| !FMath::IsFinite(Number)
		|| Number < 0.0
		|| Number > static_cast<double>(MAX_uint32)
		|| Number != FMath::TruncToDouble(Number))
	{
		return false;
	}
	OutValue = static_cast<uint32>(Number);
	return true;
}

bool TryGetDebugMapRequiredObject(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	TSharedPtr<FJsonObject>& OutObject)
{
	const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
	if (!Object.TryGetObjectField(FieldName, ObjectPtr)
		|| ObjectPtr == nullptr
		|| !ObjectPtr->IsValid())
	{
		return false;
	}
	OutObject = *ObjectPtr;
	return true;
}

bool TryGetDebugMapSpanOffsets(
	const FJsonObject& Object,
	int32& OutStart,
	int32& OutLength)
{
	int32 Line = 0;
	int32 Column = 0;
	int32 EndLine = 0;
	int32 EndColumn = 0;
	return TryGetDebugMapStrictInt32(Object, TEXT("start"), 0, MAX_int32, OutStart)
		&& TryGetDebugMapStrictInt32(Object, TEXT("length"), 0, MAX_int32, OutLength)
		&& OutLength <= MAX_int32 - OutStart
		&& TryGetDebugMapStrictInt32(Object, TEXT("line"), 0, MAX_int32 - 1, Line)
		&& TryGetDebugMapStrictInt32(Object, TEXT("column"), 0, MAX_int32 - 1, Column)
		&& TryGetDebugMapStrictInt32(Object, TEXT("end_line"), 0, MAX_int32 - 1, EndLine)
		&& TryGetDebugMapStrictInt32(Object, TEXT("end_column"), 0, MAX_int32 - 1, EndColumn)
		&& EndLine >= Line
		&& (EndLine > Line || EndColumn >= Column);
}

bool TryParseDebugProbeId(const FString& Text, uint64& OutProbeId)
{
	if (Text.Len() != 16)
	{
		return false;
	}
	uint64 Value = 0;
	for (const TCHAR Character : Text)
	{
		uint64 Digit = 0;
		if (Character >= TEXT('0') && Character <= TEXT('9'))
		{
			Digit = static_cast<uint64>(Character - TEXT('0'));
		}
		else if (Character >= TEXT('a') && Character <= TEXT('f'))
		{
			Digit = static_cast<uint64>(Character - TEXT('a') + 10);
		}
		else
		{
			return false;
		}
		Value = (Value << 4) | Digit;
	}
	OutProbeId = Value;
	return true;
}

bool IsSupportedDebugValueKind(const FString& Kind)
{
	return Kind == TEXT("scalar")
		|| Kind == TEXT("enum")
		|| Kind == TEXT("handle")
		|| Kind == TEXT("struct")
		|| Kind == TEXT("array")
		|| Kind == TEXT("string")
		|| Kind == TEXT("factory_ref")
		|| Kind == TEXT("object_type_ref")
		|| Kind == TEXT("composite_ref")
		|| Kind == TEXT("class_ref");
}

bool IsSupportedDebugStorage(const FString& Storage)
{
	return Storage == TEXT("i32")
		|| Storage == TEXT("i64")
		|| Storage == TEXT("f32")
		|| Storage == TEXT("f64")
		|| Storage == TEXT("memory");
}

uint64 ReadDebugUnsigned(
	TConstArrayView<uint8> Bytes,
	int32 Offset,
	int32 ByteCount)
{
	uint64 Value = 0;
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Value |= static_cast<uint64>(Bytes[Offset + Index]) << (Index * 8);
	}
	return Value;
}

bool IsCanonicalDebugMapSource(const FString& SourceFile)
{
	if (SourceFile.IsEmpty()
		|| SourceFile.Len() > MaxIdentityLength
		|| !FPaths::IsRelative(SourceFile)
		|| SourceFile.StartsWith(TEXT("/"))
		|| SourceFile.Contains(TEXT(":"))
		|| SourceFile.Contains(TEXT("\\")))
	{
		return false;
	}

	FString Collapsed = SourceFile;
	FPaths::CollapseRelativeDirectories(Collapsed, true);
	FPaths::NormalizeFilename(Collapsed);
	if (Collapsed != SourceFile
		|| SourceFile.Contains(TEXT("//"))
		|| SourceFile.StartsWith(TEXT("./"))
		|| SourceFile.EndsWith(TEXT("/")))
	{
		return false;
	}

	FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectRoot);
	if (!ProjectRoot.EndsWith(TEXT("/")))
	{
		ProjectRoot += TEXT("/");
	}
	FString Candidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(ProjectRoot, SourceFile));
	FPaths::NormalizeFilename(Candidate);
	return Candidate.StartsWith(ProjectRoot, ESearchCase::IgnoreCase);
}
}

bool FAvidScriptWasmDebugMap::LoadAndValidate(
	const FString& DebugMapPath,
	const FString& ExpectedArtifactSha256,
	const FAvidScriptWasmDebugProvenance& ExpectedProvenance,
	TConstArrayView<FAvidScriptWasmFunctionExport> FunctionExports,
	TSharedPtr<const FAvidScriptWasmDebugMap>& OutMap,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	OutMap.Reset();
	OutErrorCategory.Reset();
	OutErrorSource.Reset();

	if (!FPaths::FileExists(DebugMapPath))
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_file_missing"), DebugMapPath);
		return false;
	}
	if (!IsDebugMapLowercaseSha256(ExpectedArtifactSha256)
		|| !IsDebugMapLowercaseSha256(ExpectedProvenance.SourceSha256)
		|| !IsDebugMapLowercaseSha256(ExpectedProvenance.FrontendArtifactSha256)
		|| !IsDebugMapLowercaseSha256(ExpectedProvenance.SemanticSha256)
		|| !IsDebugMapLowercaseSha256(ExpectedProvenance.GuestIrSha256))
	{
		SetDebugMapFailure(
			OutMap,
			OutErrorCategory,
			OutErrorSource,
			TEXT("debug_map_manifest_invalid"),
			TEXT("expected debug map hashes must be lowercase SHA-256 values"));
		return false;
	}

	const int64 FileSize = IFileManager::Get().FileSize(*DebugMapPath);
	if (FileSize <= 0 || FileSize > MaxDebugMapByteSize)
	{
		SetDebugMapFailure(
			OutMap,
			OutErrorCategory,
			OutErrorSource,
			FileSize > MaxDebugMapByteSize ? TEXT("debug_map_artifact_too_large") : TEXT("debug_map_file_empty"),
			DebugMapPath);
		return false;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *DebugMapPath))
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_file_read_failed"), DebugMapPath);
		return false;
	}
	if (Bytes.Num() != FileSize || ComputeDebugMapSha256Hex(Bytes) != ExpectedArtifactSha256)
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_hash_mismatch"), DebugMapPath);
		return false;
	}

	FString JsonText;
	FFileHelper::BufferToString(JsonText, Bytes.GetData(), Bytes.Num());
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_invalid"), DebugMapPath);
		return false;
	}

	int32 SchemaVersion = 0;
	int32 ImportedFunctionCount = 0;
	int32 DefinedFunctionCount = 0;
	FString DebugVersion;
	FString ModuleId;
	if (!TryGetDebugMapStrictInt32(*Root, TEXT("schema_version"), 1, 2, SchemaVersion)
		|| !Root->TryGetStringField(TEXT("debug_version"), DebugVersion)
		|| !((SchemaVersion == 1 && DebugVersion == TEXT("1.0"))
			|| (SchemaVersion == 2 && DebugVersion == TEXT("2.0")))
		|| !Root->TryGetStringField(TEXT("module_id"), ModuleId)
		|| ModuleId.IsEmpty()
		|| ModuleId.Len() > MaxIdentityLength
		|| !TryGetDebugMapStrictInt32(
			*Root,
			TEXT("imported_function_count"),
			0,
			MAX_int32,
			ImportedFunctionCount)
		|| !TryGetDebugMapStrictInt32(
			*Root,
			TEXT("defined_function_count"),
			1,
			MaxDebugFunctionCount,
			DefinedFunctionCount))
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_invalid"), DebugMapPath);
		return false;
	}
	if (static_cast<uint32>(ImportedFunctionCount) != ExpectedProvenance.ImportedFunctionCount)
	{
		SetDebugMapFailure(
			OutMap,
			OutErrorCategory,
			OutErrorSource,
			TEXT("debug_map_function_index_range_mismatch"),
			FString::Printf(
				TEXT("expected_imports=%u actual_imports=%d"),
				ExpectedProvenance.ImportedFunctionCount,
				ImportedFunctionCount));
		return false;
	}
	if (static_cast<uint32>(DefinedFunctionCount) != ExpectedProvenance.DefinedFunctionCount)
	{
		SetDebugMapFailure(
			OutMap,
			OutErrorCategory,
			OutErrorSource,
			TEXT("debug_map_function_index_range_mismatch"),
			FString::Printf(
				TEXT("expected_defined=%u actual_defined=%d"),
				ExpectedProvenance.DefinedFunctionCount,
				DefinedFunctionCount));
		return false;
	}
	if (ModuleId != ExpectedProvenance.GuestModuleId)
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_module_mismatch"), ModuleId);
		return false;
	}

	TSharedPtr<FJsonObject> SourceObject;
	FString SourceFile;
	FString SourceSha256;
	if (!TryGetDebugMapRequiredObject(*Root, TEXT("source"), SourceObject)
		|| !SourceObject->TryGetStringField(TEXT("id"), SourceFile)
		|| !SourceObject->TryGetStringField(TEXT("sha256"), SourceSha256)
		|| !IsDebugMapLowercaseSha256(SourceSha256))
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_invalid"), DebugMapPath);
		return false;
	}
	if (!IsCanonicalDebugMapSource(SourceFile))
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_source_invalid"), SourceFile);
		return false;
	}
	if (SourceFile != ExpectedProvenance.SourceFile || SourceSha256 != ExpectedProvenance.SourceSha256)
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_source_mismatch"), SourceFile);
		return false;
	}

	TSharedPtr<FJsonObject> ProvenanceObject;
	FString FrontendArtifactSha256;
	FString SemanticSha256;
	FString GuestIrSha256;
	FString WasmSha256;
	if (!TryGetDebugMapRequiredObject(*Root, TEXT("provenance"), ProvenanceObject)
		|| !ProvenanceObject->TryGetStringField(TEXT("frontend_artifact_sha256"), FrontendArtifactSha256)
		|| !ProvenanceObject->TryGetStringField(TEXT("semantic_sha256"), SemanticSha256)
		|| !ProvenanceObject->TryGetStringField(TEXT("guest_ir_sha256"), GuestIrSha256)
		|| !IsDebugMapLowercaseSha256(FrontendArtifactSha256)
		|| !IsDebugMapLowercaseSha256(SemanticSha256)
		|| !IsDebugMapLowercaseSha256(GuestIrSha256))
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_invalid"), DebugMapPath);
		return false;
	}
	if (FrontendArtifactSha256 != ExpectedProvenance.FrontendArtifactSha256
		|| SemanticSha256 != ExpectedProvenance.SemanticSha256)
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_provenance_mismatch"), DebugMapPath);
		return false;
	}
	if (GuestIrSha256 != ExpectedProvenance.GuestIrSha256)
	{
		SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_guest_ir_mismatch"), DebugMapPath);
		return false;
	}
	if (SchemaVersion == 2
		&& (!ProvenanceObject->TryGetStringField(TEXT("wasm_sha256"), WasmSha256)
			|| !IsDebugMapLowercaseSha256(WasmSha256)
			|| !IsDebugMapLowercaseSha256(ExpectedProvenance.WasmSha256)
			|| WasmSha256 != ExpectedProvenance.WasmSha256))
	{
		SetDebugMapFailure(
			OutMap,
			OutErrorCategory,
			OutErrorSource,
			TEXT("debug_map_wasm_mismatch"),
			DebugMapPath);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* FunctionValues = nullptr;
	if (!Root->TryGetArrayField(TEXT("functions"), FunctionValues)
		|| FunctionValues == nullptr
		|| FunctionValues->IsEmpty()
		|| FunctionValues->Num() > MaxDebugFunctionCount)
	{
		SetDebugMapFailure(
			OutMap,
			OutErrorCategory,
			OutErrorSource,
			FunctionValues != nullptr && FunctionValues->Num() > MaxDebugFunctionCount
				? TEXT("debug_map_function_limit_exceeded")
				: TEXT("debug_map_invalid"),
			DebugMapPath);
		return false;
	}

	TSharedRef<FAvidScriptWasmDebugMap> MutableMap = MakeShared<FAvidScriptWasmDebugMap>();
	MutableMap->SourceFile = SourceFile;
	MutableMap->SourceSha256 = SourceSha256;
	MutableMap->Functions.Reserve(FunctionValues->Num());
	TSet<FString> GuestFunctionIds;
	TSet<FString> MethodSymbolIds;
	uint32 PreviousFunctionIndex = 0;
	bool bHasPreviousFunctionIndex = false;
	int32 TotalSequencePointCount = 0;
	const uint64 FunctionIndexLimit =
		static_cast<uint64>(ImportedFunctionCount) + static_cast<uint64>(DefinedFunctionCount);
	for (const TSharedPtr<FJsonValue>& FunctionValue : *FunctionValues)
	{
		const TSharedPtr<FJsonObject> FunctionObject = FunctionValue.IsValid() ? FunctionValue->AsObject() : nullptr;
		uint32 FunctionIndex = 0;
		FString GuestFunctionId;
		FString MethodSymbolId;
		FString DisplayName;
		TSharedPtr<FJsonObject> SpanObject;
		if (!FunctionObject.IsValid()
			|| !TryGetDebugMapStrictUint32(*FunctionObject, TEXT("wasm_function_index"), FunctionIndex)
			|| !FunctionObject->TryGetStringField(TEXT("guest_function_id"), GuestFunctionId)
			|| !FunctionObject->TryGetStringField(TEXT("method_symbol_id"), MethodSymbolId)
			|| !FunctionObject->TryGetStringField(TEXT("display_name"), DisplayName)
			|| GuestFunctionId.IsEmpty()
			|| GuestFunctionId.Len() > MaxIdentityLength
			|| MethodSymbolId.IsEmpty()
			|| MethodSymbolId.Len() > MaxIdentityLength
			|| DisplayName.IsEmpty()
			|| DisplayName.Len() > MaxDisplayNameLength
			|| !TryGetDebugMapRequiredObject(*FunctionObject, TEXT("span"), SpanObject))
		{
			SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_invalid"), DebugMapPath);
			return false;
		}
		if (MutableMap->Functions.Contains(FunctionIndex))
		{
			SetDebugMapFailure(
				OutMap,
				OutErrorCategory,
				OutErrorSource,
				TEXT("debug_map_duplicate_function_index"),
				FString::Printf(TEXT("%u"), FunctionIndex));
			return false;
		}
		if (GuestFunctionIds.Contains(GuestFunctionId) || MethodSymbolIds.Contains(MethodSymbolId))
		{
			SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_duplicate_function_identity"), DisplayName);
			return false;
		}
		if (FunctionIndex < static_cast<uint32>(ImportedFunctionCount)
			|| static_cast<uint64>(FunctionIndex) >= FunctionIndexLimit
			|| (bHasPreviousFunctionIndex && FunctionIndex <= PreviousFunctionIndex))
		{
			const FString PreviousFunctionIndexText = bHasPreviousFunctionIndex
				? FString::Printf(TEXT("%u"), PreviousFunctionIndex)
				: FString(TEXT("<none>"));
			SetDebugMapFailure(
				OutMap,
				OutErrorCategory,
				OutErrorSource,
				TEXT("debug_map_function_index_range_mismatch"),
				FString::Printf(
					TEXT("range=[%d,%llu) previous=%s actual=%u"),
					ImportedFunctionCount,
					FunctionIndexLimit,
					*PreviousFunctionIndexText,
					FunctionIndex));
			return false;
		}
		PreviousFunctionIndex = FunctionIndex;
		bHasPreviousFunctionIndex = true;

		int32 Start = 0;
		int32 Length = 0;
		FFunction Function;
		if (!TryGetDebugMapStrictInt32(*SpanObject, TEXT("start"), 0, MAX_int32, Start)
			|| !TryGetDebugMapStrictInt32(*SpanObject, TEXT("length"), 0, MAX_int32, Length)
			|| Length > MAX_int32 - Start
			|| !TryGetDebugMapStrictInt32(*SpanObject, TEXT("line"), 0, MAX_int32 - 1, Function.Line)
			|| !TryGetDebugMapStrictInt32(*SpanObject, TEXT("column"), 0, MAX_int32 - 1, Function.Column)
			|| !TryGetDebugMapStrictInt32(*SpanObject, TEXT("end_line"), 0, MAX_int32 - 1, Function.EndLine)
			|| !TryGetDebugMapStrictInt32(*SpanObject, TEXT("end_column"), 0, MAX_int32 - 1, Function.EndColumn)
			|| Function.EndLine < Function.Line
			|| (Function.EndLine == Function.Line && Function.EndColumn < Function.Column))
		{
			SetDebugMapFailure(OutMap, OutErrorCategory, OutErrorSource, TEXT("debug_map_span_invalid"), DisplayName);
			return false;
		}

		Function.DisplayName = MoveTemp(DisplayName);
		if (SchemaVersion == 2)
		{
			const TArray<TSharedPtr<FJsonValue>>* SequencePointValues = nullptr;
			if (!FunctionObject->TryGetArrayField(TEXT("sequence_points"), SequencePointValues)
				|| SequencePointValues == nullptr
				|| SequencePointValues->Num() > MaxDebugSequencePointsPerFunction
				|| SequencePointValues->Num() > MaxDebugSequencePointCount - TotalSequencePointCount)
			{
				SetDebugMapFailure(
					OutMap,
					OutErrorCategory,
					OutErrorSource,
					TEXT("debug_map_sequence_point_limit_exceeded"),
					Function.DisplayName);
				return false;
			}

			Function.SequencePoints.Reserve(SequencePointValues->Num());
			TSet<FString> GuestInstructionIds;
			TSet<FString> SemanticOperationIds;
			uint32 PreviousFunctionOffset = 0;
			bool bHasPreviousFunctionOffset = false;
			for (const TSharedPtr<FJsonValue>& SequencePointValue : *SequencePointValues)
			{
				const TSharedPtr<FJsonObject> SequencePointObject =
					SequencePointValue.IsValid() ? SequencePointValue->AsObject() : nullptr;
				TSharedPtr<FJsonObject> SequenceSpanObject;
				FString GuestInstructionId;
				FString SemanticOperationId;
				FString ProbeIdText;
				FString Kind;
				bool bHidden = false;
				FSequencePoint SequencePoint;
				if (!SequencePointObject.IsValid()
					|| !TryGetDebugMapStrictUint32(
						*SequencePointObject,
						TEXT("wasm_function_offset"),
						SequencePoint.FunctionOffset)
					|| !SequencePointObject->TryGetStringField(
						TEXT("guest_instruction_id"),
						GuestInstructionId)
					|| !SequencePointObject->TryGetStringField(
						TEXT("semantic_operation_id"),
						SemanticOperationId)
					|| !SequencePointObject->TryGetStringField(TEXT("kind"), Kind)
					|| !SequencePointObject->TryGetBoolField(TEXT("hidden"), bHidden)
					|| !TryGetDebugMapRequiredObject(
						*SequencePointObject,
						TEXT("span"),
						SequenceSpanObject)
					|| GuestInstructionId.IsEmpty()
					|| GuestInstructionId.Len() > MaxIdentityLength
					|| SemanticOperationId.IsEmpty()
					|| SemanticOperationId.Len() > MaxIdentityLength
					|| (Kind != TEXT("hidden")
						&& Kind != TEXT("statement")
						&& Kind != TEXT("call")
						&& Kind != TEXT("await")
						&& Kind != TEXT("return"))
					|| GuestInstructionIds.Contains(GuestInstructionId)
					|| SemanticOperationIds.Contains(SemanticOperationId)
					|| (bHasPreviousFunctionOffset
						&& SequencePoint.FunctionOffset < PreviousFunctionOffset)
					|| !TryGetDebugMapSpanOffsets(
						*SequenceSpanObject,
						SequencePoint.Start,
						SequencePoint.Length)
					|| !TryGetDebugMapStrictInt32(
						*SequenceSpanObject,
						TEXT("line"),
						0,
						MAX_int32 - 1,
						SequencePoint.Line)
					|| !TryGetDebugMapStrictInt32(
						*SequenceSpanObject,
						TEXT("column"),
						0,
						MAX_int32 - 1,
						SequencePoint.Column)
					|| !TryGetDebugMapStrictInt32(
						*SequenceSpanObject,
						TEXT("end_line"),
						0,
						MAX_int32 - 1,
						SequencePoint.EndLine)
					|| !TryGetDebugMapStrictInt32(
						*SequenceSpanObject,
						TEXT("end_column"),
						0,
						MAX_int32 - 1,
						SequencePoint.EndColumn)
					|| SequencePoint.EndLine < SequencePoint.Line
					|| (SequencePoint.EndLine == SequencePoint.Line
						&& SequencePoint.EndColumn < SequencePoint.Column))
				{
					SetDebugMapFailure(
						OutMap,
						OutErrorCategory,
						OutErrorSource,
						TEXT("debug_map_sequence_point_invalid"),
						Function.DisplayName);
					return false;
				}

				const TSharedPtr<FJsonValue>* ProbeIdValue =
					SequencePointObject->Values.Find(TEXT("probe_id"));
				if (ProbeIdValue != nullptr
					&& ProbeIdValue->IsValid()
					&& (*ProbeIdValue)->Type != EJson::Null)
				{
					if (bHidden
						|| !SequencePointObject->TryGetStringField(TEXT("probe_id"), ProbeIdText)
						|| !TryParseDebugProbeId(ProbeIdText, SequencePoint.ProbeId)
						|| MutableMap->FunctionIndicesByProbeId.Contains(SequencePoint.ProbeId))
					{
						SetDebugMapFailure(
							OutMap,
							OutErrorCategory,
							OutErrorSource,
							TEXT("debug_map_probe_id_invalid"),
							Function.DisplayName);
						return false;
					}
					SequencePoint.bHasProbeId = true;
					MutableMap->FunctionIndicesByProbeId.Add(
						SequencePoint.ProbeId,
						FunctionIndex);
				}

				SequencePoint.Kind = MoveTemp(Kind);
				SequencePoint.bHidden = bHidden;
				Function.SequencePoints.Add(MoveTemp(SequencePoint));
				GuestInstructionIds.Add(MoveTemp(GuestInstructionId));
				SemanticOperationIds.Add(MoveTemp(SemanticOperationId));
				PreviousFunctionOffset = Function.SequencePoints.Last().FunctionOffset;
				bHasPreviousFunctionOffset = true;
			}
			TotalSequencePointCount += SequencePointValues->Num();

			const TSharedPtr<FJsonValue>* FrameValue = FunctionObject->Values.Find(TEXT("frame"));
			if (FrameValue != nullptr
				&& FrameValue->IsValid()
				&& (*FrameValue)->Type != EJson::Null)
			{
				const TSharedPtr<FJsonObject> FrameObject = (*FrameValue)->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* VariableValues = nullptr;
				if (!FrameObject.IsValid()
					|| !TryGetDebugMapStrictInt32(
						*FrameObject,
						TEXT("byte_size"),
						1,
						MaxDebugFrameByteCount,
						Function.FrameByteCount)
					|| !FrameObject->TryGetArrayField(TEXT("variables"), VariableValues)
					|| VariableValues == nullptr
					|| VariableValues->Num() > MaxDebugVariablesPerFunction)
				{
					SetDebugMapFailure(
						OutMap,
						OutErrorCategory,
						OutErrorSource,
						TEXT("debug_map_variable_layout_invalid"),
						Function.DisplayName);
					return false;
				}

				TSet<FString> VariableSymbolIds;
				Function.Variables.Reserve(VariableValues->Num());
				for (const TSharedPtr<FJsonValue>& VariableValue : *VariableValues)
				{
					const TSharedPtr<FJsonObject> VariableObject =
						VariableValue.IsValid() ? VariableValue->AsObject() : nullptr;
					TSharedPtr<FJsonObject> DeclarationObject;
					TSharedPtr<FJsonObject> ScopeObject;
					FVariable Variable;
					if (!VariableObject.IsValid()
						|| !VariableObject->TryGetStringField(TEXT("symbol_id"), Variable.SymbolId)
						|| !VariableObject->TryGetStringField(TEXT("name"), Variable.Name)
						|| !VariableObject->TryGetStringField(TEXT("kind"), Variable.Kind)
						|| !VariableObject->TryGetStringField(TEXT("type_id"), Variable.TypeId)
						|| !VariableObject->TryGetStringField(TEXT("value_kind"), Variable.ValueKind)
						|| !VariableObject->TryGetStringField(TEXT("storage"), Variable.Storage)
						|| Variable.SymbolId.IsEmpty()
						|| Variable.SymbolId.Len() > MaxIdentityLength
						|| Variable.Name.IsEmpty()
						|| Variable.Name.Len() > MaxDisplayNameLength
						|| Variable.TypeId.IsEmpty()
						|| Variable.TypeId.Len() > MaxIdentityLength
						|| Variable.Kind != TEXT("parameter") && Variable.Kind != TEXT("local")
						|| !IsSupportedDebugValueKind(Variable.ValueKind)
						|| !IsSupportedDebugStorage(Variable.Storage)
						|| !TryGetDebugMapStrictInt32(
							*VariableObject,
							TEXT("offset"),
							static_cast<int32>(sizeof(int32)),
							Function.FrameByteCount - 1,
							Variable.Offset)
						|| !TryGetDebugMapStrictInt32(
							*VariableObject,
							TEXT("byte_size"),
							1,
							Function.FrameByteCount,
							Variable.ByteSize)
						|| Variable.Offset > Function.FrameByteCount - Variable.ByteSize
						|| !TryGetDebugMapRequiredObject(
							*VariableObject,
							TEXT("declaration"),
							DeclarationObject)
						|| !TryGetDebugMapRequiredObject(
							*VariableObject,
							TEXT("scope"),
							ScopeObject)
						|| !TryGetDebugMapSpanOffsets(
							*DeclarationObject,
							Variable.DeclarationStart,
							Variable.DeclarationLength)
						|| !TryGetDebugMapSpanOffsets(
							*ScopeObject,
							Variable.ScopeStart,
							Variable.ScopeLength)
						|| Variable.DeclarationStart < Variable.ScopeStart
						|| Variable.DeclarationStart > Variable.ScopeStart + Variable.ScopeLength
						|| Variable.DeclarationLength >
							Variable.ScopeStart + Variable.ScopeLength - Variable.DeclarationStart
						|| VariableSymbolIds.Contains(Variable.SymbolId))
					{
						SetDebugMapFailure(
							OutMap,
							OutErrorCategory,
							OutErrorSource,
							TEXT("debug_map_variable_layout_invalid"),
							Function.DisplayName);
						return false;
					}
					const bool bStorageSizeValid =
						(Variable.Storage == TEXT("i32")
							&& (Variable.ByteSize == 1
								|| Variable.ByteSize == 2
								|| Variable.ByteSize == 4))
						|| (Variable.Storage == TEXT("i64") && Variable.ByteSize == 8)
						|| (Variable.Storage == TEXT("f32") && Variable.ByteSize == 4)
						|| (Variable.Storage == TEXT("f64") && Variable.ByteSize == 8)
						|| Variable.Storage == TEXT("memory");
					const bool bValueStorageValid =
						(Variable.ValueKind == TEXT("scalar")
							&& Variable.Storage != TEXT("memory"))
						|| (Variable.ValueKind == TEXT("enum")
							&& Variable.Storage == TEXT("i32"))
						|| (Variable.ValueKind == TEXT("handle")
							&& Variable.Storage == TEXT("i64"))
						|| (Variable.ValueKind == TEXT("struct")
							&& Variable.Storage == TEXT("memory"))
						|| ((Variable.ValueKind == TEXT("array")
								|| Variable.ValueKind == TEXT("string")
								|| Variable.ValueKind == TEXT("factory_ref")
								|| Variable.ValueKind == TEXT("object_type_ref")
								|| Variable.ValueKind == TEXT("composite_ref")
								|| Variable.ValueKind == TEXT("class_ref"))
							&& Variable.Storage == TEXT("i32"));
					if (!bStorageSizeValid || !bValueStorageValid)
					{
						SetDebugMapFailure(
							OutMap,
							OutErrorCategory,
							OutErrorSource,
							TEXT("debug_map_variable_layout_invalid"),
							Function.DisplayName);
						return false;
					}

					for (const FVariable& Existing : Function.Variables)
					{
						const bool bOverlaps = Variable.Offset < Existing.Offset + Existing.ByteSize
							&& Existing.Offset < Variable.Offset + Variable.ByteSize;
						if (bOverlaps)
						{
							SetDebugMapFailure(
								OutMap,
								OutErrorCategory,
								OutErrorSource,
								TEXT("debug_map_variable_layout_invalid"),
								Function.DisplayName);
							return false;
						}
					}
					VariableSymbolIds.Add(Variable.SymbolId);
					Function.Variables.Add(MoveTemp(Variable));
				}
			}
		}
		MutableMap->Functions.Add(FunctionIndex, MoveTemp(Function));
		GuestFunctionIds.Add(MoveTemp(GuestFunctionId));
		MethodSymbolIds.Add(MoveTemp(MethodSymbolId));
	}

	MutableMap->FunctionIndicesByExportName.Reserve(FunctionExports.Num());
	for (const FAvidScriptWasmFunctionExport& FunctionExport : FunctionExports)
	{
		if (FunctionExport.Name.IsEmpty()
			|| FunctionExport.Name.Len() > MaxIdentityLength
			|| static_cast<uint64>(FunctionExport.FunctionIndex) >= FunctionIndexLimit
			|| MutableMap->FunctionIndicesByExportName.Contains(FunctionExport.Name))
		{
			SetDebugMapFailure(
				OutMap,
				OutErrorCategory,
				OutErrorSource,
				TEXT("debug_map_wasm_layout_mismatch"),
				FunctionExport.Name);
			return false;
		}
		MutableMap->FunctionIndicesByExportName.Add(FunctionExport.Name, FunctionExport.FunctionIndex);
	}

	OutMap = MutableMap;
	return true;
}

void FAvidScriptWasmDebugMap::MapFrames(
	TConstArrayView<FAvidScriptVmStackFrame> VmFrames,
	TArray<FAvidScriptWasmDiagnosticFrame>& OutFrames) const
{
	OutFrames.Reset(VmFrames.Num());
	for (const FAvidScriptVmStackFrame& VmFrame : VmFrames)
	{
		FAvidScriptWasmDiagnosticFrame& Frame = OutFrames.AddDefaulted_GetRef();
		uint32 ResolvedFunctionIndex = VmFrame.FunctionIndex;
		if (ResolvedFunctionIndex == MAX_uint32)
		{
			if (const uint32* ExportFunctionIndex = FunctionIndicesByExportName.Find(VmFrame.RawFunctionToken))
			{
				ResolvedFunctionIndex = *ExportFunctionIndex;
			}
		}

		Frame.FunctionIndex = ResolvedFunctionIndex;
		Frame.FunctionOffset = VmFrame.FunctionOffset;
		Frame.RawFunctionToken = VmFrame.RawFunctionToken;
		Frame.FunctionName = VmFrame.RawFunctionToken;

		const FFunction* Function = Functions.Find(ResolvedFunctionIndex);
		if (Function == nullptr)
		{
			continue;
		}
		Frame.FunctionName = Function->DisplayName;
		Frame.SourceFile = SourceFile;
		Frame.SourceSha256 = SourceSha256;
		Frame.Kind = EAvidScriptWasmDiagnosticFrameKind::CSharp;

		const FSequencePoint* ResolvedSequencePoint = nullptr;
		int32 Low = 0;
		int32 High = Function->SequencePoints.Num();
		while (Low < High)
		{
			const int32 Middle = Low + ((High - Low) / 2);
			if (Function->SequencePoints[Middle].FunctionOffset <= VmFrame.FunctionOffset)
			{
				Low = Middle + 1;
			}
			else
			{
				High = Middle;
			}
		}
		for (int32 CandidateIndex = Low - 1; CandidateIndex >= 0; --CandidateIndex)
		{
			const FSequencePoint& Candidate = Function->SequencePoints[CandidateIndex];
			if (!Candidate.bHidden)
			{
				ResolvedSequencePoint = &Candidate;
				break;
			}
		}

		if (ResolvedSequencePoint != nullptr)
		{
			Frame.Line = ResolvedSequencePoint->Line + 1;
			Frame.Column = ResolvedSequencePoint->Column + 1;
			Frame.EndLine = ResolvedSequencePoint->EndLine + 1;
			Frame.EndColumn = ResolvedSequencePoint->EndColumn + 1;
			Frame.SourceKind = ResolvedSequencePoint->Kind;
			Frame.bSequencePointMapped = true;
		}
		else
		{
			Frame.Line = Function->Line + 1;
			Frame.Column = Function->Column + 1;
			Frame.EndLine = Function->EndLine + 1;
			Frame.EndColumn = Function->EndColumn + 1;
			Frame.SourceKind = TEXT("function");
		}
		Frame.bSourceMapped = true;
	}
}

bool FAvidScriptWasmDebugMap::BuildVariableSnapshot(
	const uint64 ProbeId,
	const TConstArrayView<uint8> FrameBytes,
	FAvidScriptDebugVariablesSnapshot& OutSnapshot,
	FString& OutError) const
{
	OutSnapshot = FAvidScriptDebugVariablesSnapshot();
	OutError.Reset();
	const uint32* FunctionIndex = FunctionIndicesByProbeId.Find(ProbeId);
	const FFunction* Function = FunctionIndex != nullptr
		? Functions.Find(*FunctionIndex)
		: nullptr;
	if (Function == nullptr)
	{
		OutError = TEXT("active probe has no variable metadata");
		return false;
	}
	const FSequencePoint* SequencePoint = Function->SequencePoints.FindByPredicate(
		[ProbeId](const FSequencePoint& Candidate)
		{
			return Candidate.bHasProbeId && Candidate.ProbeId == ProbeId;
		});
	if (SequencePoint == nullptr
		|| Function->FrameByteCount <= 0
		|| FrameBytes.Num() != Function->FrameByteCount
		|| FrameBytes.Num() > MaxDebugFrameByteCount)
	{
		OutError = TEXT("active probe suspension frame does not match its debug map");
		return false;
	}

	OutSnapshot.ActiveProbeId = ProbeId;
	OutSnapshot.SourceFile = SourceFile;
	OutSnapshot.SourceSha256 = SourceSha256;
	OutSnapshot.FunctionName = Function->DisplayName;
	OutSnapshot.Line = SequencePoint->Line + 1;
	OutSnapshot.Column = SequencePoint->Column + 1;
	OutSnapshot.Variables.Reserve(FMath::Min(
		Function->Variables.Num(),
		MaxDebugSnapshotVariables));
	int32 SnapshotCharacterCount = 0;
	for (const FVariable& Variable : Function->Variables)
	{
		const int32 DeclarationEnd =
			Variable.DeclarationStart + Variable.DeclarationLength;
		const int32 ScopeEnd = Variable.ScopeStart + Variable.ScopeLength;
		const bool bVisible = Variable.Kind == TEXT("parameter")
			|| (SequencePoint->Start >= DeclarationEnd
				&& SequencePoint->Start >= Variable.ScopeStart
				&& SequencePoint->Start < ScopeEnd);
		if (!bVisible)
		{
			continue;
		}
		if (OutSnapshot.Variables.Num() >= MaxDebugSnapshotVariables)
		{
			OutSnapshot.bTruncated = true;
			break;
		}

		const uint64 Raw = Variable.ByteSize <= 8
			? ReadDebugUnsigned(FrameBytes, Variable.Offset, Variable.ByteSize)
			: 0;
		FString DisplayValue;
		if (Variable.ValueKind == TEXT("scalar"))
		{
			if (Variable.TypeId == TEXT("type:bool"))
			{
				DisplayValue = Raw == 0 ? TEXT("false") : TEXT("true");
			}
			else if (Variable.TypeId == TEXT("type:int8"))
			{
				DisplayValue = LexToString(static_cast<int8>(Raw));
			}
			else if (Variable.TypeId == TEXT("type:uint8"))
			{
				DisplayValue = LexToString(static_cast<uint8>(Raw));
			}
			else if (Variable.TypeId == TEXT("type:int16"))
			{
				DisplayValue = LexToString(static_cast<int16>(Raw));
			}
			else if (Variable.TypeId == TEXT("type:uint16")
				|| Variable.TypeId == TEXT("type:char16"))
			{
				DisplayValue = LexToString(static_cast<uint16>(Raw));
			}
			else if (Variable.TypeId == TEXT("type:int32"))
			{
				DisplayValue = LexToString(static_cast<int32>(Raw));
			}
			else if (Variable.TypeId == TEXT("type:uint32"))
			{
				DisplayValue = LexToString(static_cast<uint32>(Raw));
			}
			else if (Variable.TypeId == TEXT("type:int64"))
			{
				DisplayValue = LexToString(static_cast<int64>(Raw));
			}
			else if (Variable.TypeId == TEXT("type:uint64"))
			{
				DisplayValue = LexToString(Raw);
			}
			else if (Variable.Storage == TEXT("f32"))
			{
				const uint32 Bits = static_cast<uint32>(Raw);
				float Value = 0.0f;
				FMemory::Memcpy(&Value, &Bits, sizeof(Value));
				DisplayValue = FString::SanitizeFloat(Value);
			}
			else if (Variable.Storage == TEXT("f64"))
			{
				double Value = 0.0;
				FMemory::Memcpy(&Value, &Raw, sizeof(Value));
				DisplayValue = FString::SanitizeFloat(Value);
			}
			else
			{
				DisplayValue = FString::Printf(TEXT("0x%llx"), Raw);
			}
		}
		else if (Variable.ValueKind == TEXT("enum"))
		{
			DisplayValue = LexToString(static_cast<int32>(Raw));
		}
		else if (Variable.ValueKind == TEXT("handle"))
		{
			DisplayValue = FString::Printf(TEXT("ObjectHandle(0x%016llx)"), Raw);
		}
		else if (Variable.Storage == TEXT("memory"))
		{
			DisplayValue = FString::Printf(
				TEXT("<value type, %d bytes>"),
				Variable.ByteSize);
		}
		else
		{
			DisplayValue = FString::Printf(
				TEXT("%s#%llu"),
				*Variable.ValueKind,
				Raw);
		}

		const int32 NextSnapshotCharacterCount = SnapshotCharacterCount
			+ Variable.SymbolId.Len()
			+ Variable.Name.Len()
			+ Variable.Kind.Len()
			+ Variable.TypeId.Len()
			+ Variable.ValueKind.Len()
			+ DisplayValue.Len();
		if (NextSnapshotCharacterCount > MaxDebugSnapshotValueCharacters)
		{
			OutSnapshot.bTruncated = true;
			break;
		}
		SnapshotCharacterCount = NextSnapshotCharacterCount;
		FAvidScriptDebugVariableSnapshot& Snapshot =
			OutSnapshot.Variables.AddDefaulted_GetRef();
		Snapshot.SymbolId = Variable.SymbolId;
		Snapshot.Name = Variable.Name;
		Snapshot.Kind = Variable.Kind;
		Snapshot.TypeId = Variable.TypeId;
		Snapshot.ValueKind = Variable.ValueKind;
		Snapshot.Value = MoveTemp(DisplayValue);
	}
	return true;
}
