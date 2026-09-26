using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;

namespace AvidScript.CSharpSemantic;

public static class SemanticContract
{
    public const int CurrentSchemaVersion = 31;
    public const string CurrentSemanticVersion = "1.40";
    public const int ExceptionFlowSchemaVersion = 34;
    public const string ExceptionFlowSemanticVersion = "1.43";
    public const int TaskResultSchemaVersion = 35;
    public const string TaskResultSemanticVersion = "1.44";
    public const int TaskLocalSchemaVersion = 36;
    public const string TaskLocalSemanticVersion = "1.45";
    public const int TaskAssignmentSchemaVersion = 37;
    public const string TaskAssignmentSemanticVersion = "1.46";
    public const int TaskExistingLocalSchemaVersion = 38;
    public const string TaskExistingLocalSemanticVersion = "1.47";
    public const int TaskAliasSchemaVersion = 39;
    public const string TaskAliasSemanticVersion = "1.48";
    public const int TaskLanguageErrorSchemaVersion = 40;
    public const string TaskLanguageErrorSemanticVersion = "1.49";
    public const int AsyncLanguageErrorSchemaVersion = 41;
    public const string AsyncLanguageErrorSemanticVersion = "1.50";
    public const int AsyncExceptionFlowSchemaVersion = 42;
    public const string AsyncExceptionFlowSemanticVersion = "1.51";
    public const int DirectAwaitCleanupSchemaVersion = 43;
    public const string DirectAwaitCleanupSemanticVersion = "1.52";
    public const int AsyncCancellationFlowSchemaVersion = 44;
    public const string AsyncCancellationFlowSemanticVersion = "1.53";
    public const int TaskLocalLifetimeSchemaVersion = 45;
    public const string TaskLocalLifetimeSemanticVersion = "1.54";

    public static bool HasTaskLocalLifetimes(SemanticDocument document) =>
        document.SchemaVersion == TaskLocalLifetimeSchemaVersion
        && document.SemanticVersion == TaskLocalLifetimeSemanticVersion;

    public static string GenericInstanceId(string definitionId, IReadOnlyList<string> arguments)
    {
        string key = definitionId + "\n" + string.Join("\n", arguments);
        string digest = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key))).ToLowerInvariant();
        return "symbol:generic_instance:" + digest;
    }

    public static bool IsCurrentOrPrevious(int schemaVersion, string semanticVersion) =>
        (schemaVersion == 30 && semanticVersion is "1.34" or "1.35" or "1.36")
        || (schemaVersion == CurrentSchemaVersion
            && semanticVersion is ("1.37" or "1.38" or "1.39" or CurrentSemanticVersion))
        || (schemaVersion == TaskResultSchemaVersion
            && semanticVersion == TaskResultSemanticVersion)
        || (schemaVersion == TaskLocalSchemaVersion
            && semanticVersion == TaskLocalSemanticVersion)
        || (schemaVersion == TaskAssignmentSchemaVersion
            && semanticVersion == TaskAssignmentSemanticVersion)
        || (schemaVersion == TaskExistingLocalSchemaVersion
            && semanticVersion == TaskExistingLocalSemanticVersion)
        || (schemaVersion == TaskAliasSchemaVersion
            && semanticVersion == TaskAliasSemanticVersion)
        || (schemaVersion == TaskLanguageErrorSchemaVersion
            && semanticVersion == TaskLanguageErrorSemanticVersion)
        || (schemaVersion == AsyncLanguageErrorSchemaVersion
            && semanticVersion == AsyncLanguageErrorSemanticVersion)
        || (schemaVersion == AsyncExceptionFlowSchemaVersion
            && semanticVersion == AsyncExceptionFlowSemanticVersion)
        || (schemaVersion == DirectAwaitCleanupSchemaVersion
            && semanticVersion == DirectAwaitCleanupSemanticVersion)
        || (schemaVersion == AsyncCancellationFlowSchemaVersion
            && semanticVersion == AsyncCancellationFlowSemanticVersion)
        || (schemaVersion == TaskLocalLifetimeSchemaVersion
            && semanticVersion == TaskLocalLifetimeSemanticVersion);
}
