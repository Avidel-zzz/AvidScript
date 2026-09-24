#include "AvidScriptManagedHeap.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace AvidScript::Managed
{
struct FHeapLifetime { FHeap* Heap = nullptr; };

FPersistentRoots::~FPersistentRoots() { Reset(); }
FPersistentRoots::FPersistentRoots(FPersistentRoots&& Other) noexcept
	: Lifetime(std::move(Other.Lifetime)), Roots(std::move(Other.Roots)) {}
FPersistentRoots& FPersistentRoots::operator=(FPersistentRoots&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Lifetime = std::move(Other.Lifetime); Roots = std::move(Other.Roots);
	}
	return *this;
}
void FPersistentRoots::Reset()
{
	if (const auto Owner = Lifetime.lock(); Owner && Owner->Heap)
		for (const FToken Root : Roots) Owner->Heap->ReleaseRoot(Root);
	Roots.clear(); Lifetime.reset();
}
bool FPersistentRoots::IsValidFor(const FHeap& Heap) const
{
	const auto Owner = Lifetime.lock();
	return Owner && Owner->Heap == &Heap;
}

namespace HeapCorePrivate
{
// Never wrap an owner identity: tokens from any earlier heap stay invalid.
std::atomic<std::uint32_t> NextOwner{1};
std::uint32_t AcquireOwner()
{
	std::uint32_t Value = NextOwner.load(std::memory_order_relaxed);
	while (Value < (1u << 24))
	{
		if (NextOwner.compare_exchange_weak(Value, Value + 1, std::memory_order_relaxed)) return Value;
	}
	return 0;
}
FToken LoadReference(const std::uint8_t* Bytes)
{
	FToken Value = 0;
	for (unsigned I = 0; I != 8; ++I) Value |= FToken(Bytes[I]) << (I * 8);
	return Value;
}
void StoreReference(std::uint8_t* Bytes, FToken Value)
{
	for (unsigned I = 0; I != 8; ++I) Bytes[I] = std::uint8_t(Value >> (I * 8));
}
}

FHeap::FHeap(FHeapLimits InLimits) : Limits(InLimits), Lifetime(std::make_shared<FHeapLifetime>())
{
	Lifetime->Heap = this;
	bValidLimits = Limits.MaxObjects > 0 && Limits.MaxObjects <= 65535
		&& Limits.MaxRoots > 0 && Limits.MaxRoots <= 65535 && Limits.MaxFrames > 0 && Limits.MaxFrames <= 65535
		&& Limits.MaxLayouts > 0 && Limits.MaxLayouts <= 65535 && Limits.MaxReferencesPerLayout <= 4096
		&& Limits.MaxTotalReferences <= 1024 * 1024
		&& Limits.MaxObjectBytes > 0 && Limits.MaxObjectBytes <= 1024 * 1024
		&& Limits.MaxLiveBytes > 0 && Limits.MaxLiveBytes <= 1024ull * 1024 * 1024;
	if (bValidLimits) Owner = HeapCorePrivate::AcquireOwner();
}

FHeap::~FHeap() { Close(); }

EHeapError FHeap::RetainPersistent(std::span<const FToken> InObjects, FPersistentRoots& OutRoots)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	if (InObjects.size() > Limits.MaxRoots) return EHeapError::RootLimit;
	FPersistentRoots Candidate;
	Candidate.Lifetime = Lifetime;
	Candidate.Roots.reserve(InObjects.size());
	for (const FToken Object : InObjects)
	{
		if (!Object) continue;
		FToken Root = 0;
		if (const auto Error = CreateRoot(0, Object, Root); Error != EHeapError::Ok) return Error;
		Candidate.Roots.push_back(Root);
	}
	OutRoots = std::move(Candidate);
	return EHeapError::Ok;
}

EHeapError FHeap::Ready() const
{
	if (bClosed) return EHeapError::Closed;
	if (!bValidLimits) return EHeapError::InvalidLimits;
	if (!Owner) return EHeapError::OwnerExhausted;
	return bConfigured ? EHeapError::Ok : EHeapError::NotConfigured;
}

EHeapError FHeap::ConfigureRootsOnly()
{
	const EHeapError State = Ready();
	if (State != EHeapError::NotConfigured) return State == EHeapError::Ok ? EHeapError::AlreadyConfigured : State;
	bConfigured = true;
	return EHeapError::Ok;
}

EHeapError FHeap::Configure(std::span<const FHeapLayout> InLayouts)
{
	const EHeapError State = Ready();
	if (State != EHeapError::NotConfigured) return State == EHeapError::Ok ? EHeapError::AlreadyConfigured : State;
	if (InLayouts.empty() || InLayouts.size() > Limits.MaxLayouts) return EHeapError::InvalidLayout;
	std::uint64_t TotalReferences = 0;
	for (const auto& Layout : InLayouts)
	{
		TotalReferences += Layout.References.size();
		if (Layout.References.size() > Limits.MaxReferencesPerLayout || TotalReferences > Limits.MaxTotalReferences
			|| Layout.ByteSize > Limits.MaxObjectBytes)
			return EHeapError::InvalidLayout;
	}
	std::vector<FHeapLayout> Candidate(InLayouts.begin(), InLayouts.end());
	std::sort(Candidate.begin(), Candidate.end(), [](const auto& A, const auto& B) { return A.TypeId < B.TypeId; });
	std::uint32_t PreviousType = 0;
	for (FHeapLayout& Layout : Candidate)
	{
		if (!Layout.TypeId || Layout.TypeId == PreviousType || !Layout.ByteSize || Layout.ByteSize > Limits.MaxObjectBytes
			|| Layout.References.size() > Limits.MaxReferencesPerLayout) return EHeapError::InvalidLayout;
		PreviousType = Layout.TypeId;
		std::sort(Layout.References.begin(), Layout.References.end(), [](const auto& A, const auto& B) { return A.Offset < B.Offset; });
		std::uint64_t PreviousEnd = 0;
		for (const FReferenceField& Field : Layout.References)
		{
			if (Field.Offset % 8 || Field.Offset < PreviousEnd || std::uint64_t(Field.Offset) + 8 > Layout.ByteSize
				|| (Field.TargetTypeId && std::none_of(Candidate.begin(), Candidate.end(), [&](const auto& Item) { return Item.TypeId == Field.TargetTypeId; })))
				return EHeapError::InvalidLayout;
			PreviousEnd = std::uint64_t(Field.Offset) + 8;
		}
	}
	Layouts = std::move(Candidate);
	bConfigured = true;
	return EHeapError::Ok;
}

FToken FHeap::Token(ETokenKind Kind, std::uint32_t Slot, std::uint32_t Generation) const
{
	return (FToken(Owner) << 40) | (FToken(Kind) << 38) | (FToken(Generation) << 16) | (Slot + 1);
}
std::uint32_t FHeap::Index(FToken Value, ETokenKind Kind) const
{
	if ((Value >> 40) != Owner || ((Value >> 38) & 3) != std::uint32_t(Kind) || !(Value & 0xffff)) return InvalidIndex;
	return std::uint32_t(Value & 0xffff) - 1;
}
std::uint32_t FHeap::ObjectIndex(FToken Value, std::uint32_t ExpectedType) const
{
	const std::uint32_t Slot = Index(Value, ETokenKind::Object);
	return Slot < Objects.size() && Objects[Slot].Live && Objects[Slot].Generation == ((Value >> 16) & MaxGeneration)
		&& (!ExpectedType || Layouts[Objects[Slot].LayoutIndex].TypeId == ExpectedType) ? Slot : InvalidIndex;
}
std::uint32_t FHeap::RootIndex(FToken Value) const
{
	const std::uint32_t Slot = Index(Value, ETokenKind::Root);
	return Slot < Roots.size() && Roots[Slot].Live && Roots[Slot].Generation == ((Value >> 16) & MaxGeneration) ? Slot : InvalidIndex;
}
std::uint32_t FHeap::FrameIndex(FToken Value) const
{
	const std::uint32_t Slot = Index(Value, ETokenKind::Frame);
	return Slot < Frames.size() && Frames[Slot].Live && Frames[Slot].Generation == ((Value >> 16) & MaxGeneration) ? Slot : InvalidIndex;
}
std::uint32_t FHeap::LayoutIndex(std::uint32_t TypeId) const
{
	const auto Found = std::lower_bound(Layouts.begin(), Layouts.end(), TypeId, [](const auto& Layout, auto Id) { return Layout.TypeId < Id; });
	return Found != Layouts.end() && Found->TypeId == TypeId ? std::uint32_t(Found - Layouts.begin()) : InvalidIndex;
}
template<class T> std::uint32_t FHeap::Acquire(std::vector<T>& Slots, std::vector<std::uint32_t>& Free, std::uint32_t Limit)
{
	std::uint32_t Slot;
	if (!Free.empty()) { Slot = Free.back(); Free.pop_back(); }
	else { if (Slots.size() >= Limit) return InvalidIndex; Slot = std::uint32_t(Slots.size()); Slots.emplace_back(); }
	Slots[Slot].Live = true;
	return Slot;
}
template<class T> void FHeap::Retire(T& Slot, std::uint32_t SlotIndex, std::vector<std::uint32_t>& Free)
{
	Slot.Live = false;
	if (Slot.Generation < MaxGeneration) { ++Slot.Generation; Free.push_back(SlotIndex); }
}

EHeapError FHeap::ValidateRootTransfer(std::span<const FToken> InRoots, std::uint32_t InvocationFloor) const
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	if (InRoots.size() > Limits.MaxRoots) return EHeapError::RootLimit;
	for (const auto Root : InRoots)
	{
		const auto Slot = RootIndex(Root);
		if (Slot == InvalidIndex) return EHeapError::InvalidRoot;
		if (FrameStack.size() <= InvocationFloor || Roots[Slot].Frame != FrameStack.back()) return EHeapError::RootAuthority;
	}
	return EHeapError::Ok;
}

bool FHeap::IsObjectRootedInCurrentFrame(FToken Object, std::uint32_t InvocationFloor) const
{
	if (Ready() != EHeapError::Ok || ObjectIndex(Object) == InvalidIndex
		|| FrameStack.size() <= InvocationFloor) return false;
	for (auto Root = Frames[FrameStack.back()].FirstRoot; Root != InvalidIndex; Root = Roots[Root].Next)
		if (Roots[Root].Object == Object) return true;
	return false;
}

EHeapError FHeap::ValidateGuestRootFrame(FToken Frame, std::uint32_t InvocationFloor) const
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	if (!Frame) return InvocationFloor == 0 ? EHeapError::Ok : EHeapError::RootAuthority;
	const auto Slot = FrameIndex(Frame);
	if (Slot == InvalidIndex) return EHeapError::InvalidFrame;
	return Frames[Slot].Depth > InvocationFloor ? EHeapError::Ok : EHeapError::RootAuthority;
}

EHeapError FHeap::ValidateGuestRootAccess(FToken Root, std::uint32_t InvocationFloor,
	std::span<const FToken> TransferredRoots, bool bAllowTransfer) const
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = RootIndex(Root);
	if (Slot == InvalidIndex) return EHeapError::InvalidRoot;
	if (InvocationFloor == 0) return EHeapError::Ok;
	const auto Frame = Roots[Slot].Frame;
	if (Frame != InvalidIndex)
	{
		if (Frames[Frame].Depth > InvocationFloor) return EHeapError::Ok;
		if (bAllowTransfer && Frames[Frame].Depth == InvocationFloor
			&& std::find(TransferredRoots.begin(), TransferredRoots.end(), Root) != TransferredRoots.end()) return EHeapError::Ok;
	}
	return EHeapError::RootAuthority;
}

EHeapError FHeap::PushFrame(FToken& OutFrame)
{
	OutFrame = 0;
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = Acquire(Frames, FreeFrames, Limits.MaxFrames);
	if (Slot == InvalidIndex) return EHeapError::FrameLimit;
	Frames[Slot].FirstRoot = InvalidIndex;
	Frames[Slot].Depth = std::uint32_t(FrameStack.size()) + 1;
	FrameStack.push_back(Slot);
	Stats.ActiveFrames = std::uint32_t(FrameStack.size());
	OutFrame = Token(ETokenKind::Frame, Slot, Frames[Slot].Generation);
	return EHeapError::Ok;
}
EHeapError FHeap::PopFrame(FToken Frame)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = FrameIndex(Frame);
	if (Slot == InvalidIndex) return EHeapError::InvalidFrame;
	if (FrameStack.empty() || FrameStack.back() != Slot) return EHeapError::FrameOrder;
	while (Frames[Slot].FirstRoot != InvalidIndex) ReleaseRootIndex(Frames[Slot].FirstRoot);
	FrameStack.pop_back();
	Retire(Frames[Slot], Slot, FreeFrames);
	Stats.ActiveFrames = std::uint32_t(FrameStack.size());
	return EHeapError::Ok;
}
EHeapError FHeap::UnwindToDepth(std::uint32_t Depth)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	if (Depth > FrameStack.size()) return EHeapError::FrameOrder;
	while (FrameStack.size() > Depth)
	{
		const auto Slot = FrameStack.back();
		const auto Error = PopFrame(Token(ETokenKind::Frame, Slot, Frames[Slot].Generation));
		if (Error != EHeapError::Ok) return Error;
	}
	return EHeapError::Ok;
}
EHeapError FHeap::CreateRoot(FToken Frame, FToken InitialObject, FToken& OutRoot)
{
	OutRoot = 0;
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto OwnerFrame = Frame ? FrameIndex(Frame) : InvalidIndex;
	if (Frame && OwnerFrame == InvalidIndex) return EHeapError::InvalidFrame;
	if (InitialObject && ObjectIndex(InitialObject) == InvalidIndex) return EHeapError::InvalidObject;
	const auto Slot = Acquire(Roots, FreeRoots, Limits.MaxRoots);
	if (Slot == InvalidIndex) return EHeapError::RootLimit;
	FRootSlot& Root = Roots[Slot];
	Root.Object = InitialObject;
	Root.Frame = OwnerFrame;
	Root.Previous = InvalidIndex;
	Root.Next = OwnerFrame == InvalidIndex ? InvalidIndex : Frames[OwnerFrame].FirstRoot;
	if (Root.Next != InvalidIndex) Roots[Root.Next].Previous = Slot;
	if (OwnerFrame != InvalidIndex) Frames[OwnerFrame].FirstRoot = Slot;
	++Stats.LiveRoots;
	OutRoot = Token(ETokenKind::Root, Slot, Root.Generation);
	return EHeapError::Ok;
}
EHeapError FHeap::SetRoot(FToken Root, FToken Object)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = RootIndex(Root);
	if (Slot == InvalidIndex) return EHeapError::InvalidRoot;
	if (Object && ObjectIndex(Object) == InvalidIndex) return EHeapError::InvalidObject;
	Roots[Slot].Object = Object;
	return EHeapError::Ok;
}
void FHeap::ReleaseRootIndex(std::uint32_t Slot)
{
	FRootSlot& Root = Roots[Slot];
	if (Root.Previous != InvalidIndex) Roots[Root.Previous].Next = Root.Next;
	else if (Root.Frame != InvalidIndex) Frames[Root.Frame].FirstRoot = Root.Next;
	if (Root.Next != InvalidIndex) Roots[Root.Next].Previous = Root.Previous;
	Root.Object = 0;
	Root.Frame = Root.Previous = Root.Next = InvalidIndex;
	Retire(Root, Slot, FreeRoots);
	--Stats.LiveRoots;
}
EHeapError FHeap::ReleaseRoot(FToken Root)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = RootIndex(Root);
	if (Slot == InvalidIndex) return EHeapError::InvalidRoot;
	ReleaseRootIndex(Slot);
	return EHeapError::Ok;
}

EHeapError FHeap::Allocate(std::uint32_t TypeId, FToken Root, FToken& OutObject)
{
	OutObject = 0;
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto RootSlot = RootIndex(Root), TypeSlot = LayoutIndex(TypeId);
	if (RootSlot == InvalidIndex) return EHeapError::InvalidRoot;
	if (TypeSlot == InvalidIndex) return EHeapError::InvalidType;
	const auto Size = Layouts[TypeSlot].ByteSize;
	if (Size > Limits.MaxLiveBytes) return EHeapError::ByteLimit;
	if (Stats.LiveBytes > Limits.MaxLiveBytes - Size || (FreeObjects.empty() && Objects.size() >= Limits.MaxObjects))
		if (const auto Error = Collect(); Error != EHeapError::Ok) return Error;
	if (Stats.LiveBytes > Limits.MaxLiveBytes - Size) return EHeapError::ByteLimit;
	const auto Slot = Acquire(Objects, FreeObjects, Limits.MaxObjects);
	if (Slot == InvalidIndex) return EHeapError::ObjectLimit;
	Objects[Slot].LayoutIndex = TypeSlot;
	Objects[Slot].Marked = false;
	Objects[Slot].Bytes.assign(Size, 0);
	Stats.LiveBytes += Size;
	Stats.PeakLiveBytes = (std::max)(Stats.PeakLiveBytes, Stats.LiveBytes);
	++Stats.LiveObjects;
	++Stats.Allocations;
	OutObject = Token(ETokenKind::Object, Slot, Objects[Slot].Generation);
	Roots[RootSlot].Object = OutObject;
	return EHeapError::Ok;
}

EHeapError FHeap::ByteRange(const FObjectSlot& Object, std::uint32_t Offset, std::size_t Size) const
{
	if (Offset > Object.Bytes.size() || Size > Object.Bytes.size() - Offset) return EHeapError::InvalidRange;
	for (const auto& Field : Layouts[Object.LayoutIndex].References)
		if (Size && Offset < std::uint64_t(Field.Offset) + 8 && std::uint64_t(Offset) + Size > Field.Offset) return EHeapError::ReferenceOverlap;
	return EHeapError::Ok;
}
EHeapError FHeap::ReadBytes(FToken Object, std::uint32_t ExpectedType, std::uint32_t Offset, std::span<std::uint8_t> OutBytes) const
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = ObjectIndex(Object, ExpectedType);
	if (Slot == InvalidIndex) return EHeapError::InvalidObject;
	if (const auto Error = ByteRange(Objects[Slot], Offset, OutBytes.size()); Error != EHeapError::Ok) return Error;
	if (!OutBytes.empty()) std::memcpy(OutBytes.data(), Objects[Slot].Bytes.data() + Offset, OutBytes.size());
	return EHeapError::Ok;
}
EHeapError FHeap::WriteBytes(FToken Object, std::uint32_t ExpectedType, std::uint32_t Offset, std::span<const std::uint8_t> Bytes)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = ObjectIndex(Object, ExpectedType);
	if (Slot == InvalidIndex) return EHeapError::InvalidObject;
	if (const auto Error = ByteRange(Objects[Slot], Offset, Bytes.size()); Error != EHeapError::Ok) return Error;
	if (!Bytes.empty()) std::memcpy(Objects[Slot].Bytes.data() + Offset, Bytes.data(), Bytes.size());
	return EHeapError::Ok;
}
const FReferenceField* FHeap::ReferenceField(const FObjectSlot& Object, std::uint32_t Offset) const
{
	const auto& Fields = Layouts[Object.LayoutIndex].References;
	const auto Found = std::lower_bound(Fields.begin(), Fields.end(), Offset, [](const auto& Field, auto Value) { return Field.Offset < Value; });
	return Found != Fields.end() && Found->Offset == Offset ? &*Found : nullptr;
}
EHeapError FHeap::ReadReference(FToken Object, std::uint32_t ExpectedType, std::uint32_t Offset, FToken& OutReference) const
{
	OutReference = 0;
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = ObjectIndex(Object, ExpectedType);
	if (Slot == InvalidIndex) return EHeapError::InvalidObject;
	if (!ReferenceField(Objects[Slot], Offset)) return EHeapError::InvalidReferenceField;
	OutReference = HeapCorePrivate::LoadReference(Objects[Slot].Bytes.data() + Offset);
	return EHeapError::Ok;
}
EHeapError FHeap::WriteReference(FToken Object, std::uint32_t ExpectedType, std::uint32_t Offset, FToken Reference)
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	const auto Slot = ObjectIndex(Object, ExpectedType);
	if (Slot == InvalidIndex) return EHeapError::InvalidObject;
	const auto* Field = ReferenceField(Objects[Slot], Offset);
	if (!Field) return EHeapError::InvalidReferenceField;
	if (Reference && ObjectIndex(Reference) == InvalidIndex) return EHeapError::InvalidObject;
	if (Reference && Field->TargetTypeId && ObjectIndex(Reference, Field->TargetTypeId) == InvalidIndex) return EHeapError::ReferenceTypeMismatch;
	HeapCorePrivate::StoreReference(Objects[Slot].Bytes.data() + Offset, Reference);
	return EHeapError::Ok;
}

EHeapError FHeap::Collect()
{
	if (const auto State = Ready(); State != EHeapError::Ok) return State;
	std::vector<std::uint32_t> Work;
	Work.reserve(Objects.size());
	for (auto& Object : Objects) Object.Marked = false;
	auto Mark = [&](FToken Object)
	{
		if (!Object) return true;
		const auto Slot = ObjectIndex(Object);
		if (Slot == InvalidIndex) return false;
		if (!Objects[Slot].Marked) { Objects[Slot].Marked = true; Work.push_back(Slot); }
		return true;
	};
	for (const auto& Root : Roots) if (Root.Live && !Mark(Root.Object)) return EHeapError::InvalidObject;
	while (!Work.empty())
	{
		const auto Slot = Work.back();
		Work.pop_back();
		for (const auto& Field : Layouts[Objects[Slot].LayoutIndex].References)
			if (!Mark(HeapCorePrivate::LoadReference(Objects[Slot].Bytes.data() + Field.Offset))) return EHeapError::InvalidObject;
	}
	for (std::uint32_t Slot = 0; Slot < Objects.size(); ++Slot)
	{
		auto& Object = Objects[Slot];
		if (!Object.Live || Object.Marked) continue;
		Stats.LiveBytes -= Object.Bytes.size();
		std::vector<std::uint8_t>().swap(Object.Bytes);
		Retire(Object, Slot, FreeObjects);
		--Stats.LiveObjects;
		++Stats.ReclaimedObjects;
	}
	++Stats.Collections;
	return EHeapError::Ok;
}
bool FHeap::IsAlive(FToken Object, std::uint32_t ExpectedType) const
{
	return Ready() == EHeapError::Ok && ObjectIndex(Object, ExpectedType) != InvalidIndex;
}
void FHeap::Close()
{
	if (bClosed) return;
	Lifetime->Heap = nullptr;
	bClosed = true;
	Stats.ReclaimedObjects += Stats.LiveObjects;
	Stats.LiveObjects = Stats.LiveRoots = Stats.ActiveFrames = 0;
	Stats.LiveBytes = 0;
	std::vector<FObjectSlot>().swap(Objects);
	std::vector<FRootSlot>().swap(Roots);
	std::vector<FFrameSlot>().swap(Frames);
	std::vector<FHeapLayout>().swap(Layouts);
	std::vector<std::uint32_t>().swap(FreeObjects);
	std::vector<std::uint32_t>().swap(FreeRoots);
	std::vector<std::uint32_t>().swap(FreeFrames);
	std::vector<std::uint32_t>().swap(FrameStack);
}
}
