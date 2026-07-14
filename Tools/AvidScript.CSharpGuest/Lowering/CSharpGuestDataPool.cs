using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed class CSharpGuestDataPool
{
    private readonly IReadOnlyList<GuestType> types;
    private readonly Dictionary<string, GuestDataSegment> segments = new(StringComparer.Ordinal);

    public CSharpGuestDataPool(IReadOnlyList<GuestType> types)
    {
        this.types = types;
    }

    public IReadOnlyList<GuestDataSegment> Segments => segments.Values
        .OrderBy(segment => segment.Id, StringComparer.Ordinal)
        .ToArray();

    public string? AddUtf8String(
        string typeId,
        string value,
        List<GuestDiagnostic> diagnostics)
    {
        GuestDataEncodingResult encoding = GuestDataLayout.CreateUtf8String("pending", typeId, value);
        return Add(encoding, "string", diagnostics);
    }

    public string? AddConstantArray(
        string typeId,
        IReadOnlyList<GuestConstant> elements,
        List<GuestDiagnostic> diagnostics)
    {
        GuestDataEncodingResult encoding = GuestDataLayout.CreateConstantArray(
            "pending",
            typeId,
            elements,
            types);
        return Add(encoding, "array", diagnostics);
    }

    private string? Add(
        GuestDataEncodingResult encoding,
        string category,
        List<GuestDiagnostic> diagnostics)
    {
        if (!encoding.Succeeded || encoding.Segment is null)
        {
            foreach (GuestDiagnostic diagnostic in encoding.Diagnostics)
            {
                diagnostics.Add(new GuestDiagnostic(
                    "ASCG1003",
                    "error",
                    $"{diagnostic.Code}: {diagnostic.Message}",
                    null));
            }

            return null;
        }

        GuestDataSegment segment = encoding.Segment;
        string hash = Hash(segment.Kind, segment.TypeId, segment.Bytes);
        string id = $"data:{category}:{hash}";
        segments.TryAdd(id, segment with { Id = id });
        return id;
    }

    private static string Hash(string kind, string typeId, IReadOnlyList<byte> bytes)
    {
        using IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData(Encoding.UTF8.GetBytes(kind));
        hash.AppendData(new byte[] { 0 });
        hash.AppendData(Encoding.UTF8.GetBytes(typeId));
        hash.AppendData(new byte[] { 0 });
        hash.AppendData(bytes is byte[] array ? array : bytes.ToArray());
        return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }
}
