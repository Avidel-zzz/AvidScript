#pragma once

// Engine- and backend-independent core. Runtime owns one heap per module instance.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace AvidScript::Managed
{
using FToken = std::uint64_t;

enum class EHeapError : std::uint8_t
{
	Ok, Closed, InvalidLimits, OwnerExhausted, NotConfigured, AlreadyConfigured,
	InvalidLayout, InvalidType, InvalidObject, InvalidRoot, InvalidFrame, FrameOrder,
	ObjectLimit, ByteLimit, RootLimit, FrameLimit, InvalidRange, ReferenceOverlap,
	InvalidReferenceField, ReferenceTypeMismatch, RootAuthority
};

struct FHeapLimits
{
	std::uint32_t MaxObjects = 16384;
	std::uint32_t MaxRoots = 16384;
	std::uint32_t MaxFrames = 1024;
	std::uint32_t MaxLayouts = 1024;
	std::uint32_t MaxReferencesPerLayout = 256;
	std::uint32_t MaxTotalReferences = 65536;
	std::uint32_t MaxObjectBytes = 64 * 1024;
	std::uint64_t MaxLiveBytes = 16 * 1024 * 1024;
};

struct FReferenceField
{
	std::uint32_t Offset = 0;
	// Zero accepts any managed object, e.g. a nominal delegate's environment.
	std::uint32_t TargetTypeId = 0;
};

struct FHeapLayout
{
	std::uint32_t TypeId = 0;
	std::uint32_t ByteSize = 0;
	std::vector<FReferenceField> References;
};

struct FHeapStats
{
	std::uint32_t LiveObjects = 0;
	std::uint32_t LiveRoots = 0;
	std::uint32_t ActiveFrames = 0;
	std::uint64_t LiveBytes = 0;
	std::uint64_t PeakLiveBytes = 0;
	std::uint64_t Allocations = 0;
	std::uint64_t ReclaimedObjects = 0;
	std::uint64_t Collections = 0;
};

class FHeap;
struct FHeapLifetime;

// Native ownership only. Does not expose root tokens or grant Guest mutation.
// Like FHeap, leases are used and destroyed on the owning execution thread.
class FPersistentRoots final
{
public:
	FPersistentRoots() = default;
	~FPersistentRoots();
	FPersistentRoots(const FPersistentRoots&) = delete;
	FPersistentRoots& operator=(const FPersistentRoots&) = delete;
	FPersistentRoots(FPersistentRoots&& Other) noexcept;
	FPersistentRoots& operator=(FPersistentRoots&& Other) noexcept;
	void Reset();
	bool IsValidFor(const FHeap& Heap) const;
	std::size_t Count() const { return Roots.size(); }
private:
	friend class FHeap;
	std::weak_ptr<FHeapLifetime> Lifetime;
	std::vector<FToken> Roots;
};

class FHeap final
{
public:
	explicit FHeap(FHeapLimits InLimits = {});
	~FHeap();
	FHeap(const FHeap&) = delete;
	FHeap& operator=(const FHeap&) = delete;
	EHeapError Configure(std::span<const FHeapLayout> InLayouts);
	// No object allocation authority; only frames and null roots are available.
	EHeapError ConfigureRootsOnly();
	// Native transfers grant only roots owned by the caller's current activation.
	EHeapError ValidateRootTransfer(std::span<const FToken> InRoots, std::uint32_t InvocationFloor = 0) const;
	bool IsObjectRootedInCurrentFrame(FToken Object, std::uint32_t InvocationFloor = 0) const;
	EHeapError ValidateGuestRootFrame(FToken Frame, std::uint32_t InvocationFloor) const;
	EHeapError ValidateGuestRootAccess(FToken Root, std::uint32_t InvocationFloor,
		std::span<const FToken> TransferredRoots, bool bAllowTransfer) const;
	EHeapError PushFrame(FToken& OutFrame);
	EHeapError PopFrame(FToken Frame);
	EHeapError UnwindToDepth(std::uint32_t Depth);
	// Frame zero denotes a persistent root owned by a continuation/subscription/etc.
	EHeapError CreateRoot(FToken Frame, FToken InitialObject, FToken& OutRoot);
	EHeapError SetRoot(FToken Root, FToken Object);
	EHeapError ReleaseRoot(FToken Root);
	// Acquires all non-null objects before replacing OutRoots. Failure preserves
	// its old lease and releases every partially acquired root.
	EHeapError RetainPersistent(std::span<const FToken> Objects, FPersistentRoots& OutRoots);
	// Allocation publishes into an existing root atomically, including across GC.
	EHeapError Allocate(std::uint32_t TypeId, FToken Root, FToken& OutObject);
	EHeapError ReadBytes(FToken Object, std::uint32_t ExpectedType, std::uint32_t Offset, std::span<std::uint8_t> OutBytes) const;
	EHeapError WriteBytes(FToken Object, std::uint32_t ExpectedType, std::uint32_t Offset, std::span<const std::uint8_t> Bytes);
	EHeapError ReadReference(FToken Object, std::uint32_t ExpectedType, std::uint32_t Offset, FToken& OutReference) const;
	EHeapError WriteReference(FToken Object, std::uint32_t ExpectedType, std::uint32_t Offset, FToken Reference);
	EHeapError Collect();
	void Close();
	bool IsAlive(FToken Object, std::uint32_t ExpectedType = 0) const;
	FHeapStats GetStats() const { return Stats; }

private:
	static constexpr std::uint32_t InvalidIndex = 0xffffffffu;
	static constexpr std::uint32_t MaxGeneration = (1u << 22) - 1;
	enum class ETokenKind : std::uint32_t { Object = 1, Root = 2, Frame = 3 };
	struct FSlot { std::uint32_t Generation = 1; bool Live = false; };
	struct FObjectSlot : FSlot { std::uint32_t LayoutIndex = 0; bool Marked = false; std::vector<std::uint8_t> Bytes; };
	struct FRootSlot : FSlot { FToken Object = 0; std::uint32_t Frame = InvalidIndex; std::uint32_t Previous = InvalidIndex; std::uint32_t Next = InvalidIndex; };
	struct FFrameSlot : FSlot { std::uint32_t FirstRoot = InvalidIndex; std::uint32_t Depth = 0; };
	FToken Token(ETokenKind Kind, std::uint32_t Index, std::uint32_t Generation) const;
	std::uint32_t Index(FToken Value, ETokenKind Kind) const;
	std::uint32_t ObjectIndex(FToken Value, std::uint32_t ExpectedType = 0) const;
	std::uint32_t RootIndex(FToken Value) const;
	std::uint32_t FrameIndex(FToken Value) const;
	std::uint32_t LayoutIndex(std::uint32_t TypeId) const;
	EHeapError Ready() const;
	EHeapError ByteRange(const FObjectSlot& Object, std::uint32_t Offset, std::size_t Size) const;
	const FReferenceField* ReferenceField(const FObjectSlot& Object, std::uint32_t Offset) const;
	void ReleaseRootIndex(std::uint32_t Slot);
	template<class T> static std::uint32_t Acquire(std::vector<T>& Slots, std::vector<std::uint32_t>& Free, std::uint32_t Limit);
	template<class T> static void Retire(T& Slot, std::uint32_t Index, std::vector<std::uint32_t>& Free);

	FHeapLimits Limits;
	std::shared_ptr<FHeapLifetime> Lifetime;
	FHeapStats Stats;
	std::uint32_t Owner = 0;
	bool bValidLimits = false;
	bool bConfigured = false;
	bool bClosed = false;
	std::vector<FHeapLayout> Layouts;
	std::vector<FObjectSlot> Objects;
	std::vector<FRootSlot> Roots;
	std::vector<FFrameSlot> Frames;
	std::vector<std::uint32_t> FreeObjects, FreeRoots, FreeFrames, FrameStack;
};
}
