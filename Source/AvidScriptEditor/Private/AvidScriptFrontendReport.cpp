#include "AvidScriptFrontendReport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
void SetAvidScriptReportLoadFailure(
	const FString& ReportPath,
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	FAvidScriptFrontendReportLoadResult& OutResult)
{
	OutResult = FAvidScriptFrontendReportLoadResult();
	OutResult.ReportPath = ReportPath;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
}

void SetAvidScriptReportLoadSuccess(
	const FString& ReportPath,
	FAvidScriptFrontendReportLoadResult& OutResult)
{
	OutResult = FAvidScriptFrontendReportLoadResult();
	OutResult.bSucceeded = true;
	OutResult.ReportPath = ReportPath;
}

int32 GetAvidScriptJsonIntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, int32 DefaultValue)
{
	double NumberValue = 0.0;
	if (Object.IsValid() && Object->TryGetNumberField(FieldName, NumberValue))
	{
		return static_cast<int32>(NumberValue);
	}

	return DefaultValue;
}

FString GetAvidScriptJsonValueAsString(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->IsNull())
	{
		return FString();
	}

	switch (Value->Type)
	{
	case EJson::String:
		return Value->AsString();
	case EJson::Number:
		return FString::SanitizeFloat(Value->AsNumber());
	case EJson::Boolean:
		return Value->AsBool() ? FString(TEXT("true")) : FString(TEXT("false"));
	default:
		return FString();
	}
}

void LoadAvidScriptDiagnostics(
	const TSharedPtr<FJsonObject>& RootObject,
	TArray<FAvidScriptFrontendDiagnostic>& OutDiagnostics)
{
	OutDiagnostics.Reset();

	const TArray<TSharedPtr<FJsonValue>>* DiagnosticsArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("diagnostics"), DiagnosticsArray) || DiagnosticsArray == nullptr)
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& DiagnosticValue : *DiagnosticsArray)
	{
		const TSharedPtr<FJsonObject> DiagnosticObject = DiagnosticValue.IsValid() ? DiagnosticValue->AsObject() : nullptr;
		if (!DiagnosticObject.IsValid())
		{
			continue;
		}

		FAvidScriptFrontendDiagnostic Diagnostic;
		DiagnosticObject->TryGetStringField(TEXT("code"), Diagnostic.Code);
		DiagnosticObject->TryGetStringField(TEXT("severity"), Diagnostic.Severity);
		DiagnosticObject->TryGetStringField(TEXT("file"), Diagnostic.File);
		Diagnostic.Start = GetAvidScriptJsonIntField(DiagnosticObject, TEXT("start"), INDEX_NONE);
		Diagnostic.Length = GetAvidScriptJsonIntField(DiagnosticObject, TEXT("length"), 0);
		Diagnostic.Line = GetAvidScriptJsonIntField(DiagnosticObject, TEXT("line"), 0);
		Diagnostic.Column = GetAvidScriptJsonIntField(DiagnosticObject, TEXT("column"), 0);
		Diagnostic.EndLine = GetAvidScriptJsonIntField(DiagnosticObject, TEXT("end_line"), Diagnostic.Line);
		Diagnostic.EndColumn = GetAvidScriptJsonIntField(DiagnosticObject, TEXT("end_column"), Diagnostic.Column);
		DiagnosticObject->TryGetStringField(TEXT("message"), Diagnostic.Message);
		OutDiagnostics.Add(MoveTemp(Diagnostic));
	}
}

void LoadAvidScriptBuildEvents(
	const TSharedPtr<FJsonObject>& RootObject,
	TArray<FAvidScriptFrontendBuildEvent>& OutBuildEvents)
{
	OutBuildEvents.Reset();

	const TArray<TSharedPtr<FJsonValue>>* BuildEventsArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("build_events"), BuildEventsArray) || BuildEventsArray == nullptr)
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& BuildEventValue : *BuildEventsArray)
	{
		const TSharedPtr<FJsonObject> BuildEventObject = BuildEventValue.IsValid() ? BuildEventValue->AsObject() : nullptr;
		if (!BuildEventObject.IsValid())
		{
			continue;
		}

		FAvidScriptFrontendBuildEvent BuildEvent;
		BuildEventObject->TryGetStringField(TEXT("result"), BuildEvent.Result);

		const TSharedPtr<FJsonObject>* FieldsObject = nullptr;
		if (BuildEventObject->TryGetObjectField(TEXT("fields"), FieldsObject) && FieldsObject != nullptr && FieldsObject->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& FieldPair : (*FieldsObject)->Values)
			{
				BuildEvent.Fields.Add(FieldPair.Key, GetAvidScriptJsonValueAsString(FieldPair.Value));
			}
		}

		OutBuildEvents.Add(MoveTemp(BuildEvent));
	}
}

void LoadAvidScriptRawOutput(
	const TSharedPtr<FJsonObject>& RootObject,
	TArray<FString>& OutRawOutput)
{
	OutRawOutput.Reset();

	const TArray<TSharedPtr<FJsonValue>>* RawOutputArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("raw_output"), RawOutputArray) || RawOutputArray == nullptr)
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& RawOutputValue : *RawOutputArray)
	{
		FString Line;
		if (RawOutputValue.IsValid() && RawOutputValue->TryGetString(Line))
		{
			OutRawOutput.Add(MoveTemp(Line));
		}
	}
}

void LoadAvidScriptSource(const TSharedPtr<FJsonObject>& RootObject, FAvidScriptFrontendReport& OutReport)
{
	const TSharedPtr<FJsonObject>* SourceObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("source"), SourceObject) && SourceObject != nullptr && SourceObject->IsValid())
	{
		(*SourceObject)->TryGetStringField(TEXT("file"), OutReport.Source);
		(*SourceObject)->TryGetStringField(TEXT("sha256"), OutReport.SourceSha256);
		(*SourceObject)->TryGetStringField(TEXT("script_type"), OutReport.ScriptType);
		return;
	}

	RootObject->TryGetStringField(TEXT("source"), OutReport.Source);
}

void LoadAvidScriptFrontendMetadata(const TSharedPtr<FJsonObject>& RootObject, FAvidScriptFrontendReport& OutReport)
{
	const TSharedPtr<FJsonObject>* ArtifactsObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("artifacts"), ArtifactsObject) && ArtifactsObject != nullptr && ArtifactsObject->IsValid())
	{
		(*ArtifactsObject)->TryGetStringField(TEXT("frontend_file"), OutReport.FrontendArtifact);
	}

	const TSharedPtr<FJsonObject>* FrontendObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("frontend"), FrontendObject) && FrontendObject != nullptr && FrontendObject->IsValid())
	{
		OutReport.FrontendSchemaVersion = GetAvidScriptJsonIntField(*FrontendObject, TEXT("schema_version"), 0);
		(*FrontendObject)->TryGetStringField(TEXT("version"), OutReport.FrontendVersion);
	}
}

void LoadAvidScriptSemanticMetadata(const TSharedPtr<FJsonObject>& RootObject, FAvidScriptFrontendReport& OutReport)
{
	const TSharedPtr<FJsonObject>* ArtifactsObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("artifacts"), ArtifactsObject) && ArtifactsObject != nullptr && ArtifactsObject->IsValid())
	{
		(*ArtifactsObject)->TryGetStringField(TEXT("semantic_file"), OutReport.SemanticArtifact);
	}

	const TSharedPtr<FJsonObject>* SemanticObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("semantic"), SemanticObject) && SemanticObject != nullptr && SemanticObject->IsValid())
	{
		OutReport.SemanticSchemaVersion = GetAvidScriptJsonIntField(*SemanticObject, TEXT("schema_version"), 0);
		(*SemanticObject)->TryGetStringField(TEXT("version"), OutReport.SemanticVersion);
		(*SemanticObject)->TryGetBoolField(TEXT("succeeded"), OutReport.bSemanticSucceeded);
		(*SemanticObject)->TryGetStringField(TEXT("source_sha256"), OutReport.SemanticSourceSha256);
		(*SemanticObject)->TryGetStringField(TEXT("frontend_sha256"), OutReport.SemanticFrontendSha256);
		OutReport.SemanticDiagnosticCount = GetAvidScriptJsonIntField(*SemanticObject, TEXT("diagnostic_count"), 0);
	}
}

void LoadAvidScriptGuestIrMetadata(const TSharedPtr<FJsonObject>& RootObject, FAvidScriptFrontendReport& OutReport)
{
	const TSharedPtr<FJsonObject>* ArtifactsObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("artifacts"), ArtifactsObject) && ArtifactsObject != nullptr && ArtifactsObject->IsValid())
	{
		(*ArtifactsObject)->TryGetStringField(TEXT("guest_ir_file"), OutReport.GuestIrArtifact);
	}

	const TSharedPtr<FJsonObject>* GuestIrObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("guest_ir"), GuestIrObject) && GuestIrObject != nullptr && GuestIrObject->IsValid())
	{
		OutReport.GuestIrSchemaVersion = GetAvidScriptJsonIntField(*GuestIrObject, TEXT("schema_version"), 0);
		(*GuestIrObject)->TryGetStringField(TEXT("version"), OutReport.GuestIrVersion);
		(*GuestIrObject)->TryGetBoolField(TEXT("succeeded"), OutReport.bGuestIrSucceeded);
		(*GuestIrObject)->TryGetStringField(TEXT("semantic_sha256"), OutReport.GuestIrSemanticSha256);
		(*GuestIrObject)->TryGetStringField(TEXT("sha256"), OutReport.GuestIrSha256);
	}
}

void LoadAvidScriptBindingPackageMetadata(const TSharedPtr<FJsonObject>& RootObject, FAvidScriptFrontendReport& OutReport)
{
	OutReport.BindingPackage = FAvidScriptFrontendBindingPackage();
	const TSharedPtr<FJsonObject>* PackageObject = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("binding_package"), PackageObject)
		|| PackageObject == nullptr
		|| !PackageObject->IsValid())
	{
		return;
	}

	FAvidScriptFrontendBindingPackage& Package = OutReport.BindingPackage;
	Package.bPresent = true;
	(*PackageObject)->TryGetBoolField(TEXT("required"), Package.bRequired);
	(*PackageObject)->TryGetStringField(TEXT("package_manifest"), Package.PackageManifest);
	(*PackageObject)->TryGetStringField(TEXT("package_name"), Package.PackageName);
	(*PackageObject)->TryGetStringField(TEXT("package_hash"), Package.PackageHash);
	(*PackageObject)->TryGetStringField(TEXT("descriptor_file"), Package.DescriptorFile);
	(*PackageObject)->TryGetStringField(TEXT("descriptor_sha256"), Package.DescriptorSha256);
	(*PackageObject)->TryGetStringField(TEXT("reference_source_file"), Package.ReferenceSourceFile);
	(*PackageObject)->TryGetStringField(TEXT("reference_source_sha256"), Package.ReferenceSourceSha256);
	Package.ProfileImportCount = GetAvidScriptJsonIntField(*PackageObject, TEXT("profile_import_count"), 0);
	Package.UsedImportCount = GetAvidScriptJsonIntField(*PackageObject, TEXT("used_import_count"), 0);

	const TArray<TSharedPtr<FJsonValue>>* UsedImports = nullptr;
	if (!(*PackageObject)->TryGetArrayField(TEXT("used_imports"), UsedImports) || UsedImports == nullptr)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& ImportValue : *UsedImports)
	{
		const TSharedPtr<FJsonObject> ImportObject = ImportValue.IsValid() ? ImportValue->AsObject() : nullptr;
		if (!ImportObject.IsValid())
		{
			continue;
		}

		FAvidScriptFrontendBindingImport Import;
		ImportObject->TryGetStringField(TEXT("stable_id"), Import.StableId);
		Import.Ordinal = GetAvidScriptJsonIntField(ImportObject, TEXT("ordinal"), INDEX_NONE);
		ImportObject->TryGetStringField(TEXT("module"), Import.Module);
		ImportObject->TryGetStringField(TEXT("name"), Import.Name);
		ImportObject->TryGetStringField(TEXT("signature"), Import.Signature);
		Package.UsedImports.Add(MoveTemp(Import));
	}
}
} // namespace

bool FAvidScriptFrontendDiagnostic::IsError() const
{
	return Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase);
}

const FAvidScriptFrontendBuildEvent* FAvidScriptFrontendReport::GetLastBuildEvent() const
{
	return BuildEvents.Num() > 0 ? &BuildEvents.Last() : nullptr;
}

bool FAvidScriptFrontendReport::HasErrorDiagnostics() const
{
	for (const FAvidScriptFrontendDiagnostic& Diagnostic : Diagnostics)
	{
		if (Diagnostic.IsError())
		{
			return true;
		}
	}

	return false;
}

bool FAvidScriptFrontendReportReader::LoadFromFile(
	const FString& ReportPath,
	FAvidScriptFrontendReport& OutReport,
	FAvidScriptFrontendReportLoadResult& OutResult)
{
	OutReport = FAvidScriptFrontendReport();
	OutResult = FAvidScriptFrontendReportLoadResult();

	FString NormalizedReportPath = ReportPath;
	FPaths::NormalizeFilename(NormalizedReportPath);

	if (!FPaths::FileExists(NormalizedReportPath))
	{
		SetAvidScriptReportLoadFailure(
			NormalizedReportPath,
			TEXT("report_missing"),
			FString::Printf(TEXT("AvidScript frontend report does not exist: %s"), *NormalizedReportPath),
			OutResult);
		return false;
	}

	FString ReportJson;
	if (!FFileHelper::LoadFileToString(ReportJson, *NormalizedReportPath))
	{
		SetAvidScriptReportLoadFailure(
			NormalizedReportPath,
			TEXT("report_read_failed"),
			FString::Printf(TEXT("AvidScript frontend report could not be read: %s"), *NormalizedReportPath),
			OutResult);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ReportJson);
	if (!FJsonSerializer::Deserialize(JsonReader, RootObject) || !RootObject.IsValid())
	{
		SetAvidScriptReportLoadFailure(
			NormalizedReportPath,
			TEXT("report_invalid"),
			FString::Printf(TEXT("AvidScript frontend report is not valid JSON: %s"), *NormalizedReportPath),
			OutResult);
		return false;
	}

	OutReport.SchemaVersion = GetAvidScriptJsonIntField(RootObject, TEXT("schema_version"), 0);
	if (OutReport.SchemaVersion <= 0)
	{
		SetAvidScriptReportLoadFailure(
			NormalizedReportPath,
			TEXT("report_invalid"),
			FString::Printf(TEXT("AvidScript frontend report is missing schema_version: %s"), *NormalizedReportPath),
			OutResult);
		OutReport = FAvidScriptFrontendReport();
		return false;
	}

	RootObject->TryGetStringField(TEXT("result"), OutReport.Result);
	LoadAvidScriptSource(RootObject, OutReport);
	LoadAvidScriptFrontendMetadata(RootObject, OutReport);
	LoadAvidScriptSemanticMetadata(RootObject, OutReport);
	LoadAvidScriptGuestIrMetadata(RootObject, OutReport);
	LoadAvidScriptBindingPackageMetadata(RootObject, OutReport);
	RootObject->TryGetStringField(TEXT("bindings"), OutReport.Bindings);
	RootObject->TryGetStringField(TEXT("output_root"), OutReport.OutputRoot);
	OutReport.ExitCode = GetAvidScriptJsonIntField(RootObject, TEXT("exit_code"), 0);
	RootObject->TryGetBoolField(TEXT("succeeded"), OutReport.bSucceeded);

	LoadAvidScriptDiagnostics(RootObject, OutReport.Diagnostics);
	LoadAvidScriptBuildEvents(RootObject, OutReport.BuildEvents);
	LoadAvidScriptRawOutput(RootObject, OutReport.RawOutput);

	SetAvidScriptReportLoadSuccess(NormalizedReportPath, OutResult);
	return true;
}