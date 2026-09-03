#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "Tests/AvidScriptGeneratedNetworkTopologyHarness.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptGenerated, Log, All);

namespace
{
constexpr TCHAR EditorPackageDescriptorRelativePath[] =
	TEXT("Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json");
constexpr TCHAR CookPackageDescriptorRelativePath[] =
	TEXT("Content/AvidScriptGenerated/current.json");

#if WITH_EDITOR
bool ReadGeneratedPackageDescriptor(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
{
	FString Json;
	return FFileHelper::LoadFileToString(Json, *Path)
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), OutObject)
		&& OutObject.IsValid();
}

bool MatchesHeadlessCookPackage(const FJsonObject& Editor, const FJsonObject& Cook)
{
	const auto SameString = [&](const TCHAR* EditorField, const TCHAR* CookField)
	{
		FString Left;
		FString Right;
		return Editor.TryGetStringField(EditorField, Left) && !Left.IsEmpty()
			&& Cook.TryGetStringField(CookField, Right) && Left == Right;
	};
	const TSharedPtr<FJsonObject>* EditorTypes = nullptr;
	const TSharedPtr<FJsonObject>* CookTypes = nullptr;
	FString EditorTypeHash;
	FString CookTypeHash;
	return SameString(TEXT("runtime_package_id"), TEXT("package_id"))
		&& SameString(TEXT("runtime_module_id"), TEXT("module_id"))
		&& SameString(TEXT("generation_key_sha256"), TEXT("generation_key_sha256"))
		&& SameString(TEXT("module_name"), TEXT("module_name"))
		&& Editor.TryGetObjectField(TEXT("type_manifest"), EditorTypes)
		&& Cook.TryGetObjectField(TEXT("type_manifest"), CookTypes)
		&& (*EditorTypes)->TryGetStringField(TEXT("sha256"), EditorTypeHash)
		&& (*CookTypes)->TryGetStringField(TEXT("sha256"), CookTypeHash)
		&& !EditorTypeHash.IsEmpty() && EditorTypeHash == CookTypeHash;
}
#endif

bool TryResolveGeneratedPackageDescriptor(
	const IPlugin& Plugin,
	FString& OutDescriptorPath,
	FString& OutError)
{
	TArray<const TCHAR*> CandidateRelativePaths;
#if WITH_EDITOR
	CandidateRelativePaths.Add(EditorPackageDescriptorRelativePath);
#endif
	CandidateRelativePaths.Add(CookPackageDescriptorRelativePath);
	for (const TCHAR* RelativePath : CandidateRelativePaths)
	{
		const FString CandidatePath = FPaths::Combine(Plugin.GetBaseDir(), RelativePath);
		if (FPaths::FileExists(CandidatePath))
		{
#if WITH_EDITOR
			TSharedPtr<FJsonObject> EditorDescriptor;
			FString Backend;
			if (RelativePath == EditorPackageDescriptorRelativePath
				&& ReadGeneratedPackageDescriptor(CandidatePath, EditorDescriptor)
				&& EditorDescriptor->TryGetStringField(TEXT("execution_backend"), Backend)
				&& Backend == TEXT("wasmtime_precompiled"))
			{
				// Cold Editor processes must use published-package trust, not a prior process's attestation.
				const FString CookPath = FPaths::Combine(Plugin.GetBaseDir(), CookPackageDescriptorRelativePath);
				TSharedPtr<FJsonObject> CookDescriptor;
				if (!ReadGeneratedPackageDescriptor(CookPath, CookDescriptor)
					|| !MatchesHeadlessCookPackage(*EditorDescriptor, *CookDescriptor))
				{
					OutError = TEXT("Headless Generated Type package does not match the Cook pointer; republish for the Editor platform/configuration.");
					return false;
				}
				OutDescriptorPath = CookPath;
				return true;
			}
#endif
			OutDescriptorPath = CandidatePath;
			return true;
		}
	}
	return false;
}
}

class FAvidScriptGeneratedModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FAvidScriptGeneratedNetworkTopologyHarness::Startup();
		if (FAvidScriptGeneratedTypeRuntimeHost::IsCommandletExecutionSuppressed())
		{
			UE_LOG(
				LogAvidScriptGenerated,
				Verbose,
				TEXT("Generated Type execution is suppressed for this commandlet."));
			return;
		}
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogAvidScriptGenerated, Error, TEXT("AvidScript plugin root is unavailable."));
			return;
		}
		FString DescriptorPath;
		FString ResolveError;
		if (!TryResolveGeneratedPackageDescriptor(*Plugin, DescriptorPath, ResolveError))
		{
			if (!ResolveError.IsEmpty())
			{
				UE_LOG(LogAvidScriptGenerated, Error, TEXT("%s"), *ResolveError);
				return;
			}
			UE_LOG(
				LogAvidScriptGenerated,
				Verbose,
				TEXT("No generated script package descriptor is installed."));
			return;
		}

		FString Error;
		bInstalledPackage = FAvidScriptGeneratedTypeRuntimeHost::Get()
			.InstallPackageFromDescriptorFile(DescriptorPath, Error);
		if (!bInstalledPackage)
		{
			UE_LOG(
				LogAvidScriptGenerated,
				Error,
				TEXT("Generated script package installation failed: %s"),
				Error.IsEmpty() ? TEXT("unknown") : *Error);
		}
	}

	virtual void ShutdownModule() override
	{
		FAvidScriptGeneratedNetworkTopologyHarness::Shutdown();
		if (!bInstalledPackage)
		{
			return;
		}
		FString Error;
		if (!FAvidScriptGeneratedTypeRuntimeHost::Get().ClearPackage(Error))
		{
			UE_LOG(
				LogAvidScriptGenerated,
				Warning,
				TEXT("Generated script package teardown was deferred: %s"),
				Error.IsEmpty() ? TEXT("unknown") : *Error);
		}
		bInstalledPackage = false;
	}

private:
	bool bInstalledPackage = false;
};

IMPLEMENT_MODULE(FAvidScriptGeneratedModule, AvidScriptGenerated)

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedHeadlessBootstrapIdentityTest,
	"AvidScript.GeneratedTypes.HeadlessBootstrapIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedHeadlessBootstrapIdentityTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	FJsonObject Editor;
	FJsonObject Cook;
	Editor.SetStringField(TEXT("runtime_package_id"), FString::ChrN(64, TEXT('a')));
	Cook.SetStringField(TEXT("package_id"), FString::ChrN(64, TEXT('a')));
	Editor.SetStringField(TEXT("runtime_module_id"), TEXT("generated.test"));
	Cook.SetStringField(TEXT("module_id"), TEXT("generated.test"));
	for (const TCHAR* Field : { TEXT("module_name"), TEXT("generation_key_sha256") })
	{
		Editor.SetStringField(Field, TEXT("same"));
		Cook.SetStringField(Field, TEXT("same"));
	}
	const TSharedRef<FJsonObject> EditorTypes = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> CookTypes = MakeShared<FJsonObject>();
	EditorTypes->SetStringField(TEXT("sha256"), FString::ChrN(64, TEXT('b')));
	CookTypes->SetStringField(TEXT("sha256"), FString::ChrN(64, TEXT('b')));
	Editor.SetObjectField(TEXT("type_manifest"), EditorTypes);
	Cook.SetObjectField(TEXT("type_manifest"), CookTypes);
	TestTrue(TEXT("Headless metadata selects the exact published package"), MatchesHeadlessCookPackage(Editor, Cook));
	Cook.SetStringField(TEXT("package_id"), FString::ChrN(64, TEXT('c')));
	TestFalse(TEXT("A stale Cook pointer is rejected"), MatchesHeadlessCookPackage(Editor, Cook));
	Cook.SetStringField(TEXT("package_id"), FString::ChrN(64, TEXT('a')));
	CookTypes->SetStringField(TEXT("sha256"), FString::ChrN(64, TEXT('c')));
	TestFalse(TEXT("Type identity drift is rejected"), MatchesHeadlessCookPackage(Editor, Cook));
	CookTypes->SetStringField(TEXT("sha256"), FString::ChrN(64, TEXT('b')));
	Editor.RemoveField(TEXT("runtime_package_id"));
	TestFalse(TEXT("Legacy headless metadata must be republished"), MatchesHeadlessCookPackage(Editor, Cook));
	return true;
}
#endif
