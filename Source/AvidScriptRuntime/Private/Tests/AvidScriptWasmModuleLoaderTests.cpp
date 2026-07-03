#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmModuleLoader.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString MakeAvidScriptExternalWasmTestPath(const FString& FileName)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptTests"), FileName);
}

void DeleteAvidScriptExternalWasmTestFile(const FString& FilePath)
{
	IFileManager::Get().Delete(*FilePath);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptExternalWasmFileLoaderSmokeTest,
	"AvidScript.Runtime.ExternalWasm.FileLoaderSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptExternalWasmFileLoaderSmokeTest::RunTest(const FString& Parameters)
{
	const FString FixturePath = MakeAvidScriptExternalWasmTestPath(TEXT("external_loader_smoke.wasm"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FixturePath), true);

	const TArray<uint8> FixtureBytes = {
		0x00, 0x61, 0x73, 0x6d,
		0x01, 0x00, 0x00, 0x00
	};
	TestTrue(TEXT("Fixture file writes"), FFileHelper::SaveArrayToFile(FixtureBytes, *FixturePath));

	TArray<uint8> LoadedBytes;
	FAvidScriptWasmModuleLoadResult Result;
	const bool bLoaded = FAvidScriptWasmModuleLoader::LoadFromFile(FixturePath, LoadedBytes, Result);

	TestTrue(TEXT("WASM file loads"), bLoaded);
	TestTrue(TEXT("Load result succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Loaded byte size"), LoadedBytes.Num(), FixtureBytes.Num());
	TestEqual(TEXT("Result byte size"), Result.ByteSize, static_cast<int64>(FixtureBytes.Num()));
	TestEqual(TEXT("Result path"), Result.ModulePath, FixturePath);

	DeleteAvidScriptExternalWasmTestFile(FixturePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptExternalWasmMissingFileSmokeTest,
	"AvidScript.Runtime.ExternalWasm.MissingFileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptExternalWasmMissingFileSmokeTest::RunTest(const FString& Parameters)
{
	const FString MissingPath = MakeAvidScriptExternalWasmTestPath(TEXT("missing_module.wasm"));
	DeleteAvidScriptExternalWasmTestFile(MissingPath);

	TArray<uint8> LoadedBytes;
	FAvidScriptWasmModuleLoadResult Result;
	const bool bLoaded = FAvidScriptWasmModuleLoader::LoadFromFile(MissingPath, LoadedBytes, Result);

	TestFalse(TEXT("Missing file is rejected"), bLoaded);
	TestFalse(TEXT("Load result fails"), Result.bSucceeded);
	TestEqual(TEXT("Missing file category"), Result.ErrorCategory, FString(TEXT("module_file_missing")));
	TestEqual(TEXT("No bytes returned"), LoadedBytes.Num(), 0);
	TestTrue(TEXT("Diagnostic names missing path"), Result.ErrorMessage.Contains(TEXT("missing_module.wasm")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptExternalWasmEmptyFileSmokeTest,
	"AvidScript.Runtime.ExternalWasm.EmptyFileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptExternalWasmEmptyFileSmokeTest::RunTest(const FString& Parameters)
{
	const FString EmptyPath = MakeAvidScriptExternalWasmTestPath(TEXT("empty_module.wasm"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(EmptyPath), true);
	FFileHelper::SaveStringToFile(FString(), *EmptyPath);

	TArray<uint8> LoadedBytes;
	FAvidScriptWasmModuleLoadResult Result;
	const bool bLoaded = FAvidScriptWasmModuleLoader::LoadFromFile(EmptyPath, LoadedBytes, Result);

	TestFalse(TEXT("Empty file is rejected"), bLoaded);
	TestFalse(TEXT("Load result fails"), Result.bSucceeded);
	TestEqual(TEXT("Empty file category"), Result.ErrorCategory, FString(TEXT("module_file_empty")));
	TestEqual(TEXT("No bytes returned"), LoadedBytes.Num(), 0);

	DeleteAvidScriptExternalWasmTestFile(EmptyPath);
	return true;
}

#endif
