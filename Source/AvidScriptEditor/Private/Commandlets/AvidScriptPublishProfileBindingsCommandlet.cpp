#include "Commandlets/AvidScriptPublishProfileBindingsCommandlet.h"

#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptProfileBindingsCommandlet, Log, All);

UAvidScriptPublishProfileBindingsCommandlet::UAvidScriptPublishProfileBindingsCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UAvidScriptPublishProfileBindingsCommandlet::Main(const FString& Params)
{
	FString ProfilePath;
	FString OutputRoot;
	FString ReportPath;
	if (!FParse::Value(*Params, TEXT("Profile="), ProfilePath) || ProfilePath.IsEmpty()
		|| !FParse::Value(*Params, TEXT("Report="), ReportPath) || ReportPath.IsEmpty())
	{
		UE_LOG(LogAvidScriptProfileBindingsCommandlet, Error,
			TEXT("Expected -Profile=<C# profile> -Report=<new JSON file> [-OutputRoot=<binding output root>]."));
		return 1;
	}
	FParse::Value(*Params, TEXT("OutputRoot="), OutputRoot);
	ReportPath = FPaths::ConvertRelativePathToFull(ReportPath);
	if (IFileManager::Get().FileExists(*ReportPath))
	{
		UE_LOG(LogAvidScriptProfileBindingsCommandlet, Error, TEXT("Report already exists; use a unique report path."));
		return 1;
	}
	if (OutputRoot.IsEmpty())
	{
		OutputRoot = FAvidScriptEditorCSharpBindingEmitter::GetDefaultOutputRoot();
	}

	FAvidScriptEditorCSharpProfileLoadResult Profile;
	FAvidScriptCSharpBindingEmitResult Package;
	bool bSucceeded = FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, Profile);
	FString ErrorCategory = Profile.ErrorCategory;
	FString ErrorMessage = Profile.ErrorMessage;
	if (bSucceeded)
	{
		bSucceeded = Profile.bUsesEngineGameplayBindingProfile
			? FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(OutputRoot, Package)
			: FAvidScriptEditorCSharpBindingEmitter::PublishProfile(
				Profile.ResolvedBindingSelection, Profile.ResolvedClassReferences,
				Profile.ResolvedObjectFactories, OutputRoot, Package);
		ErrorCategory = Package.ErrorCategory;
		ErrorMessage = Package.ErrorMessage;
	}

	const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetNumberField(TEXT("schema_version"), 1);
	Report->SetStringField(TEXT("result"), bSucceeded
		? TEXT("avidscript_profile_bindings_published") : TEXT("avidscript_profile_bindings_failed"));
	Report->SetStringField(TEXT("status"), bSucceeded ? TEXT("ok") : TEXT("error"));
	Report->SetStringField(TEXT("profile_path"), Profile.NormalizedProfilePath);
	Report->SetStringField(TEXT("package_name"), Package.PackageName);
	Report->SetStringField(TEXT("package_hash"), Package.PackageHash);
	Report->SetStringField(TEXT("manifest_path"), Package.ManifestPath);
	Report->SetStringField(TEXT("manifest_sha256"), Package.ManifestHash);
	Report->SetStringField(TEXT("reference_source_path"), Package.ReferenceSourcePath);
	Report->SetNumberField(TEXT("binding_count"), Package.BindingCount);
	Report->SetNumberField(TEXT("delegate_event_count"), Package.DelegateEventCount);
	Report->SetBoolField(TEXT("reused_existing_package"), Package.bReusedExistingPackage);
	Report->SetStringField(TEXT("error_category"), ErrorCategory);
	Report->SetStringField(TEXT("error_message"), ErrorMessage);
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	if (!FJsonSerializer::Serialize(Report, Writer)
		|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true)
		|| !FFileHelper::SaveStringToFile(Json, *ReportPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_NoReplaceExisting))
	{
		UE_LOG(LogAvidScriptProfileBindingsCommandlet, Error, TEXT("Could not write the unique profile binding report."));
		return 1;
	}
	UE_LOG(LogAvidScriptProfileBindingsCommandlet, Display, TEXT("AVIDSCRIPT_PROFILE_BINDINGS_RESULT %s"), *Json);
	return bSucceeded ? 0 : 1;
}
