#include "Packages/AvidScriptRuntimeCooperativeSafepointProvenance.h"

#include "Dom/JsonObject.h"

namespace
{
bool IsRuntimeSafepointLowercaseSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

bool TryGetRuntimeSafepointUInt(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	uint32& OutValue)
{
	double Number = 0.0;
	if (!Object.TryGetNumberField(FieldName, Number)
		|| !FMath::IsFinite(Number)
		|| Number < 0.0
		|| Number > static_cast<double>(MAX_uint32))
	{
		return false;
	}
	const uint32 Value = static_cast<uint32>(Number);
	if (static_cast<double>(Value) != Number)
	{
		return false;
	}
	OutValue = Value;
	return true;
}
} // namespace

bool ParseAvidScriptRuntimeCooperativeSafepointProvenance(
	const FJsonObject& RootObject,
	const FJsonObject& ExecutionObject,
	const FString& CompilerBuildIdentity,
	FAvidScriptRuntimeCooperativeSafepointProvenance& OutProvenance,
	FString& OutError)
{
	OutProvenance = FAvidScriptRuntimeCooperativeSafepointProvenance();
	OutError.Reset();
	const bool bEpochInterruptionDisabled = CompilerBuildIdentity.Contains(
		TEXT(";epoch_interruption=off;"),
		ESearchCase::CaseSensitive);
	const TSharedPtr<FJsonObject>* SafepointObject = nullptr;
	if (!ExecutionObject.TryGetObjectField(
			TEXT("cooperative_safepoints"),
			SafepointObject))
	{
		if (bEpochInterruptionDisabled)
		{
			OutError = TEXT("Epoch-free execution metadata requires cooperative safepoint provenance.");
			return false;
		}
		return true;
	}
	if (SafepointObject == nullptr || !SafepointObject->IsValid())
	{
		OutError = TEXT("The execution cooperative_safepoints field must be an object.");
		return false;
	}
	bool bEnabled = false;
	if (!(*SafepointObject)->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		OutError = TEXT("The execution cooperative_safepoints object requires an enabled flag.");
		return false;
	}
	if (!bEnabled)
	{
		if (bEpochInterruptionDisabled)
		{
			OutError = TEXT("Epoch-free execution metadata cannot disable cooperative safepoints.");
			return false;
		}
		return true;
	}
	if (!bEpochInterruptionDisabled)
	{
		OutError = TEXT("Enabled cooperative safepoints require an epoch-free compiler identity.");
		return false;
	}

	const TSharedPtr<FJsonObject>* GuestIrObject = nullptr;
	FString RootGuestIrSha256;
	if (!RootObject.TryGetObjectField(TEXT("guest_ir"), GuestIrObject)
		|| GuestIrObject == nullptr
		|| !GuestIrObject->IsValid()
		|| !(*GuestIrObject)->TryGetStringField(
			TEXT("sha256"),
			RootGuestIrSha256)
		|| !(*SafepointObject)->TryGetStringField(
			TEXT("receipt_sha256"),
			OutProvenance.ReceiptSha256)
		|| !(*SafepointObject)->TryGetStringField(
			TEXT("guest_ir_sha256"),
			OutProvenance.GuestIrSha256)
		|| !TryGetRuntimeSafepointUInt(
			**SafepointObject,
			TEXT("proof_schema_version"),
			OutProvenance.ProofSchemaVersion)
		|| !TryGetRuntimeSafepointUInt(
			**SafepointObject,
			TEXT("poll_interval"),
			OutProvenance.PollInterval)
		|| !TryGetRuntimeSafepointUInt(
			**SafepointObject,
			TEXT("loop_poll_blocks"),
			OutProvenance.LoopPollBlockCount)
		|| !TryGetRuntimeSafepointUInt(
			**SafepointObject,
			TEXT("recursive_functions"),
			OutProvenance.RecursiveFunctionCount)
		|| !TryGetRuntimeSafepointUInt(
			**SafepointObject,
			TEXT("site_count"),
			OutProvenance.SiteCount)
		|| !(*SafepointObject)->TryGetStringField(
			TEXT("site_sha256"),
			OutProvenance.SiteSha256)
		|| !IsRuntimeSafepointLowercaseSha256(RootGuestIrSha256)
		|| !IsRuntimeSafepointLowercaseSha256(
			OutProvenance.ReceiptSha256)
		|| !IsRuntimeSafepointLowercaseSha256(
			OutProvenance.GuestIrSha256)
		|| !IsRuntimeSafepointLowercaseSha256(
			OutProvenance.SiteSha256)
		|| RootGuestIrSha256 != OutProvenance.GuestIrSha256
		|| OutProvenance.ProofSchemaVersion != 2
		|| OutProvenance.PollInterval == 0
		|| OutProvenance.PollInterval > 65536
		|| static_cast<uint64>(OutProvenance.LoopPollBlockCount)
			+ static_cast<uint64>(OutProvenance.RecursiveFunctionCount)
			!= OutProvenance.SiteCount)
	{
		OutError = TEXT("The execution cooperative safepoint provenance is incomplete or inconsistent.");
		return false;
	}
	OutProvenance.bEnabled = true;
	return true;
}
