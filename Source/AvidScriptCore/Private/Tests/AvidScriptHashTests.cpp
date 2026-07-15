#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptHash.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSha256KnownVectorsTest,
	"AvidScript.Core.Hash.Sha256KnownVectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSha256KnownVectorsTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Empty SHA-256 vector"),
		FAvidScriptHash::Sha256Hex(TConstArrayView<uint8>()),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqual(
		TEXT("abc SHA-256 vector"),
		FAvidScriptHash::Sha256HexUtf8(TEXT("abc")),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	return true;
}

#endif
