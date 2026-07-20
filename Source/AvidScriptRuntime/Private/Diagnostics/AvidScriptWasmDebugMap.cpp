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
constexpr int32 MaxDisplayNameLength = 512;
constexpr int32 MaxIdentityLength = 2048;

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
	if (!TryGetDebugMapStrictInt32(*Root, TEXT("schema_version"), 1, 1, SchemaVersion)
		|| !Root->TryGetStringField(TEXT("debug_version"), DebugVersion)
		|| DebugVersion != TEXT("1.0")
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
	MutableMap->Functions.Reserve(FunctionValues->Num());
	TSet<FString> GuestFunctionIds;
	TSet<FString> MethodSymbolIds;
	uint32 PreviousFunctionIndex = 0;
	bool bHasPreviousFunctionIndex = false;
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
		Frame.Line = Function->Line + 1;
		Frame.Column = Function->Column + 1;
		Frame.EndLine = Function->EndLine + 1;
		Frame.EndColumn = Function->EndColumn + 1;
		Frame.bSourceMapped = true;
	}
}
