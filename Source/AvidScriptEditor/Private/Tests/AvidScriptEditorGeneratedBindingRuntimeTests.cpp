#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedBindingRegistry.h"

#include "Async/Async.h"
#include "Misc/AutomationTest.h"

namespace
{
EAvidScriptVmTypedHostStatus GeneratedPairCall(
	UObject& Receiver,
	const int32 Left,
	const int32 Right,
	int32& OutValue)
{
	OutValue = Left + Right;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

EAvidScriptVmTypedHostStatus GeneratedVectorCall(
	UObject& Receiver,
	const FVector& InValue,
	FVector& OutValue)
{
	OutValue = InValue;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

FAvidScriptGeneratedBindingEntry MakePairEntry(
	const FString& PackageHash,
	const FString& StableId,
	const FString& DescriptorIdentity)
{
	FAvidScriptGeneratedBindingEntry Entry;
	Entry.StableId = StableId;
	Entry.PackageHash = PackageHash;
	Entry.DescriptorIdentity = DescriptorIdentity;
	Entry.Shape = EAvidScriptGeneratedBindingShape::I32PairToI32;
	Entry.ReceiverMode = EAvidScriptGeneratedReceiverMode::SelfBound;
	Entry.I32PairCall = &GeneratedPairCall;
	return Entry;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedBindingRegistryLeaseTest,
	"AvidScript.Editor.GeneratedBindings.RegistryLeaseRevocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedBindingRegistryLeaseTest::RunTest(
	const FString& Parameters)
{
	const FString PackageHash = FString::ChrN(64, TEXT('d'));
	FAvidScriptGeneratedBindingRegistry& Registry =
		FAvidScriptGeneratedBindingRegistry::Get();
	Registry.UnregisterPackage(PackageHash);

	const FAvidScriptGeneratedBindingEntry Entry = MakePairEntry(
		PackageHash,
		FString::ChrN(64, TEXT('1')),
		TEXT("test::pair"));
	FString Error;
	TestTrue(
		TEXT("A valid generated package registers"),
		Registry.RegisterPackage(PackageHash, MakeArrayView(&Entry, 1), Error));

	FAvidScriptGeneratedBindingLease Lease;
	TestTrue(
		TEXT("The exact generated identity acquires a lease"),
		Registry.Acquire(
			PackageHash,
			Entry.StableId,
			Entry.DescriptorIdentity,
			Entry.Shape,
			Lease,
			Error));
	TestTrue(TEXT("A newly acquired lease is active"), Lease.IsActive());
	TestNotNull(TEXT("An active lease publishes its immutable entry"), Lease.GetEntry());
	const bool bWrongThreadRejected = Async(
		EAsyncExecution::ThreadPool,
		[&Registry, PackageHash, Entry]()
		{
			FAvidScriptGeneratedBindingLease WorkerLease;
			FString WorkerError;
			return !Registry.Acquire(
				PackageHash,
				Entry.StableId,
				Entry.DescriptorIdentity,
				Entry.Shape,
				WorkerLease,
				WorkerError)
				&& !WorkerLease.IsActive()
				&& WorkerLease.GetEntry() == nullptr;
		}).Get();
	TestTrue(
		TEXT("Wrong-thread acquire and lease access fail closed"),
		bWrongThreadRejected);

	FAvidScriptGeneratedBindingLease WrongShapeLease;
	TestFalse(
		TEXT("A shape mismatch fails closed"),
		Registry.Acquire(
			PackageHash,
			Entry.StableId,
			Entry.DescriptorIdentity,
			EAvidScriptGeneratedBindingShape::VectorValue,
			WrongShapeLease,
			Error));
	TestFalse(
		TEXT("A duplicate package identity is rejected"),
		Registry.RegisterPackage(PackageHash, MakeArrayView(&Entry, 1), Error));

	Registry.UnregisterPackage(PackageHash);
	TestFalse(TEXT("Package unload revokes an existing lease"), Lease.IsActive());
	TestNull(TEXT("A revoked lease exposes no generated function pointer"), Lease.GetEntry());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedBindingRegistryValidationTest,
	"AvidScript.Editor.GeneratedBindings.RegistryValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedBindingRegistryValidationTest::RunTest(
	const FString& Parameters)
{
	const FString PackageHash = FString::ChrN(64, TEXT('e'));
	FAvidScriptGeneratedBindingRegistry& Registry =
		FAvidScriptGeneratedBindingRegistry::Get();
	Registry.UnregisterPackage(PackageHash);

	FAvidScriptGeneratedBindingEntry InvalidEntry = MakePairEntry(
		PackageHash,
		FString::ChrN(64, TEXT('2')),
		TEXT("test::invalid-pointer-shape"));
	InvalidEntry.VectorValueCall = &GeneratedVectorCall;
	FString Error;
	TestFalse(
		TEXT("More than one shape-specific pointer is rejected"),
		Registry.RegisterPackage(
			PackageHash,
			MakeArrayView(&InvalidEntry, 1),
			Error));

	const FAvidScriptGeneratedBindingEntry Duplicate = MakePairEntry(
		PackageHash,
		FString::ChrN(64, TEXT('3')),
		TEXT("test::duplicate"));
	const TArray<FAvidScriptGeneratedBindingEntry> DuplicateEntries = {
		Duplicate,
		Duplicate
	};
	TestFalse(
		TEXT("Duplicate stable and descriptor identities are rejected"),
		Registry.RegisterPackage(PackageHash, DuplicateEntries, Error));

	const TArray<FAvidScriptGeneratedBindingEntry> NonAdjacentDescriptorEntries = {
		MakePairEntry(
			PackageHash,
			FString::ChrN(64, TEXT('4')),
			TEXT("test::non-adjacent-duplicate")),
		MakePairEntry(
			PackageHash,
			FString::ChrN(64, TEXT('5')),
			TEXT("test::middle")),
		MakePairEntry(
			PackageHash,
			FString::ChrN(64, TEXT('6')),
			TEXT("test::non-adjacent-duplicate"))
	};
	TestFalse(
		TEXT("A non-adjacent descriptor identity duplicate is rejected"),
		Registry.RegisterPackage(
			PackageHash,
			NonAdjacentDescriptorEntries,
			Error));

	const FString MalformedPackageHash = TEXT("not-a-sha256");
	const FAvidScriptGeneratedBindingEntry MalformedPackageEntry =
		MakePairEntry(
			MalformedPackageHash,
			FString::ChrN(64, TEXT('7')),
			TEXT("test::malformed-package"));
	TestFalse(
		TEXT("A malformed package hash is rejected"),
		Registry.RegisterPackage(
			MalformedPackageHash,
			MakeArrayView(&MalformedPackageEntry, 1),
			Error));

	const FString UppercasePackageHash = FString::ChrN(64, TEXT('A'));
	const FAvidScriptGeneratedBindingEntry UppercasePackageEntry =
		MakePairEntry(
			UppercasePackageHash,
			FString::ChrN(64, TEXT('8')),
			TEXT("test::uppercase-package"));
	TestFalse(
		TEXT("A non-lowercase package SHA-256 is rejected"),
		Registry.RegisterPackage(
			UppercasePackageHash,
			MakeArrayView(&UppercasePackageEntry, 1),
			Error));

	FAvidScriptGeneratedBindingEntry MalformedStableEntry = MakePairEntry(
		PackageHash,
		FString::ChrN(64, TEXT('A')),
		TEXT("test::malformed-stable"));
	TestFalse(
		TEXT("A non-lowercase stable SHA-256 is rejected"),
		Registry.RegisterPackage(
			PackageHash,
			MakeArrayView(&MalformedStableEntry, 1),
			Error));
	return true;
}

#endif
