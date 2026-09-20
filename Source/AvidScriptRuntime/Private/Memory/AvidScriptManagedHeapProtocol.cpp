#include "AvidScriptManagedHeapProtocol.h"

namespace AvidScript::Managed
{
namespace ProtocolPrivate
{
struct FReader
{
	std::span<const std::uint8_t> Bytes;
	std::size_t Position = 0;
	bool Valid = true;

	std::uint64_t Read(unsigned Count)
	{
		if (Count > Bytes.size() - Position) { Valid = false; return 0; }
		std::uint64_t Value = 0;
		for (unsigned I = 0; I < Count; ++I) Value |= std::uint64_t(Bytes[Position++]) << (8 * I);
		return Value;
	}
	std::uint32_t ReadUint32() { return static_cast<std::uint32_t>(Read(4)); }
	FToken ReadToken() { return Read(8); }
	bool Finished() const { return Valid && Position == Bytes.size(); }
};
FHeapProtocolResult Failure(EHeapProtocolError Error) { return {Error, EHeapError::Ok}; }
FHeapProtocolResult FromHeap(EHeapError Error)
{
	return {Error == EHeapError::Ok ? EHeapProtocolError::Ok : EHeapProtocolError::HeapFailure, Error};
}
void WriteToken(std::span<std::uint8_t> Response, FToken Token)
{
	for (unsigned I = 0; I < 8; ++I) Response[I] = static_cast<std::uint8_t>(Token >> (8 * I));
}
}

FHeapProtocolResult ExecuteHeapCommand(FHeap& Heap, std::span<const std::uint8_t> Request,
	std::span<std::uint8_t> Response, std::uint32_t InvocationFrameFloor, std::span<const FToken> TransferredRoots)
{
	using namespace ProtocolPrivate;
	if (Request.size() < Abi::HeaderBytes || Request.size() > Abi::MaxRequestBytes)
		return Failure(EHeapProtocolError::InvalidPacket);
	if (Response.size() > Abi::MaxResponseBytes) return Failure(EHeapProtocolError::InvalidOutput);
	FReader Reader{Request};
	if (Reader.ReadUint32() != Abi::Magic) return Failure(EHeapProtocolError::InvalidVersion);
	const auto Command = static_cast<Abi::ECommand>(Reader.ReadUint32());
	if (Command == Abi::ECommand::ConfigureRootsOnly)
	{
		if (!Reader.Finished()) return Failure(EHeapProtocolError::InvalidPacket);
		if (!Response.empty()) return Failure(EHeapProtocolError::InvalidOutput);
		return FromHeap(Heap.ConfigureRootsOnly());
	}
	if (Command == Abi::ECommand::Configure)
	{
		if (!Response.empty()) return Failure(EHeapProtocolError::InvalidOutput);
		const auto Count = Reader.ReadUint32();
		if (!Reader.Valid) return Failure(EHeapProtocolError::InvalidPacket);
		if (Count == 0 || Count > Abi::MaxLayouts) return Failure(EHeapProtocolError::LimitExceeded);
		// Preflight the entire descriptor, including aggregate reference count, before allocation.
		std::uint32_t TotalReferences = 0;
		for (std::uint32_t I = 0; I < Count; ++I)
		{
			Reader.ReadUint32(); const auto Size = Reader.ReadUint32(); const auto References = Reader.ReadUint32();
			if (!Reader.Valid) return Failure(EHeapProtocolError::InvalidPacket);
			if (Size > Abi::MaxResponseBytes || References > Abi::MaxReferencesPerLayout
				|| References > Abi::MaxTotalReferences - TotalReferences) return Failure(EHeapProtocolError::LimitExceeded);
			TotalReferences += References;
			if (std::uint64_t(References) * 8 > Request.size() - Reader.Position) return Failure(EHeapProtocolError::InvalidPacket);
			Reader.Position += std::size_t(References) * 8;
		}
		if (!Reader.Finished()) return Failure(EHeapProtocolError::InvalidPacket);
		std::vector<FHeapLayout> Layouts; Layouts.reserve(Count);
		Reader.Position = Abi::HeaderBytes + 4;
		for (std::uint32_t I = 0; I < Count; ++I)
		{
			FHeapLayout Layout; Layout.TypeId = Reader.ReadUint32(); Layout.ByteSize = Reader.ReadUint32();
			const auto References = Reader.ReadUint32(); Layout.References.reserve(References);
			for (std::uint32_t J = 0; J < References; ++J) Layout.References.push_back({Reader.ReadUint32(), Reader.ReadUint32()});
			Layouts.push_back(std::move(Layout));
		}
		return FromHeap(Heap.Configure(Layouts));
	}

	FToken First = 0, Second = 0;
	std::uint32_t Type = 0, Offset = 0, Count = 0;
	std::size_t OutputSize = 0;
	switch (Command)
	{
	case Abi::ECommand::PushFrame: OutputSize = 8; break;
	case Abi::ECommand::PopFrame:
	case Abi::ECommand::ReleaseRoot: First = Reader.ReadToken(); break;
	case Abi::ECommand::CreateRoot: First = Reader.ReadToken(); Second = Reader.ReadToken(); OutputSize = 8; break;
	case Abi::ECommand::SetRoot: First = Reader.ReadToken(); Second = Reader.ReadToken(); break;
	case Abi::ECommand::Allocate: Type = Reader.ReadUint32(); First = Reader.ReadToken(); OutputSize = 8; break;
	case Abi::ECommand::ReadBytes:
	case Abi::ECommand::WriteBytes:
		First = Reader.ReadToken(); Type = Reader.ReadUint32(); Offset = Reader.ReadUint32(); Count = Reader.ReadUint32();
		if (Count > Abi::MaxResponseBytes) return Failure(EHeapProtocolError::LimitExceeded);
		if (Command == Abi::ECommand::ReadBytes) OutputSize = Count;
		else if (Reader.Valid && Request.size() - Reader.Position == Count) Reader.Position += Count;
		else return Failure(EHeapProtocolError::InvalidPacket);
		break;
	case Abi::ECommand::ReadReference:
	case Abi::ECommand::WriteReference:
		First = Reader.ReadToken(); Type = Reader.ReadUint32(); Offset = Reader.ReadUint32();
		if (Command == Abi::ECommand::ReadReference) OutputSize = 8;
		else Second = Reader.ReadToken();
		break;
	case Abi::ECommand::Collect: break;
	default: return Failure(EHeapProtocolError::UnknownCommand);
	}
	if (!Reader.Finished()) return Failure(EHeapProtocolError::InvalidPacket);
	if (Response.size() != OutputSize) return Failure(EHeapProtocolError::InvalidOutput);
	// Only SetRoot may use a native transfer, never release or allocation.
	if (InvocationFrameFloor != 0)
	{
		EHeapError Authority = EHeapError::Ok;
		if (Command == Abi::ECommand::CreateRoot) Authority = Heap.ValidateGuestRootFrame(First, InvocationFrameFloor);
		else if (Command == Abi::ECommand::SetRoot || Command == Abi::ECommand::ReleaseRoot || Command == Abi::ECommand::Allocate)
			Authority = Heap.ValidateGuestRootAccess(First, InvocationFrameFloor, TransferredRoots, Command == Abi::ECommand::SetRoot);
		if (Authority != EHeapError::Ok) return FromHeap(Authority);
	}
	EHeapError Error = EHeapError::Ok;
	FToken Token = 0;
	switch (Command)
	{
	case Abi::ECommand::PushFrame: Error = Heap.PushFrame(Token); break;
	case Abi::ECommand::PopFrame:
		if (Heap.GetStats().ActiveFrames <= InvocationFrameFloor) return Failure(EHeapProtocolError::FrameBoundary);
		Error = Heap.PopFrame(First); break;
	case Abi::ECommand::CreateRoot: Error = Heap.CreateRoot(First, Second, Token); break;
	case Abi::ECommand::SetRoot: Error = Heap.SetRoot(First, Second); break;
	case Abi::ECommand::ReleaseRoot: Error = Heap.ReleaseRoot(First); break;
	case Abi::ECommand::Allocate: Error = Heap.Allocate(Type, First, Token); break;
	case Abi::ECommand::ReadBytes: Error = Heap.ReadBytes(First, Type, Offset, Response); break;
	case Abi::ECommand::WriteBytes: Error = Heap.WriteBytes(First, Type, Offset, Request.last(Count)); break;
	case Abi::ECommand::ReadReference: Error = Heap.ReadReference(First, Type, Offset, Token); break;
	case Abi::ECommand::WriteReference: Error = Heap.WriteReference(First, Type, Offset, Second); break;
	case Abi::ECommand::Collect: Error = Heap.Collect(); break;
	default: return Failure(EHeapProtocolError::UnknownCommand);
	}
	if (Error == EHeapError::Ok && OutputSize == 8 && Command != Abi::ECommand::ReadBytes) WriteToken(Response, Token);
	return FromHeap(Error);
}

const char* HeapProtocolErrorName(EHeapProtocolError Error)
{
	switch (Error)
	{
#define AVID_PROTOCOL_NAME(Name) case EHeapProtocolError::Name: return #Name;
	AVID_PROTOCOL_NAME(Ok) AVID_PROTOCOL_NAME(InvalidPacket) AVID_PROTOCOL_NAME(InvalidVersion)
	AVID_PROTOCOL_NAME(UnknownCommand) AVID_PROTOCOL_NAME(InvalidOutput) AVID_PROTOCOL_NAME(LimitExceeded)
	AVID_PROTOCOL_NAME(FrameBoundary) AVID_PROTOCOL_NAME(HeapFailure)
#undef AVID_PROTOCOL_NAME
	}
	return "UnknownProtocolError";
}
const char* HeapErrorName(EHeapError Error)
{
	switch (Error)
	{
#define AVID_HEAP_NAME(Name) case EHeapError::Name: return #Name;
	AVID_HEAP_NAME(Ok) AVID_HEAP_NAME(Closed) AVID_HEAP_NAME(InvalidLimits) AVID_HEAP_NAME(OwnerExhausted)
	AVID_HEAP_NAME(NotConfigured) AVID_HEAP_NAME(AlreadyConfigured) AVID_HEAP_NAME(InvalidLayout)
	AVID_HEAP_NAME(InvalidType) AVID_HEAP_NAME(InvalidObject) AVID_HEAP_NAME(InvalidRoot)
	AVID_HEAP_NAME(InvalidFrame) AVID_HEAP_NAME(FrameOrder) AVID_HEAP_NAME(ObjectLimit)
	AVID_HEAP_NAME(ByteLimit) AVID_HEAP_NAME(RootLimit) AVID_HEAP_NAME(FrameLimit)
	AVID_HEAP_NAME(InvalidRange) AVID_HEAP_NAME(ReferenceOverlap) AVID_HEAP_NAME(InvalidReferenceField)
	AVID_HEAP_NAME(ReferenceTypeMismatch)
	AVID_HEAP_NAME(RootAuthority)
#undef AVID_HEAP_NAME
	}
	return "UnknownHeapError";
}
}
