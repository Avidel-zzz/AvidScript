#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "Tests/AvidScriptGeneratedNetworkTopologyHarness.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptGenerated, Log, All);

namespace
{
constexpr TCHAR GeneratedPackageDescriptorRelativePath[] =
	TEXT("Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json");
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
		const FString DescriptorPath = FPaths::Combine(
			Plugin->GetBaseDir(),
			GeneratedPackageDescriptorRelativePath);
		if (!FPaths::FileExists(DescriptorPath))
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
