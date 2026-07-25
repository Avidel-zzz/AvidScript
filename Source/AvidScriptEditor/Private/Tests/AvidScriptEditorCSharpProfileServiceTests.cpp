#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "BindingGeneration/AvidScriptEditorCSharpBindingArtifact.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildPipeline.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString NormalizeAvidScriptCSharpProfileTestPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString GetAvidScriptCSharpProfileServiceTestRoot()
{
	return NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptEditorTests"),
		TEXT("CSharpProfileService")));
}

FString MakeAvidScriptCSharpProfileSourceText()
{
	return TEXT(
		"using AvidScript;\n"
		"\n"
		"public static class ProfileDrivenMover\n"
		"{\n"
		"    public static void BeginPlay()\n"
		"    {\n"
		"        Actor.SetLocation(31.0f, 32.0f, 33.0f);\n"
		"    }\n"
		"\n"
		"    public static void Tick(float deltaSeconds)\n"
		"    {\n"
		"        Actor.SetLocation(deltaSeconds * 8.0f, 9.0f, 10.0f);\n"
		"    }\n"
		"}\n");
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpProfileServiceMissingProfileTest,
	"AvidScript.Editor.CSharpProfileService.MissingProfileFailsSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpProfileServiceMissingProfileTest::RunTest(const FString& Parameters)
{
	const FString MissingProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		GetAvidScriptCSharpProfileServiceTestRoot(),
		TEXT("Missing"),
		TEXT("missing.csharp-profile.json")));

	FAvidScriptEditorCSharpProfileLoadResult Result;
	TestFalse(TEXT("Missing C# profile fails"), FAvidScriptEditorCSharpProfileService::LoadProfile(MissingProfilePath, Result));
	TestFalse(TEXT("Missing C# profile result fails"), Result.bSucceeded);
	TestEqual(TEXT("Missing profile category"), Result.ErrorCategory, FString(TEXT("profile_missing")));
	TestEqual(TEXT("Missing profile path normalized"), Result.NormalizedProfilePath, MissingProfilePath);
	TestFalse(TEXT("Missing profile next action is present"), Result.NextAction.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpProfileServiceLoadProfileTest,
	"AvidScript.Editor.CSharpProfileService.LoadProfileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpProfileServiceLoadProfileTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		GetAvidScriptCSharpProfileServiceTestRoot(),
		TEXT("ProfileDriven")));
	TestTrue(TEXT("C# profile test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString SourcePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(TestRoot, TEXT("ProfileDrivenMover.cs")));
	TestTrue(TEXT("C# profile source can be written"), FFileHelper::SaveStringToFile(MakeAvidScriptCSharpProfileSourceText(), *SourcePath));

	const FString OutputRoot = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ProfileDriven")));
	const FString ProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(TestRoot, TEXT("profile_driven.csharp-profile.json")));
	const FString ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	const FString BindingPackagePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(TestRoot, TEXT("bindings/package.json")));
	const FString ProfileText = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"language\": \"csharp\",\n")
		TEXT("  \"source_path\": \"%s\",\n")
		TEXT("  \"project_path\": \"%s\",\n")
		TEXT("  \"binding_package_path\": \"%s\",\n")
		TEXT("  \"module_id\": \"csharp_profile_driven\",\n")
		TEXT("  \"artifact_stem\": \"profile_driven\",\n")
		TEXT("  \"output_root\": \"%s\",\n")
		TEXT("  \"configuration\": \"Release\"\n")
		TEXT("}\n"),
		*SourcePath,
		*ProjectPath,
		*BindingPackagePath,
		*OutputRoot);
	TestTrue(TEXT("C# profile can be written"), FFileHelper::SaveStringToFile(ProfileText, *ProfilePath));

	FAvidScriptEditorCSharpProfileLoadResult Result;
	TestTrue(TEXT("C# profile loads"), FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, Result));
	TestTrue(TEXT("C# profile load result succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Legacy profile retains schema version"), Result.SchemaVersion, 1);
	TestTrue(TEXT("Legacy profile defaults to EngineGameplay"), Result.bUsesEngineGameplayBindingProfile);
	TestEqual(TEXT("Legacy profile resolves six gameplay classes"), Result.ResolvedBindingSelection.Classes.Num(), 6);
	TestEqual(TEXT("C# profile path normalized"), Result.NormalizedProfilePath, ProfilePath);
	TestEqual(TEXT("C# profile source"), Result.BuildConfig.SourcePath, SourcePath);
	TestEqual(TEXT("C# profile project"), Result.BuildConfig.ProjectPath, ProjectPath);
	TestEqual(TEXT("C# profile binding package"), Result.BuildConfig.BindingPackagePath, BindingPackagePath);
	TestEqual(TEXT("C# profile module id"), Result.BuildConfig.ModuleId, FString(TEXT("csharp_profile_driven")));
	TestEqual(TEXT("C# profile artifact stem"), Result.BuildConfig.ArtifactStem, FString(TEXT("profile_driven")));
	TestEqual(TEXT("C# profile output root"), Result.BuildConfig.OutputRoot, OutputRoot);
	TestEqual(TEXT("C# profile report default"), Result.BuildConfig.ReportPath, FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(OutputRoot, TEXT("profile_driven")));
	TestEqual(TEXT("C# profile manifest default"), Result.BuildConfig.ManifestPath, FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(OutputRoot, TEXT("profile_driven")));
	TestEqual(TEXT("C# profile build script default"), Result.BuildConfig.BuildScriptPath, FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath());
	TestEqual(TEXT("C# profile configuration"), Result.BuildConfig.Configuration, FString(TEXT("Release")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpProfileServiceDefaultTemplateTest,
	"AvidScript.Editor.CSharpProfileService.DefaultTemplateSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpProfileServiceDefaultTemplateTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		GetAvidScriptCSharpProfileServiceTestRoot(),
		TEXT("DefaultTemplate")));
	TestTrue(TEXT("C# profile template test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString TemplatePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(TestRoot, TEXT("default_template.csharp-profile.json")));
	IFileManager::Get().Delete(*TemplatePath, false, true, true);

	FAvidScriptEditorCSharpProfileTemplateResult TemplateResult;
	TestTrue(TEXT("Default C# profile template writes"), FAvidScriptEditorCSharpProfileService::WriteProfileTemplate(TemplatePath, TemplateResult, false));
	TestTrue(TEXT("Default C# profile template result succeeds"), TemplateResult.bSucceeded);
	TestTrue(TEXT("Default C# profile template is created"), TemplateResult.bCreated);
	TestEqual(TEXT("Default C# profile template path"), TemplateResult.NormalizedProfilePath, TemplatePath);
	TestEqual(TEXT("Default C# profile template source"), TemplateResult.SourcePath, FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath());
	TestEqual(TEXT("Default C# profile template module id"), TemplateResult.ModuleId, FString(TEXT("csharp_profile_actor_lifecycle")));
	TestEqual(TEXT("Default C# profile template artifact stem"), TemplateResult.ArtifactStem, FString(TEXT("profile_actor_lifecycle")));
	TestTrue(TEXT("Default C# profile template file exists"), FPaths::FileExists(TemplatePath));

	FAvidScriptEditorCSharpProfileLoadResult LoadResult;
	TestTrue(TEXT("Generated default C# profile loads"), FAvidScriptEditorCSharpProfileService::LoadProfile(TemplatePath, LoadResult));
	TestTrue(TEXT("Generated default C# profile load succeeds"), LoadResult.bSucceeded);
	TestEqual(TEXT("Generated template uses schema version 2"), LoadResult.SchemaVersion, 2);
	TestTrue(TEXT("Generated template defaults to EngineGameplay"), LoadResult.bUsesEngineGameplayBindingProfile);
	TestEqual(TEXT("Generated default C# profile source"), LoadResult.BuildConfig.SourcePath, FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath());
	TestEqual(TEXT("Generated default C# profile project"), LoadResult.BuildConfig.ProjectPath, FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath());
	TestEqual(TEXT("Generated default C# profile module"), LoadResult.BuildConfig.ModuleId, FString(TEXT("csharp_profile_actor_lifecycle")));
	FString TemplateText;
	if (TestTrue(
		TEXT("Generated default C# profile text can be read"),
		FFileHelper::LoadFileToString(TemplateText, *TemplatePath)))
	{
		TestFalse(
			TEXT("Schema v2 template omits project absolute paths"),
			TemplateText.Contains(
				NormalizeAvidScriptCSharpProfileTestPath(FPaths::ProjectDir()),
				ESearchCase::IgnoreCase));
	}
	TestEqual(TEXT("Generated default C# profile artifact"), LoadResult.BuildConfig.ArtifactStem, FString(TEXT("profile_actor_lifecycle")));
	TestTrue(TEXT("Generated default C# profile output root uses profile folder"), LoadResult.BuildConfig.OutputRoot.EndsWith(TEXT("Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle")));

	FString OriginalProfileText;
	TestTrue(TEXT("Generated default C# profile can be read back"), FFileHelper::LoadFileToString(OriginalProfileText, *TemplatePath));

	FAvidScriptEditorCSharpProfileTemplateResult ExistingResult;
	TestTrue(TEXT("Existing default C# profile template is accepted"), FAvidScriptEditorCSharpProfileService::WriteProfileTemplate(TemplatePath, ExistingResult, false));
	TestTrue(TEXT("Existing default C# profile template result succeeds"), ExistingResult.bSucceeded);
	TestFalse(TEXT("Existing default C# profile template is not overwritten"), ExistingResult.bCreated);

	FString ExistingProfileText;
	TestTrue(TEXT("Existing default C# profile can be read back"), FFileHelper::LoadFileToString(ExistingProfileText, *TemplatePath));
	TestEqual(TEXT("Existing default C# profile text is unchanged"), ExistingProfileText, OriginalProfileText);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpProfileServiceProjectBindingProfileTest,
	"AvidScript.Editor.CSharpProfileService.ProjectBindingProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpProfileServiceProjectBindingProfileTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		GetAvidScriptCSharpProfileServiceTestRoot(),
		TEXT("ProjectBindingProfile")));
	TestTrue(TEXT("Project binding profile test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString SourcePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(TestRoot, TEXT("ProjectBinding.cs")));
	TestTrue(TEXT("Project binding C# source can be written"), FFileHelper::SaveStringToFile(MakeAvidScriptCSharpProfileSourceText(), *SourcePath));
	const FString ProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		TestRoot,
		TEXT("project_binding.csharp-profile.json")));
	const FString ProfileText = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 2,\n")
		TEXT("  \"language\": \"csharp\",\n")
		TEXT("  \"source_path\": \"%s\",\n")
		TEXT("  \"binding_profile\": {\n")
		TEXT("    \"package_name\": \"avidscript.project.profile_service\",\n")
		TEXT("    \"classes\": [\n")
		TEXT("      {\n")
		TEXT("        \"class_path\": \"/Script/Engine.Actor\",\n")
		TEXT("        \"include_functions\": [\"SetActorScale3D\", \"K2_GetActorLocation\"],\n")
		TEXT("        \"include_properties\": [\"CustomTimeDilation\"]\n")
		TEXT("      }\n")
		TEXT("    ],\n")
		TEXT("    \"class_references\": [\n")
		TEXT("      {\n")
		TEXT("        \"script_name\": \"ProjectileClass\",\n")
		TEXT("        \"class_path\": \"/Script/Engine.StaticMeshActor\",\n")
		TEXT("        \"base_class_path\": \"/Script/Engine.Actor\",\n")
		TEXT("        \"load_policy\": \"EditorLoad\"\n")
		TEXT("      }\n")
		TEXT("    ]\n")
		TEXT("  }\n")
		TEXT("}\n"),
		*SourcePath);
	TestTrue(TEXT("Project binding C# profile can be written"), FFileHelper::SaveStringToFile(ProfileText, *ProfilePath));

	FAvidScriptEditorCSharpProfileLoadResult Result;
	TestTrue(TEXT("Project binding C# profile loads"), FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, Result));
	TestTrue(TEXT("Project binding profile load succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Project binding profile schema is 2"), Result.SchemaVersion, 2);
	TestFalse(TEXT("Project binding profile does not use EngineGameplay"), Result.bUsesEngineGameplayBindingProfile);
	TestEqual(TEXT("Project binding package name is retained"), Result.ProjectBindingProfile.PackageName, FString(TEXT("avidscript.project.profile_service")));
	TestEqual(TEXT("Project binding resolves one class"), Result.ResolvedBindingSelection.Classes.Num(), 1);
	TestEqual(TEXT("Project binding resolves one class reference"), Result.ResolvedClassReferences.Num(), 1);
	if (Result.ResolvedBindingSelection.Classes.Num() == 1
		&& Result.ResolvedBindingSelection.Classes[0].IncludeFunctions.Num() == 2)
	{
		TestEqual(
			TEXT("Project binding normalizes function order"),
			Result.ResolvedBindingSelection.Classes[0].IncludeFunctions[0],
			FName(TEXT("K2_GetActorLocation")));
	}
	TestEqual(TEXT("Project binding hash is SHA-256"), Result.BindingSelectionHash.Len(), 64);

	FAvidScriptEditorCSharpBuildPlan FirstPlan;
	FAvidScriptEditorCSharpBuildResult FirstPrepareResult;
	const bool bFirstPrepared = FAvidScriptEditorCSharpBuildPipeline::Prepare(
		FAvidScriptEditorCSharpProfileService::MakeBuildRequest(Result),
		FirstPlan,
		FirstPrepareResult);
	TestTrue(TEXT("Project binding profile enters the build pipeline"), bFirstPrepared);
	if (!bFirstPrepared)
	{
		FAvidScriptEditorCSharpBuildPipeline::Cleanup(FirstPlan);
		return false;
	}
	TestTrue(TEXT("Project binding plan uses automatic authorization generation"), FirstPlan.bAutomaticBindingSlice);
	TestEqual(TEXT("Project class references reach the build plan"), FirstPlan.AuthorizationClassReferences.Num(), 1);
	TestEqual(TEXT("Project binding selection hash reaches the build result"), FirstPrepareResult.BindingSelectionHash, Result.BindingSelectionHash);
	TestTrue(TEXT("Project package name reaches the generated manifest path"), FirstPlan.AuthorizationBindingPackagePath.Contains(Result.ProjectBindingProfile.PackageName));
	TestTrue(TEXT("Project authorization manifest exists"), FPaths::FileExists(FirstPlan.AuthorizationBindingPackagePath));
	const FString FirstManifestPath = FirstPlan.AuthorizationBindingPackagePath;
	const FString PackageDirectory = FPaths::GetPath(FirstManifestPath);
	FString PublishedDescriptorJson;
	FAvidScriptBindingPackageModel PublishedPackage;
	FString PublishedParseCategory;
	FString PublishedParseSource;
	TestTrue(
		TEXT("Project authorization descriptor retains the resolved class table"),
		FFileHelper::LoadFileToString(
			PublishedDescriptorJson,
			*FPaths::Combine(PackageDirectory, AvidScriptCSharpBindingArtifact::DescriptorFileName))
		&& FAvidScriptBindingDescriptorParser::Parse(
			PublishedDescriptorJson,
			PublishedPackage,
			PublishedParseCategory,
			PublishedParseSource));
	TestEqual(TEXT("Published project descriptor contains one class reference"), PublishedPackage.ClassReferences.Num(), 1);
	FString PublishedReferenceSource;
	TestTrue(
		TEXT("Project authorization facade is readable"),
		FFileHelper::LoadFileToString(
			PublishedReferenceSource,
			*FPaths::Combine(PackageDirectory, AvidScriptCSharpBindingArtifact::ReferenceSourceFileName)));
	TestTrue(
		TEXT("Project authorization facade exposes ProjectileClass"),
		PublishedReferenceSource.Contains(TEXT("public static TSubclassOfAActor ProjectileClass => new(0);")));
	const TArray<FString> ImmutablePackageFiles = {
		FPaths::Combine(PackageDirectory, AvidScriptCSharpBindingArtifact::DescriptorFileName),
		FPaths::Combine(PackageDirectory, AvidScriptCSharpBindingArtifact::ReferenceSourceFileName),
		FirstManifestPath
	};
	const FDateTime FrozenTimestamp(2020, 1, 2, 3, 4, 5);
	for (const FString& PackageFile : ImmutablePackageFiles)
	{
		TestTrue(TEXT("Generated package file exists before reuse"), FPaths::FileExists(PackageFile));
		TestTrue(
			TEXT("Generated package timestamp can be frozen for reuse proof"),
			IFileManager::Get().SetTimeStamp(*PackageFile, FrozenTimestamp));
	}
	FAvidScriptEditorCSharpBuildPipeline::Cleanup(FirstPlan);

	FAvidScriptEditorCSharpBuildPlan ReusedPlan;
	FAvidScriptEditorCSharpBuildResult ReusedPrepareResult;
	const bool bReusedPrepared = FAvidScriptEditorCSharpBuildPipeline::Prepare(
		FAvidScriptEditorCSharpProfileService::MakeBuildRequest(Result),
		ReusedPlan,
		ReusedPrepareResult);
	TestTrue(TEXT("Unchanged project binding profile prepares again"), bReusedPrepared);
	if (bReusedPrepared)
	{
		TestEqual(TEXT("Unchanged profile resolves the same package"), ReusedPlan.AuthorizationBindingPackagePath, FirstManifestPath);
		TestTrue(TEXT("Unchanged profile reuses the content-addressed package"), ReusedPlan.bReusedAuthorizationBindingPackage);
		TestTrue(TEXT("Package reuse reaches the build result"), ReusedPrepareResult.bReusedAuthorizationBindingPackage);
		for (const FString& PackageFile : ImmutablePackageFiles)
		{
			TestEqual(
				TEXT("Descriptor, facade, and manifest timestamps remain unchanged"),
				IFileManager::Get().GetTimeStamp(*PackageFile),
				FrozenTimestamp);
		}
	}
	FAvidScriptEditorCSharpBuildPipeline::Cleanup(ReusedPlan);

	const FString TypedSelfProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		TestRoot,
		TEXT("typed_self_project_binding.csharp-profile.json")));
	FString TypedSelfProfileText = ProfileText;
	TypedSelfProfileText.ReplaceInline(
		TEXT("\"schema_version\": 2"),
		TEXT("\"schema_version\": 3"),
		ESearchCase::CaseSensitive);
	TypedSelfProfileText.ReplaceInline(
		TEXT("    \"package_name\": \"avidscript.project.profile_service\",\n"),
		TEXT("    \"package_name\": \"avidscript.project.profile_service\",\n")
		TEXT("    \"self_class_path\": \"/Script/AvidScriptEditor.AvidScriptBindingRuntimeProcessEventTestActor\",\n"),
		ESearchCase::CaseSensitive);
	TestTrue(
		TEXT("Typed self project binding C# profile can be written"),
		FFileHelper::SaveStringToFile(TypedSelfProfileText, *TypedSelfProfilePath));
	FAvidScriptEditorCSharpProfileLoadResult TypedSelfResult;
	TestTrue(
		TEXT("Schema v3 typed self project binding profile loads"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(TypedSelfProfilePath, TypedSelfResult));
	TestEqual(TEXT("Typed self project binding schema is 3"), TypedSelfResult.SchemaVersion, 3);
	TestEqual(
		TEXT("Typed self project binding spec retains self class"),
		TypedSelfResult.ProjectBindingProfile.SelfClassPath,
		FString(TEXT("/Script/AvidScriptEditor.AvidScriptBindingRuntimeProcessEventTestActor")));
	TestEqual(
		TEXT("Typed self project binding selection retains self class"),
		TypedSelfResult.ResolvedBindingSelection.SelfClassPath,
		FString(TEXT("/Script/AvidScriptEditor.AvidScriptBindingRuntimeProcessEventTestActor")));

	const FString InvalidProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		TestRoot,
		TEXT("invalid_project_binding.csharp-profile.json")));
	const FString InvalidProfileText = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 2,\n")
		TEXT("  \"language\": \"csharp\",\n")
		TEXT("  \"source_path\": \"%s\",\n")
		TEXT("  \"binding_profile\": {\n")
		TEXT("    \"package_name\": \"avidscript.project.invalid_json_type\",\n")
		TEXT("    \"classes\": [42]\n")
		TEXT("  }\n")
		TEXT("}\n"),
		*SourcePath);
	TestTrue(
		TEXT("Invalid project binding C# profile can be written"),
		FFileHelper::SaveStringToFile(InvalidProfileText, *InvalidProfilePath));
	FAvidScriptEditorCSharpProfileLoadResult InvalidResult;
	TestFalse(
		TEXT("Non-object project binding class fails closed"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(InvalidProfilePath, InvalidResult));
	TestEqual(
		TEXT("Non-object project binding class has stable category"),
		InvalidResult.ErrorCategory,
		FString(TEXT("binding_profile_class_invalid")));

	const FString InvalidArrayProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		TestRoot,
		TEXT("invalid_project_binding_array.csharp-profile.json")));
	const FString InvalidArrayProfileText = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 2,\n")
		TEXT("  \"language\": \"csharp\",\n")
		TEXT("  \"source_path\": \"%s\",\n")
		TEXT("  \"binding_profile\": {\n")
		TEXT("    \"package_name\": \"avidscript.project.invalid_array_type\",\n")
		TEXT("    \"module_paths\": [42]\n")
		TEXT("  }\n")
		TEXT("}\n"),
		*SourcePath);
	TestTrue(
		TEXT("Invalid array project binding C# profile can be written"),
		FFileHelper::SaveStringToFile(InvalidArrayProfileText, *InvalidArrayProfilePath));
	FAvidScriptEditorCSharpProfileLoadResult InvalidArrayResult;
	TestFalse(
		TEXT("Non-string project binding array value fails closed"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(
			InvalidArrayProfilePath,
			InvalidArrayResult));
	TestEqual(
		TEXT("Non-string project binding array value has stable category"),
		InvalidArrayResult.ErrorCategory,
		FString(TEXT("binding_profile_array_value_invalid")));

	const FString InvalidLoadPolicyPath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		TestRoot,
		TEXT("invalid_load_policy.csharp-profile.json")));
	FString InvalidLoadPolicyText = ProfileText;
	InvalidLoadPolicyText.ReplaceInline(
		TEXT("\"load_policy\": \"EditorLoad\""),
		TEXT("\"load_policy\": 42"),
		ESearchCase::CaseSensitive);
	TestTrue(
		TEXT("Invalid load policy profile can be written"),
		FFileHelper::SaveStringToFile(InvalidLoadPolicyText, *InvalidLoadPolicyPath));
	FAvidScriptEditorCSharpProfileLoadResult InvalidLoadPolicyResult;
	TestFalse(
		TEXT("Non-string class reference load policy fails closed"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(
			InvalidLoadPolicyPath,
			InvalidLoadPolicyResult));
	TestEqual(
		TEXT("Non-string load policy has stable category"),
		InvalidLoadPolicyResult.ErrorCategory,
		FString(TEXT("binding_profile_class_reference_load_policy_invalid")));

	const FString LegacyBindingProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		TestRoot,
		TEXT("legacy_binding_profile.csharp-profile.json")));
	FString LegacyBindingProfileText = ProfileText;
	LegacyBindingProfileText.ReplaceInline(
		TEXT("\"schema_version\": 2"),
		TEXT("\"schema_version\": 1"),
		ESearchCase::CaseSensitive);
	TestTrue(
		TEXT("Legacy schema with binding profile can be written"),
		FFileHelper::SaveStringToFile(LegacyBindingProfileText, *LegacyBindingProfilePath));
	FAvidScriptEditorCSharpProfileLoadResult LegacyBindingProfileResult;
	TestFalse(
		TEXT("Schema v1 rejects binding_profile instead of silently ignoring it"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(
			LegacyBindingProfilePath,
			LegacyBindingProfileResult));
	TestEqual(
		TEXT("Schema v1 binding profile rejection category is stable"),
		LegacyBindingProfileResult.ErrorCategory,
		FString(TEXT("binding_profile_schema_unsupported")));

	const FString SchemaV2TypedSelfProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		TestRoot,
		TEXT("schema_v2_typed_self.csharp-profile.json")));
	FString SchemaV2TypedSelfProfileText = ProfileText;
	SchemaV2TypedSelfProfileText.ReplaceInline(
		TEXT("    \"package_name\": \"avidscript.project.profile_service\",\n"),
		TEXT("    \"package_name\": \"avidscript.project.profile_service\",\n")
		TEXT("    \"self_class_path\": \"/Script/Engine.Actor\",\n"),
		ESearchCase::CaseSensitive);
	TestTrue(
		TEXT("Schema v2 typed self C# profile can be written"),
		FFileHelper::SaveStringToFile(SchemaV2TypedSelfProfileText, *SchemaV2TypedSelfProfilePath));
	FAvidScriptEditorCSharpProfileLoadResult SchemaV2TypedSelfResult;
	TestFalse(
		TEXT("Schema v2 rejects typed self field"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(
			SchemaV2TypedSelfProfilePath,
			SchemaV2TypedSelfResult));
	TestEqual(
		TEXT("Schema v2 typed self rejection category is stable"),
		SchemaV2TypedSelfResult.ErrorCategory,
		FString(TEXT("profile_self_field_not_supported")));

	const FString SchemaV1TypedSelfProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		TestRoot,
		TEXT("schema_v1_typed_self.csharp-profile.json")));
	FString SchemaV1TypedSelfProfileText = SchemaV2TypedSelfProfileText;
	SchemaV1TypedSelfProfileText.ReplaceInline(
		TEXT("\"schema_version\": 2"),
		TEXT("\"schema_version\": 1"),
		ESearchCase::CaseSensitive);
	TestTrue(
		TEXT("Schema v1 typed self C# profile can be written"),
		FFileHelper::SaveStringToFile(SchemaV1TypedSelfProfileText, *SchemaV1TypedSelfProfilePath));
	FAvidScriptEditorCSharpProfileLoadResult SchemaV1TypedSelfResult;
	TestFalse(
		TEXT("Schema v1 rejects typed self field"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(
			SchemaV1TypedSelfProfilePath,
			SchemaV1TypedSelfResult));
	TestEqual(
		TEXT("Schema v1 typed self rejection category is stable"),
		SchemaV1TypedSelfResult.ErrorCategory,
		FString(TEXT("profile_self_field_not_supported")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpProfileServiceObjectFactorySchemaTest,
	"AvidScript.Editor.CSharpProfileService.ObjectFactorySchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpProfileServiceObjectFactorySchemaTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		GetAvidScriptCSharpProfileServiceTestRoot(),
		TEXT("ObjectFactorySchema")));
	TestTrue(
		TEXT("Object factory profile test root can be created"),
		IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString SourcePath = NormalizeAvidScriptCSharpProfileTestPath(
		FPaths::Combine(TestRoot, TEXT("ObjectFactories.cs")));
	TestTrue(
		TEXT("Object factory profile source can be written"),
		FFileHelper::SaveStringToFile(MakeAvidScriptCSharpProfileSourceText(), *SourcePath));

	const auto MakeProfileText = [&SourcePath](
		const int32 SchemaVersion,
		const FString& ClassReferencesJson,
		const FString& FactoriesJson)
	{
		return FString::Printf(
			TEXT("{\n")
			TEXT("  \"schema_version\": %d,\n")
			TEXT("  \"language\": \"csharp\",\n")
			TEXT("  \"source_path\": \"%s\",\n")
			TEXT("  \"binding_profile\": {\n")
			TEXT("    \"package_name\": \"avidscript.project.object_factories\",\n")
			TEXT("    \"classes\": [{\"class_path\": \"/Script/Engine.Actor\"}],\n")
			TEXT("    \"class_references\": %s,\n")
			TEXT("    \"object_factories\": %s\n")
			TEXT("  }\n")
			TEXT("}\n"),
			SchemaVersion,
			*SourcePath,
			*ClassReferencesJson,
			*FactoriesJson);
	};

	int32 ProfileIndex = 0;
	const auto LoadProfileText = [
		this,
		&TestRoot,
		&ProfileIndex](
			const FString& Label,
			const FString& ProfileText,
			FAvidScriptEditorCSharpProfileLoadResult& OutResult)
	{
		const FString ProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
			TestRoot,
			FString::Printf(TEXT("factory_profile_%d.csharp-profile.json"), ProfileIndex++)));
		if (!TestTrue(
				*FString::Printf(TEXT("%s can be written"), *Label),
				FFileHelper::SaveStringToFile(ProfileText, *ProfilePath)))
		{
			return false;
		}
		return FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, OutResult);
	};

	const FString ActorClassReferences = TEXT(
		"[{"
		"\"script_name\":\"SpawnClass\","
		"\"class_path\":\"/Script/Engine.StaticMeshActor\","
		"\"base_class_path\":\"/Script/Engine.Actor\""
		"}]");
	const FString ObjectFactoryClassReferences = TEXT(
		"["
		"{"
		"\"script_name\":\"InventoryStateClass\","
		"\"class_path\":\"/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject\","
		"\"base_class_path\":\"/Script/CoreUObject.Object\""
		"},"
		"{"
		"\"script_name\":\"SensorComponentClass\","
		"\"class_path\":\"/Script/Engine.StaticMeshComponent\","
		"\"base_class_path\":\"/Script/Engine.ActorComponent\""
		"}"
		"]");
	const FString ValidFactories = TEXT(
		"["
		"{"
		"\"script_name\":\"InventoryState\","
		"\"class_reference\":\"InventoryStateClass\","
		"\"kind\":\"new_object\","
		"\"outer_base_class_path\":\"/Script/CoreUObject.Object\","
		"\"ownership\":\"session\""
		"},"
		"{"
		"\"script_name\":\"SensorComponent\","
		"\"class_reference\":\"SensorComponentClass\","
		"\"kind\":\"actor_component\","
		"\"outer_base_class_path\":\"/Script/Engine.Actor\","
		"\"ownership\":\"session\","
		"\"registration\":\"register_instance\""
		"}"
		"]");

	FAvidScriptEditorCSharpProfileLoadResult SchemaV3Result;
	TestFalse(
		TEXT("Schema v3 rejects object_factories"),
		LoadProfileText(
			TEXT("Schema v3 object factory profile"),
			MakeProfileText(
				3,
				ActorClassReferences,
				TEXT(
					"[{"
					"\"script_name\":\"LegacyFactory\","
					"\"class_reference\":\"SpawnClass\","
					"\"kind\":\"new_object\","
					"\"outer_base_class_path\":\"/Script/CoreUObject.Object\","
					"\"ownership\":\"session\""
					"}]")),
			SchemaV3Result));
	TestEqual(
		TEXT("Schema v3 object factory rejection category is stable"),
		SchemaV3Result.ErrorCategory,
		FString(TEXT("binding_profile_factory_schema_unsupported")));

	FAvidScriptEditorCSharpProfileLoadResult ValidResult;
	TestTrue(
		TEXT("Schema v4 accepts valid object factories"),
		LoadProfileText(
			TEXT("Schema v4 valid object factory profile"),
			MakeProfileText(4, ObjectFactoryClassReferences, ValidFactories),
			ValidResult));
	TestEqual(TEXT("Schema v4 is retained"), ValidResult.SchemaVersion, 4);
	TestEqual(
		TEXT("Schema v4 profile retains two object factories"),
		ValidResult.ProjectBindingProfile.ObjectFactories.Num(),
		2);
	TestEqual(
		TEXT("Schema v4 resolves two object factories"),
		ValidResult.ResolvedObjectFactories.Num(),
		2);
	if (ValidResult.ResolvedObjectFactories.Num() == 2)
	{
		TestEqual(
			TEXT("Resolved factories use deterministic script-name order"),
			ValidResult.ResolvedObjectFactories[0].ScriptName,
			FString(TEXT("InventoryState")));
		TestEqual(
			TEXT("Resolved component factory retains canonical outer"),
			ValidResult.ResolvedObjectFactories[1].OuterBaseClassPath,
			FString(TEXT("/Script/Engine.Actor")));
	}
	const FAvidScriptEditorCSharpBuildRequest ValidFactoryRequest =
		FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ValidResult);
	TestEqual(
		TEXT("Resolved factories reach the C# build request"),
		ValidFactoryRequest.AuthorizationObjectFactories.Num(),
		2);
	FAvidScriptEditorCSharpBuildPlan UnsupportedFactoryPlan;
	FAvidScriptEditorCSharpBuildResult UnsupportedFactoryResult;
	TestFalse(
		TEXT("Incomplete descriptor v7 propagation fails closed"),
		FAvidScriptEditorCSharpBuildPipeline::Prepare(
			ValidFactoryRequest,
			UnsupportedFactoryPlan,
			UnsupportedFactoryResult));
	TestEqual(
		TEXT("Incomplete factory pipeline category is stable"),
		UnsupportedFactoryResult.ErrorCategory,
		FString(TEXT("binding_factory_pipeline_unavailable")));
	FAvidScriptEditorCSharpBuildPipeline::Cleanup(UnsupportedFactoryPlan);

	FAvidScriptEditorCSharpProfileLoadResult MissingReferenceResult;
	TestFalse(
		TEXT("Object factory rejects an unknown class reference"),
		LoadProfileText(
			TEXT("Unknown class reference factory profile"),
			MakeProfileText(
				4,
				ActorClassReferences,
				TEXT(
					"[{"
					"\"script_name\":\"MissingReference\","
					"\"class_reference\":\"UnknownClass\","
					"\"kind\":\"new_object\","
					"\"outer_base_class_path\":\"/Script/CoreUObject.Object\","
					"\"ownership\":\"session\""
					"}]")),
			MissingReferenceResult));
	TestEqual(
		TEXT("Unknown factory class reference category is stable"),
		MissingReferenceResult.ErrorCategory,
		FString(TEXT("binding_profile_factory_class_reference_missing")));

	FString DuplicateFactories = ValidFactories;
	DuplicateFactories.ReplaceInline(
		TEXT("\"script_name\":\"SensorComponent\""),
		TEXT("\"script_name\":\"InventoryState\""),
		ESearchCase::CaseSensitive);
	FAvidScriptEditorCSharpProfileLoadResult DuplicateResult;
	TestFalse(
		TEXT("Object factory script names must be unique"),
		LoadProfileText(
			TEXT("Duplicate object factory profile"),
			MakeProfileText(4, ObjectFactoryClassReferences, DuplicateFactories),
			DuplicateResult));
	TestEqual(
		TEXT("Duplicate factory script name category is stable"),
		DuplicateResult.ErrorCategory,
		FString(TEXT("binding_profile_factory_script_name_duplicate")));

	FString UnknownKindFactories = ValidFactories;
	UnknownKindFactories.ReplaceInline(
		TEXT("\"kind\":\"new_object\""),
		TEXT("\"kind\":\"pooled_object\""),
		ESearchCase::CaseSensitive);
	FAvidScriptEditorCSharpProfileLoadResult UnknownKindResult;
	TestFalse(
		TEXT("Object factory rejects unknown kind"),
		LoadProfileText(
			TEXT("Unknown object factory kind profile"),
			MakeProfileText(4, ObjectFactoryClassReferences, UnknownKindFactories),
			UnknownKindResult));
	TestEqual(
		TEXT("Unknown factory kind category is stable"),
		UnknownKindResult.ErrorCategory,
		FString(TEXT("binding_profile_factory_kind_invalid")));

	FString ComponentWithoutRegistration = ValidFactories;
	ComponentWithoutRegistration.ReplaceInline(
		TEXT(",\"registration\":\"register_instance\""),
		TEXT(""),
		ESearchCase::CaseSensitive);
	FAvidScriptEditorCSharpProfileLoadResult ComponentRegistrationResult;
	TestFalse(
		TEXT("Actor component factory requires register_instance"),
		LoadProfileText(
			TEXT("Unregistered component factory profile"),
			MakeProfileText(4, ObjectFactoryClassReferences, ComponentWithoutRegistration),
			ComponentRegistrationResult));
	TestEqual(
		TEXT("Component registration mismatch category is stable"),
		ComponentRegistrationResult.ErrorCategory,
		FString(TEXT("binding_factory_registration_mismatch")));

	FString NewObjectRegistration = ValidFactories;
	NewObjectRegistration.ReplaceInline(
		TEXT(
			"\"kind\":\"new_object\","
			"\"outer_base_class_path\":\"/Script/CoreUObject.Object\","
			"\"ownership\":\"session\""),
		TEXT(
			"\"kind\":\"new_object\","
			"\"outer_base_class_path\":\"/Script/CoreUObject.Object\","
			"\"ownership\":\"session\","
			"\"registration\":\"register_instance\""),
		ESearchCase::CaseSensitive);
	FAvidScriptEditorCSharpProfileLoadResult NewObjectRegistrationResult;
	TestFalse(
		TEXT("New object factory rejects component registration"),
		LoadProfileText(
			TEXT("Registered UObject factory profile"),
			MakeProfileText(4, ObjectFactoryClassReferences, NewObjectRegistration),
			NewObjectRegistrationResult));
	TestEqual(
		TEXT("New object registration mismatch category is stable"),
		NewObjectRegistrationResult.ErrorCategory,
		FString(TEXT("binding_factory_registration_mismatch")));

	const auto VerifyFactoryParseFailure = [
		this,
		&LoadProfileText,
		&MakeProfileText,
		&ObjectFactoryClassReferences](
			const FString& Label,
			const FString& Factories,
			const FString& ExpectedCategory)
	{
		FAvidScriptEditorCSharpProfileLoadResult Result;
		TestFalse(
			*FString::Printf(TEXT("%s is rejected"), *Label),
			LoadProfileText(
				Label,
				MakeProfileText(4, ObjectFactoryClassReferences, Factories),
				Result));
		TestEqual(
			*FString::Printf(TEXT("%s category is stable"), *Label),
			Result.ErrorCategory,
			ExpectedCategory);
	};

	FString NonStringFactoryField = ValidFactories;
	NonStringFactoryField.ReplaceInline(
		TEXT("\"script_name\":\"InventoryState\""),
		TEXT("\"script_name\":42"),
		ESearchCase::CaseSensitive);
	VerifyFactoryParseFailure(
		TEXT("Non-string object factory field"),
		NonStringFactoryField,
		TEXT("binding_profile_factory_invalid"));

	FString EmptyFactoryField = ValidFactories;
	EmptyFactoryField.ReplaceInline(
		TEXT("\"class_reference\":\"InventoryStateClass\""),
		TEXT("\"class_reference\":\"\""),
		ESearchCase::CaseSensitive);
	VerifyFactoryParseFailure(
		TEXT("Empty object factory field"),
		EmptyFactoryField,
		TEXT("binding_profile_factory_invalid"));

	FString UnknownOwnershipFactories = ValidFactories;
	UnknownOwnershipFactories.ReplaceInline(
		TEXT("\"ownership\":\"session\""),
		TEXT("\"ownership\":\"world\""),
		ESearchCase::CaseSensitive);
	VerifyFactoryParseFailure(
		TEXT("Unknown object factory ownership"),
		UnknownOwnershipFactories,
		TEXT("binding_profile_factory_ownership_invalid"));

	FString UnknownRegistrationFactories = ValidFactories;
	UnknownRegistrationFactories.ReplaceInline(
		TEXT("\"registration\":\"register_instance\""),
		TEXT("\"registration\":\"auto\""),
		ESearchCase::CaseSensitive);
	VerifyFactoryParseFailure(
		TEXT("Unknown object factory registration"),
		UnknownRegistrationFactories,
		TEXT("binding_profile_factory_registration_invalid"));
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
