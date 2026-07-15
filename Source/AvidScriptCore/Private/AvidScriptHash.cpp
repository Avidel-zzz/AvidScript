#include "AvidScriptHash.h"

namespace
{
constexpr uint32 RoundConstants[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

uint32 RotateRight(uint32 Value, uint32 Bits)
{
	return (Value >> Bits) | (Value << (32u - Bits));
}

struct FSha256Context
{
	uint32 State[8] = {
		0x6a09e667u,
		0xbb67ae85u,
		0x3c6ef372u,
		0xa54ff53au,
		0x510e527fu,
		0x9b05688cu,
		0x1f83d9abu,
		0x5be0cd19u
	};
	uint8 Buffer[64] = {};
	uint64 TotalBytes = 0;
	uint32 BufferSize = 0;
};

void Transform(FSha256Context& Context, const uint8* Block)
{
	uint32 Schedule[64] = {};
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		const uint32 Offset = Index * 4;
		Schedule[Index] = (static_cast<uint32>(Block[Offset]) << 24)
			| (static_cast<uint32>(Block[Offset + 1]) << 16)
			| (static_cast<uint32>(Block[Offset + 2]) << 8)
			| static_cast<uint32>(Block[Offset + 3]);
	}
	for (uint32 Index = 16; Index < 64; ++Index)
	{
		const uint32 S0 = RotateRight(Schedule[Index - 15], 7)
			^ RotateRight(Schedule[Index - 15], 18)
			^ (Schedule[Index - 15] >> 3);
		const uint32 S1 = RotateRight(Schedule[Index - 2], 17)
			^ RotateRight(Schedule[Index - 2], 19)
			^ (Schedule[Index - 2] >> 10);
		Schedule[Index] = Schedule[Index - 16] + S0 + Schedule[Index - 7] + S1;
	}

	uint32 A = Context.State[0];
	uint32 B = Context.State[1];
	uint32 C = Context.State[2];
	uint32 D = Context.State[3];
	uint32 E = Context.State[4];
	uint32 F = Context.State[5];
	uint32 G = Context.State[6];
	uint32 H = Context.State[7];
	for (uint32 Index = 0; Index < 64; ++Index)
	{
		const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
		const uint32 Choice = (E & F) ^ ((~E) & G);
		const uint32 Temporary1 = H + S1 + Choice + RoundConstants[Index] + Schedule[Index];
		const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
		const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
		const uint32 Temporary2 = S0 + Majority;

		H = G;
		G = F;
		F = E;
		E = D + Temporary1;
		D = C;
		C = B;
		B = A;
		A = Temporary1 + Temporary2;
	}

	Context.State[0] += A;
	Context.State[1] += B;
	Context.State[2] += C;
	Context.State[3] += D;
	Context.State[4] += E;
	Context.State[5] += F;
	Context.State[6] += G;
	Context.State[7] += H;
}

void Update(FSha256Context& Context, TConstArrayView<uint8> Bytes)
{
	Context.TotalBytes += static_cast<uint64>(Bytes.Num());
	for (const uint8 Byte : Bytes)
	{
		Context.Buffer[Context.BufferSize++] = Byte;
		if (Context.BufferSize == 64)
		{
			Transform(Context, Context.Buffer);
			Context.BufferSize = 0;
		}
	}
}

void Finalize(FSha256Context& Context, uint8 OutDigest[32])
{
	const uint64 TotalBits = Context.TotalBytes * 8u;
	Context.Buffer[Context.BufferSize++] = 0x80u;
	if (Context.BufferSize > 56)
	{
		while (Context.BufferSize < 64)
		{
			Context.Buffer[Context.BufferSize++] = 0;
		}
		Transform(Context, Context.Buffer);
		Context.BufferSize = 0;
	}
	while (Context.BufferSize < 56)
	{
		Context.Buffer[Context.BufferSize++] = 0;
	}
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Context.Buffer[Context.BufferSize++] = static_cast<uint8>((TotalBits >> Shift) & 0xffu);
	}
	Transform(Context, Context.Buffer);

	for (uint32 Index = 0; Index < 8; ++Index)
	{
		OutDigest[Index * 4] = static_cast<uint8>((Context.State[Index] >> 24) & 0xffu);
		OutDigest[Index * 4 + 1] = static_cast<uint8>((Context.State[Index] >> 16) & 0xffu);
		OutDigest[Index * 4 + 2] = static_cast<uint8>((Context.State[Index] >> 8) & 0xffu);
		OutDigest[Index * 4 + 3] = static_cast<uint8>(Context.State[Index] & 0xffu);
	}
}
} // namespace

FString FAvidScriptHash::Sha256Hex(TConstArrayView<uint8> Bytes)
{
	FSha256Context Context;
	Update(Context, Bytes);
	uint8 Digest[32] = {};
	Finalize(Context, Digest);

	static constexpr TCHAR HexDigits[] = TEXT("0123456789abcdef");
	FString Result;
	Result.Reserve(64);
	for (const uint8 Byte : Digest)
	{
		Result.AppendChar(HexDigits[Byte >> 4]);
		Result.AppendChar(HexDigits[Byte & 0x0f]);
	}
	return Result;
}

FString FAvidScriptHash::Sha256HexUtf8(const FString& Value)
{
	const FTCHARToUTF8 Utf8(*Value);
	return Sha256Hex(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}
