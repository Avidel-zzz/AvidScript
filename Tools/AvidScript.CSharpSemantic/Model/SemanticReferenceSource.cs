namespace AvidScript.CSharpSemantic;

public sealed record SemanticReferenceSource(
    string Source,
    string SourceId,
    bool IsExecutable = false);
