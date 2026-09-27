#include "AvidScriptManagedHeap.h"

#include <algorithm>
#include <utility>

namespace AvidScript::Managed
{
EHeapError FHeap::PrepareNativeData(FToken Object, std::unique_ptr<FNativeObjectData> Data,
	FPendingNativeData& OutPublication)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	auto Slot = ObjectIndex(Object);
	if (Slot == InvalidIndex) return EHeapError::InvalidObject;
	if (!Data || !Data->KindId || Data->ByteSize < sizeof(FNativeObjectData) || Data->bPublished)
		return EHeapError::InvalidNativeData;
	if (Objects[Slot].NativeData) return EHeapError::NativeDataConflict;
	const auto Size = Data->ByteSize;
	if (std::uint64_t(Size) + Objects[Slot].Bytes.size() > Limits.MaxObjectBytes
		|| Size > Limits.MaxLiveBytes) return EHeapError::ByteLimit;
	if (Stats.LiveBytes > Limits.MaxLiveBytes - Size)
	{
		if (const auto Error = Collect(); Error != EHeapError::Ok) return Error;
		Slot = ObjectIndex(Object);
		if (Slot == InvalidIndex) return EHeapError::InvalidObject;
	}
	if (Stats.LiveBytes > Limits.MaxLiveBytes - Size) return EHeapError::ByteLimit;
	Objects[Slot].NativeData = std::move(Data);
	Stats.LiveBytes += Size;
	Stats.NativeDataBytes += Size;
	++Stats.NativeDataObjects;
	Stats.PeakLiveBytes = (std::max)(Stats.PeakLiveBytes, Stats.LiveBytes);
	FPendingNativeData Candidate;
	Candidate.Lifetime = Lifetime;
	Candidate.Object = Object;
	OutPublication = std::move(Candidate);
	return EHeapError::Ok;
}

const FNativeObjectData* FHeap::FindNativeData(FToken Object, std::uint32_t Kind) const
{
	if (Ready() != EHeapError::Ok || !Kind) return nullptr;
	const auto Slot = ObjectIndex(Object);
	if (Slot == InvalidIndex) return nullptr;
	const auto* Data = Objects[Slot].NativeData.get();
	return Data && Data->bPublished && Data->KindId == Kind ? Data : nullptr;
}

EHeapError FHeap::PublishNativeData(FToken Object)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = ObjectIndex(Object);
	if (Slot == InvalidIndex) return EHeapError::InvalidObject;
	auto* Data = Objects[Slot].NativeData.get();
	if (!Data || Data->bPublished) return EHeapError::NativeDataConflict;
	Data->bPublished = true;
	return EHeapError::Ok;
}

void FHeap::ReleaseNativeData(std::uint32_t Slot)
{
	auto& Data = Objects[Slot].NativeData;
	if (!Data) return;
	Stats.LiveBytes -= Data->ByteSize;
	Stats.NativeDataBytes -= Data->ByteSize;
	--Stats.NativeDataObjects;
	Data.reset();
}

void FHeap::DiscardPendingNativeData(FToken Object)
{
	if (Ready() != EHeapError::Ok) return;
	const auto Slot = ObjectIndex(Object);
	if (Slot != InvalidIndex && Objects[Slot].NativeData && !Objects[Slot].NativeData->bPublished)
		ReleaseNativeData(Slot);
}
}
