#include "BindingGeneration/AvidScriptEditorCSharpStateContractRenderer.h"

void FAvidScriptEditorCSharpStateContractRenderer::AppendReferenceSurface(
	TArray<FString>& Lines)
{
	Lines.Append({
		TEXT("public enum AvidStateMode"),
		TEXT("{"),
		TEXT("    Compatible = 0,"),
		TEXT("    Explicit = 1,"),
		TEXT("}"),
		TEXT(""),
		TEXT("[AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]"),
		TEXT("public sealed class AvidStateContractAttribute : Attribute"),
		TEXT("{"),
		TEXT("    public AvidStateContractAttribute(AvidStateMode mode = AvidStateMode.Compatible)"),
		TEXT("    {"),
		TEXT("        Mode = mode;"),
		TEXT("    }"),
		TEXT(""),
		TEXT("    public AvidStateMode Mode { get; }"),
		TEXT("    public int Version { get; set; } = 1;"),
		TEXT("}"),
		TEXT(""),
		TEXT("[AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = false)]"),
		TEXT("public sealed class AvidPersistAttribute : Attribute"),
		TEXT("{"),
		TEXT("}"),
		TEXT(""),
		TEXT("[AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = false)]"),
		TEXT("public sealed class AvidTransientAttribute : Attribute"),
		TEXT("{"),
		TEXT("}"),
		TEXT(""),
		TEXT("[AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = true)]"),
		TEXT("public sealed class AvidStateAliasAttribute : Attribute"),
		TEXT("{"),
		TEXT("    public AvidStateAliasAttribute(string formerName)"),
		TEXT("    {"),
		TEXT("        FormerName = formerName;"),
		TEXT("    }"),
		TEXT(""),
		TEXT("    public string FormerName { get; }"),
		TEXT("}"),
		TEXT("")
	});
}
