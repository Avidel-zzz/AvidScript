using System;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class SemanticArtifactReader
{
    public static SemanticDocument Deserialize(ReadOnlySpan<byte> artifact)
    {
        return SemanticSerializer.Deserialize(artifact);
    }
}
