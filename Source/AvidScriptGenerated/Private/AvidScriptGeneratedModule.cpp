#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "Tests/AvidScriptGeneratedNetworkTopologyHarness.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptGenerated, Log, All);

namespace
{
constexpr TCHAR EditorPackageDescriptorRelativePath[] =
	TEXT("Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json");
constexpr TCHAR CookPackageDescriptorRelativePath[] =
	TEXT("Content/AvidScriptGenerated/current.json");

bool TryResolveGeneratedPackageDescriptor(
	const IPlugin& Plugin,
	FString& OutDescriptorPath)
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
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogAvidScriptGenerated, Error, TEXT("AvidScript plugin root is unavailable."));
			return;
		}
		FString DescriptorPath;
		if (!TryResolveGeneratedPackageDescriptor(*Plugin, DescriptorPath))
		{
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
