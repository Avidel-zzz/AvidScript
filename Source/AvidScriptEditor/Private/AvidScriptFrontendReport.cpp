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
		Diagnostic.Line = GetAvidScriptJsonIntField(DiagnosticObject, TEXT("line"), 0);
		Diagnostic.Column = GetAvidScriptJsonIntField(DiagnosticObject, TEXT("column"), 0);
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

	RootObject->TryGetStringField(TEXT("source"), OutReport.Source);
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