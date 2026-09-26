using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpAsyncLanguageErrorCatalog
{
    public static CSharpLanguageErrorTokenCatalog Build(SemanticDocument document) =>
        CSharpLanguageErrorCatalogBuilder.ForAsync(document);
}
