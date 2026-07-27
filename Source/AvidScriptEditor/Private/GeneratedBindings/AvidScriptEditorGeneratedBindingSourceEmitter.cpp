#include "GeneratedBindings/AvidScriptEditorGeneratedBindingSourceEmitter.h"

#include "Algo/Unique.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr const TCHAR* GeneratedModuleName =
	TEXT("AvidScriptGeneratedBindings");

void SetEmitterFailure(
	FAvidScriptEditorGeneratedBindingResult& OutResult,
	const FString& Category,
	const FString& Source)
{
	OutResult = FAvidScriptEditorGeneratedBindingResult();
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("Generated source emission failed | category=%s | source=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source);
}

bool IsLowerHexSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character)
			&& (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool IsSafeIdentifier(const FString& Value)
{
	if (Value.IsEmpty()
		|| (!FChar::IsAlpha(Value[0]) && Value[0] != TEXT('_')))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

bool IsSafeHeader(const FString& Value)
{
	if (Value.IsEmpty()
		|| Value.StartsWith(TEXT("/"))
		|| Value.StartsWith(TEXT("\\"))
		|| Value.Contains(TEXT(".."))
		|| Value.Contains(TEXT("\\"))
		|| Value.Contains(TEXT(":"))
		|| !Value.EndsWith(TEXT(".h"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character)
			&& Character != TEXT('_')
			&& Character != TEXT('-')
			&& Character != TEXT('/')
			&& Character != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

FString EscapeCppString(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	return Value;
}

FString EscapeJsonString(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return Value;
}

const TCHAR* ShapeToken(const EAvidScriptGeneratedBindingShape Shape)
{
	switch (Shape)
	{
	case EAvidScriptGeneratedBindingShape::I32PairToI32:
		return TEXT("EAvidScriptGeneratedBindingShape::I32PairToI32");
	case EAvidScriptGeneratedBindingShape::PropertyI32GetSet:
		return TEXT("EAvidScriptGeneratedBindingShape::PropertyI32GetSet");
	case EAvidScriptGeneratedBindingShape::VectorValue:
		return TEXT("EAvidScriptGeneratedBindingShape::VectorValue");
	case EAvidScriptGeneratedBindingShape::StableObjectRoundtrip:
		return TEXT("EAvidScriptGeneratedBindingShape::StableObjectRoundtrip");
	default:
		return TEXT("EAvidScriptGeneratedBindingShape::I32PairToI32");
	}
}

const TCHAR* ReceiverToken(const EAvidScriptGeneratedReceiverMode Mode)
{
	return Mode == EAvidScriptGeneratedReceiverMode::StableBorrow
		? TEXT("EAvidScriptGeneratedReceiverMode::StableBorrow")
		: TEXT("EAvidScriptGeneratedReceiverMode::SelfBound");
}

const TCHAR* ShapeManifestToken(
	const EAvidScriptGeneratedBindingShape Shape)
{
	switch (Shape)
	{
	case EAvidScriptGeneratedBindingShape::I32PairToI32:
		return TEXT("i32_pair_to_i32");
	case EAvidScriptGeneratedBindingShape::PropertyI32GetSet:
		return TEXT("property_i32_get_set");
	case EAvidScriptGeneratedBindingShape::VectorValue:
		return TEXT("vector_value");
	case EAvidScriptGeneratedBindingShape::StableObjectRoundtrip:
		return TEXT("stable_object_roundtrip");
	default:
		return TEXT("invalid");
	}
}

FString RenderBuildCs(const TArray<FString>& OwnerModules)
{
	TArray<FString> Dependencies = {
		TEXT("Core"),
		TEXT("CoreUObject"),
		TEXT("AvidScriptBindings"),
		TEXT("AvidScriptVM")
	};
	Dependencies.Append(OwnerModules);
	Dependencies.Sort();
	Dependencies.SetNum(Algo::Unique(Dependencies));

	TArray<FString> QuotedDependencies;
	for (const FString& Dependency : Dependencies)
	{
		QuotedDependencies.Add(
			TEXT("\"") + EscapeCppString(Dependency) + TEXT("\""));
	}
	return
		TEXT("using UnrealBuildTool;\n\n")
		TEXT("public class AvidScriptGeneratedBindings : ModuleRules\n")
		TEXT("{\n")
		TEXT("\tpublic AvidScriptGeneratedBindings(ReadOnlyTargetRules Target) : base(Target)\n")
		TEXT("\t{\n")
		TEXT("\t\tPCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;\n")
		TEXT("\t\tPublicDependencyModuleNames.AddRange(new string[] { ")
		+ FString::Join(QuotedDependencies, TEXT(", "))
		+ TEXT(" });\n")
		TEXT("\t}\n")
		TEXT("}\n");
}

FString RenderPublicHeader()
{
	return
		TEXT("#pragma once\n\n")
		TEXT("#include \"Modules/ModuleInterface.h\"\n\n")
		TEXT("class FAvidScriptGeneratedBindingsModule final : public IModuleInterface\n")
		TEXT("{\n")
		TEXT("public:\n")
		TEXT("\tvirtual void StartupModule() override;\n")
		TEXT("\tvirtual void ShutdownModule() override;\n")
		TEXT("};\n");
}

FString RenderTypedCallSite(
	const FAvidScriptGeneratedBindingIr& Binding,
	const int32 Index)
{
	const FString FunctionName =
		FString::Printf(TEXT("InvokeGenerated_%04d"), Index);
	const FString OwnerType = Binding.OwnerCppType;
	const FString UeFunction = Binding.FunctionName;
	switch (Binding.Shape)
	{
	case EAvidScriptGeneratedBindingShape::I32PairToI32:
		return FString::Printf(
			TEXT("static EAvidScriptVmTypedHostStatus %s(UObject& Receiver, int32 Left, int32 Right, int32& OutValue)\n")
			TEXT("{\n")
			TEXT("\t%s* TypedReceiver = Cast<%s>(&Receiver);\n")
			TEXT("\tif (TypedReceiver == nullptr)\n")
			TEXT("\t{\n")
			TEXT("\t\treturn EAvidScriptVmTypedHostStatus::Rejected;\n")
			TEXT("\t}\n")
			TEXT("\tOutValue = TypedReceiver->%s(Left, Right);\n")
			TEXT("\treturn EAvidScriptVmTypedHostStatus::Succeeded;\n")
			TEXT("}\n\n"),
			*FunctionName,
			*OwnerType,
			*OwnerType,
			*UeFunction);
	case EAvidScriptGeneratedBindingShape::VectorValue:
		return FString::Printf(
			TEXT("static EAvidScriptVmTypedHostStatus %s(UObject& Receiver, const FVector& InValue, FVector& OutValue)\n")
			TEXT("{\n")
			TEXT("\t%s* TypedReceiver = Cast<%s>(&Receiver);\n")
			TEXT("\tif (TypedReceiver == nullptr)\n")
			TEXT("\t{\n")
			TEXT("\t\treturn EAvidScriptVmTypedHostStatus::Rejected;\n")
			TEXT("\t}\n")
			TEXT("\tOutValue = TypedReceiver->%s(InValue);\n")
			TEXT("\treturn EAvidScriptVmTypedHostStatus::Succeeded;\n")
			TEXT("}\n\n"),
			*FunctionName,
			*OwnerType,
			*OwnerType,
			*UeFunction);
	case EAvidScriptGeneratedBindingShape::StableObjectRoundtrip:
		return FString::Printf(
			TEXT("static EAvidScriptVmTypedHostStatus %s(UObject& Receiver, UObject* InValue, UObject*& OutValue)\n")
			TEXT("{\n")
			TEXT("\t%s* TypedReceiver = Cast<%s>(&Receiver);\n")
			TEXT("\tif (TypedReceiver == nullptr)\n")
			TEXT("\t{\n")
			TEXT("\t\treturn EAvidScriptVmTypedHostStatus::Rejected;\n")
			TEXT("\t}\n")
			TEXT("\tOutValue = TypedReceiver->%s(InValue);\n")
			TEXT("\treturn EAvidScriptVmTypedHostStatus::Succeeded;\n")
			TEXT("}\n\n"),
			*FunctionName,
			*OwnerType,
			*OwnerType,
			*UeFunction);
	case EAvidScriptGeneratedBindingShape::PropertyI32GetSet:
		return FString::Printf(
			TEXT("static EAvidScriptVmTypedHostStatus %s(UObject& Receiver, bool bWrite, int32& InOutValue)\n")
			TEXT("{\n")
			TEXT("\t%s* TypedReceiver = Cast<%s>(&Receiver);\n")
			TEXT("\tif (TypedReceiver == nullptr)\n")
			TEXT("\t{\n")
			TEXT("\t\treturn EAvidScriptVmTypedHostStatus::Rejected;\n")
			TEXT("\t}\n")
			TEXT("\tif (bWrite)\n")
			TEXT("\t{\n")
			TEXT("\t\tTypedReceiver->%s = InOutValue;\n")
			TEXT("\t}\n")
			TEXT("\telse\n")
			TEXT("\t{\n")
			TEXT("\t\tInOutValue = TypedReceiver->%s;\n")
			TEXT("\t}\n")
			TEXT("\treturn EAvidScriptVmTypedHostStatus::Succeeded;\n")
			TEXT("}\n\n"),
			*FunctionName,
			*OwnerType,
			*OwnerType,
			*UeFunction,
			*UeFunction);
	default:
		return FString();
	}
}

FString RenderPrivateCpp(
	const FAvidScriptGeneratedBindingPackageIr& Package)
{
	TArray<FString> Headers;
	for (const FAvidScriptGeneratedBindingIr& Binding : Package.Bindings)
	{
		Headers.Add(Binding.OwnerHeader);
	}
	Headers.Sort();
	Headers.SetNum(Algo::Unique(Headers));

	FString Result =
		TEXT("#include \"AvidScriptGeneratedBindings.h\"\n\n")
		TEXT("#include \"AvidScriptGeneratedBindingRegistry.h\"\n")
		TEXT("#include \"AvidScriptVmTypedHostImport.h\"\n")
		TEXT("#include \"Modules/ModuleManager.h\"\n");
	for (const FString& Header : Headers)
	{
		Result += TEXT("#include \"") + Header + TEXT("\"\n");
	}
	Result += TEXT("\nnamespace\n{\n");
	for (int32 Index = 0; Index < Package.Bindings.Num(); ++Index)
	{
		Result += RenderTypedCallSite(Package.Bindings[Index], Index);
	}
	Result += TEXT("const FAvidScriptGeneratedBindingEntry GeneratedEntries[] =\n{\n");
	for (int32 Index = 0; Index < Package.Bindings.Num(); ++Index)
	{
		const FAvidScriptGeneratedBindingIr& Binding = Package.Bindings[Index];
		const FString CallSite =
			FString::Printf(TEXT("&InvokeGenerated_%04d"), Index);
		const FString I32PairCall = Binding.Shape
				== EAvidScriptGeneratedBindingShape::I32PairToI32
			? CallSite
			: FString(TEXT("nullptr"));
		const FString PropertyI32Call = Binding.Shape
				== EAvidScriptGeneratedBindingShape::PropertyI32GetSet
			? CallSite
			: FString(TEXT("nullptr"));
		const FString VectorValueCall = Binding.Shape
				== EAvidScriptGeneratedBindingShape::VectorValue
			? CallSite
			: FString(TEXT("nullptr"));
		const FString StableObjectCall = Binding.Shape
				== EAvidScriptGeneratedBindingShape::StableObjectRoundtrip
			? CallSite
			: FString(TEXT("nullptr"));
		Result += FString::Printf(
			TEXT("\t{ TEXT(\"%s\"), TEXT(\"%s\"), TEXT(\"%s\"), %s, %s, %s, %s, %s, %s },\n"),
			*EscapeCppString(Binding.StableId),
			*EscapeCppString(Package.PackageHash),
			*EscapeCppString(Binding.DescriptorIdentity),
			ShapeToken(Binding.Shape),
			ReceiverToken(Binding.ReceiverMode),
			*I32PairCall,
			*PropertyI32Call,
			*VectorValueCall,
			*StableObjectCall);
	}
	Result += TEXT("};\n} // namespace\n\n");
	Result +=
		TEXT("void FAvidScriptGeneratedBindingsModule::StartupModule()\n")
		TEXT("{\n")
		TEXT("\tFString Error;\n")
		TEXT("\tFAvidScriptGeneratedBindingRegistry::Get().RegisterPackage(\n")
		TEXT("\t\tTEXT(\"") + EscapeCppString(Package.PackageHash) + TEXT("\"),\n")
		TEXT("\t\tMakeArrayView(GeneratedEntries),\n")
		TEXT("\t\tError);\n")
		TEXT("}\n\n")
		TEXT("void FAvidScriptGeneratedBindingsModule::ShutdownModule()\n")
		TEXT("{\n")
		TEXT("\tFAvidScriptGeneratedBindingRegistry::Get().UnregisterPackage(\n")
		TEXT("\t\tTEXT(\"") + EscapeCppString(Package.PackageHash) + TEXT("\"));\n")
		TEXT("}\n\n")
		TEXT("IMPLEMENT_MODULE(FAvidScriptGeneratedBindingsModule, AvidScriptGeneratedBindings)\n");
	return Result;
}

FString RenderManifest(
	const FAvidScriptGeneratedBindingPackageIr& Package)
{
	FString Result = FString::Printf(
		TEXT("{\n  \"schema_version\": %d,\n  \"package_name\": \"%s\",\n  \"package_hash\": \"%s\",\n  \"bindings\": [\n"),
		Package.SchemaVersion,
		*EscapeJsonString(Package.PackageName),
		*EscapeJsonString(Package.PackageHash));
	for (int32 Index = 0; Index < Package.Bindings.Num(); ++Index)
	{
		const FAvidScriptGeneratedBindingIr& Binding = Package.Bindings[Index];
		Result += FString::Printf(
			TEXT("    {\"stable_id\":\"%s\",\"owner_module\":\"%s\",\"owner_header\":\"%s\",\"owner_cpp_type\":\"%s\",\"member\":\"%s\",\"import_module\":\"%s\",\"import_name\":\"%s\",\"abi_signature\":\"%s\",\"shape\":\"%s\",\"receiver_mode\":\"%s\",\"descriptor_identity\":\"%s\"}%s\n"),
			*EscapeJsonString(Binding.StableId),
			*EscapeJsonString(Binding.OwnerModule),
			*EscapeJsonString(Binding.OwnerHeader),
			*EscapeJsonString(Binding.OwnerCppType),
			*EscapeJsonString(Binding.FunctionName),
			*EscapeJsonString(Binding.ImportModule),
			*EscapeJsonString(Binding.ImportName),
			*EscapeJsonString(Binding.AbiSignature),
			ShapeManifestToken(Binding.Shape),
			Binding.ReceiverMode == EAvidScriptGeneratedReceiverMode::StableBorrow
				? TEXT("stable_borrow")
				: TEXT("self_bound"),
			*EscapeJsonString(Binding.DescriptorIdentity),
			Index + 1 == Package.Bindings.Num() ? TEXT("") : TEXT(","));
	}
	Result += TEXT("  ]\n}\n");
	return Result;
}

bool ValidateJson(const FString& Json)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid();
}

bool AddGeneratedModuleToProject(
	const FString& Source,
	FString& OutUpdated)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Source);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (Root->TryGetArrayField(TEXT("Modules"), Modules)
		&& Modules != nullptr
		&& Modules->ContainsByPredicate([](
			const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Module =
				Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject()
				: nullptr;
			FString Name;
			return Module.IsValid()
				&& Module->TryGetStringField(TEXT("Name"), Name)
				&& Name == GeneratedModuleName;
		}))
	{
		OutUpdated = Source;
		return true;
	}

	const FString Descriptor =
		TEXT("{\"Name\":\"AvidScriptGeneratedBindings\",\"Type\":\"Runtime\",\"LoadingPhase\":\"Default\"}");
	const int32 ModulesField = Source.Find(
		TEXT("\"Modules\""),
		ESearchCase::CaseSensitive);
	if (ModulesField == INDEX_NONE)
	{
		const int32 RootEnd = Source.Find(
			TEXT("}"),
			ESearchCase::CaseSensitive,
			ESearchDir::FromEnd);
		if (RootEnd == INDEX_NONE)
		{
			return false;
		}
		int32 Previous = RootEnd - 1;
		while (Previous >= 0 && FChar::IsWhitespace(Source[Previous]))
		{
			--Previous;
		}
		const bool bNeedsComma = Previous >= 0 && Source[Previous] != TEXT('{');
		OutUpdated = Source.Left(RootEnd)
			+ (bNeedsComma ? TEXT(",") : TEXT(""))
			+ TEXT("\n  \"Modules\": [\n    ")
			+ Descriptor
			+ TEXT("\n  ]\n")
			+ Source.Mid(RootEnd);
		return ValidateJson(OutUpdated);
	}

	const int32 ArrayStart = Source.Find(
		TEXT("["),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		ModulesField);
	if (ArrayStart == INDEX_NONE)
	{
		return false;
	}
	bool bInString = false;
	bool bEscaped = false;
	int32 Depth = 0;
	int32 ArrayEnd = INDEX_NONE;
	for (int32 Index = ArrayStart; Index < Source.Len(); ++Index)
	{
		const TCHAR Character = Source[Index];
		if (bInString)
		{
			if (bEscaped)
			{
				bEscaped = false;
			}
			else if (Character == TEXT('\\'))
			{
				bEscaped = true;
			}
			else if (Character == TEXT('"'))
			{
				bInString = false;
			}
			continue;
		}
		if (Character == TEXT('"'))
		{
			bInString = true;
		}
		else if (Character == TEXT('['))
		{
			++Depth;
		}
		else if (Character == TEXT(']') && --Depth == 0)
		{
			ArrayEnd = Index;
			break;
		}
	}
	if (ArrayEnd == INDEX_NONE)
	{
		return false;
	}
	const FString Existing = Source.Mid(
		ArrayStart + 1,
		ArrayEnd - ArrayStart - 1);
	const FString Insertion = Existing.TrimStartAndEnd().IsEmpty()
		? TEXT("\n    ") + Descriptor + TEXT("\n  ")
		: TEXT(",\n    ") + Descriptor + TEXT("\n  ");
	OutUpdated = Source.Left(ArrayEnd) + Insertion + Source.Mid(ArrayEnd);
	return ValidateJson(OutUpdated);
}

bool SaveDeterministicFile(const FString& Path, const FString& Contents)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveStringToFile(
		Contents,
		*Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool HasGeneratedDependencyCycle(
	const FString& ProjectRoot,
	const TArray<FString>& OwnerModules,
	FString& OutSource)
{
	if (OwnerModules.Contains(GeneratedModuleName))
	{
		OutSource = GeneratedModuleName;
		return true;
	}
	for (const FString& OwnerModule : OwnerModules)
	{
		TArray<FString> BuildFiles;
		IFileManager::Get().FindFilesRecursive(
			BuildFiles,
			*(ProjectRoot / TEXT("Source")),
			*(OwnerModule + TEXT(".Build.cs")),
			true,
			false);
		for (const FString& BuildFile : BuildFiles)
		{
			FString BuildRules;
			if (FFileHelper::LoadFileToString(BuildRules, *BuildFile)
				&& BuildRules.Contains(
					GeneratedModuleName,
					ESearchCase::CaseSensitive))
			{
				OutSource = BuildFile;
				return true;
			}
		}
	}
	return false;
}
} // namespace

bool FAvidScriptEditorGeneratedBindingSourceEmitter::Emit(
	const FString& ProjectFile,
	const FAvidScriptGeneratedBindingPackageIr& Package,
	FAvidScriptEditorGeneratedBindingResult& OutResult)
{
	OutResult = FAvidScriptEditorGeneratedBindingResult();
	const FString FullProjectFile =
		FPaths::ConvertRelativePathToFull(ProjectFile);
	FString ProjectJson;
	if (!FullProjectFile.EndsWith(TEXT(".uproject"), ESearchCase::IgnoreCase)
		|| !FFileHelper::LoadFileToString(ProjectJson, *FullProjectFile)
		|| !ValidateJson(ProjectJson))
	{
		SetEmitterFailure(
			OutResult,
			TEXT("generated_project_descriptor_invalid"),
			FullProjectFile);
		return false;
	}
	if (Package.PackageName.IsEmpty()
		|| !IsLowerHexSha256(Package.PackageHash)
		|| Package.Bindings.IsEmpty())
	{
		SetEmitterFailure(
			OutResult,
			TEXT("generated_package_invalid"),
			Package.PackageName);
		return false;
	}

	TArray<FAvidScriptGeneratedBindingIr> Bindings = Package.Bindings;
	Bindings.Sort([](
		const FAvidScriptGeneratedBindingIr& Left,
		const FAvidScriptGeneratedBindingIr& Right)
	{
		return Left.StableId.Compare(
			Right.StableId,
			ESearchCase::CaseSensitive) < 0;
	});
	FAvidScriptGeneratedBindingPackageIr SortedPackage = Package;
	SortedPackage.Bindings = MoveTemp(Bindings);
	for (int32 Index = 0; Index < SortedPackage.Bindings.Num(); ++Index)
	{
		const FAvidScriptGeneratedBindingIr& Binding =
			SortedPackage.Bindings[Index];
		if (!IsLowerHexSha256(Binding.StableId)
			|| !IsSafeIdentifier(Binding.OwnerModule)
			|| !IsSafeHeader(Binding.OwnerHeader)
			|| !IsSafeIdentifier(Binding.OwnerCppType)
			|| !IsSafeIdentifier(Binding.FunctionName)
			|| Binding.ImportModule != TEXT("avidscript")
			|| !Binding.ImportName.StartsWith(
				TEXT("avid_s1_"),
				ESearchCase::CaseSensitive)
			|| Binding.ImportName.Len() != 24
			|| Binding.AbiSignature.IsEmpty()
			|| Binding.DescriptorIdentity.IsEmpty()
			|| (Index > 0
				&& Binding.StableId
					== SortedPackage.Bindings[Index - 1].StableId))
		{
			SetEmitterFailure(
				OutResult,
				TEXT("generated_binding_ir_invalid"),
				Binding.StableId);
			return false;
		}
	}

	TArray<FString> OwnerModules;
	for (const FAvidScriptGeneratedBindingIr& Binding : SortedPackage.Bindings)
	{
		OwnerModules.Add(Binding.OwnerModule);
	}
	OwnerModules.Sort();
	OwnerModules.SetNum(Algo::Unique(OwnerModules));

	const FString ProjectRoot = FPaths::GetPath(FullProjectFile);
	FString CycleSource;
	if (HasGeneratedDependencyCycle(ProjectRoot, OwnerModules, CycleSource))
	{
		SetEmitterFailure(
			OutResult,
			TEXT("generated_module_dependency_cycle"),
			CycleSource);
		return false;
	}

	FString UpdatedProjectJson;
	if (!AddGeneratedModuleToProject(ProjectJson, UpdatedProjectJson))
	{
		SetEmitterFailure(
			OutResult,
			TEXT("generated_project_module_update_failed"),
			FullProjectFile);
		return false;
	}

	const FString OutputDirectory =
		ProjectRoot / TEXT("Source") / GeneratedModuleName;
	const FString TransactionId = FGuid::NewGuid().ToString(
		EGuidFormats::Digits);
	const FString StageDirectory =
		ProjectRoot / TEXT("Source")
		/ (TEXT(".AvidScriptGeneratedBindings.stage-") + TransactionId);
	const FString BackupDirectory =
		OutputDirectory + TEXT(".backup-") + TransactionId;
	const FString ProjectStage =
		FullProjectFile + TEXT(".stage-") + TransactionId;
	const FString ProjectBackup =
		FullProjectFile + TEXT(".backup-") + TransactionId;

	const bool bStageWritten =
		SaveDeterministicFile(
			StageDirectory / TEXT("AvidScriptGeneratedBindings.Build.cs"),
			RenderBuildCs(OwnerModules))
		&& SaveDeterministicFile(
			StageDirectory / TEXT("Public")
				/ TEXT("AvidScriptGeneratedBindings.h"),
			RenderPublicHeader())
		&& SaveDeterministicFile(
			StageDirectory / TEXT("Private")
				/ TEXT("AvidScriptGeneratedBindings.cpp"),
			RenderPrivateCpp(SortedPackage))
		&& SaveDeterministicFile(
			StageDirectory / TEXT("Private")
				/ TEXT("generated-bindings.manifest.json"),
			RenderManifest(SortedPackage))
		&& SaveDeterministicFile(ProjectStage, UpdatedProjectJson);
	if (!bStageWritten)
	{
		IFileManager::Get().DeleteDirectory(*StageDirectory, false, true);
		IFileManager::Get().Delete(*ProjectStage, false, true);
		SetEmitterFailure(
			OutResult,
			TEXT("generated_stage_write_failed"),
			StageDirectory);
		return false;
	}

	const bool bHadOutput = IFileManager::Get().DirectoryExists(
		*OutputDirectory);
	IPlatformFile& PlatformFile =
		FPlatformFileManager::Get().GetPlatformFile();
	if ((bHadOutput
			&& !PlatformFile.MoveDirectory(
				*BackupDirectory,
				*OutputDirectory))
		|| !PlatformFile.MoveDirectory(
			*OutputDirectory,
			*StageDirectory))
	{
		IFileManager::Get().DeleteDirectory(*StageDirectory, false, true);
		if (bHadOutput
			&& PlatformFile.DirectoryExists(*BackupDirectory))
		{
			PlatformFile.MoveDirectory(
				*OutputDirectory,
				*BackupDirectory);
		}
		SetEmitterFailure(
			OutResult,
			TEXT("generated_module_replace_failed"),
			OutputDirectory);
		return false;
	}

	const bool bProjectReplaced =
		IFileManager::Get().Move(
			*ProjectBackup,
			*FullProjectFile,
			true,
			true)
		&& IFileManager::Get().Move(
			*FullProjectFile,
			*ProjectStage,
			true,
			true);
	if (!bProjectReplaced)
	{
		IFileManager::Get().DeleteDirectory(*OutputDirectory, false, true);
		if (bHadOutput)
		{
			PlatformFile.MoveDirectory(
				*OutputDirectory,
				*BackupDirectory);
		}
		if (IFileManager::Get().FileExists(*ProjectBackup))
		{
			IFileManager::Get().Move(
				*FullProjectFile,
				*ProjectBackup,
				true,
				true);
		}
		IFileManager::Get().Delete(*ProjectStage, false, true);
		SetEmitterFailure(
			OutResult,
			TEXT("generated_project_replace_failed"),
			FullProjectFile);
		return false;
	}

	IFileManager::Get().DeleteDirectory(*BackupDirectory, false, true);
	IFileManager::Get().Delete(*ProjectBackup, false, true);
	OutResult.bSucceeded = true;
	OutResult.BindingCount = SortedPackage.Bindings.Num();
	OutResult.PackageHash = SortedPackage.PackageHash;
	OutResult.OutputDirectory = OutputDirectory;
	return true;
}
