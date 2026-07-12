using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.CSharpFrontend;

internal static class AstContractTests
{
    public static void Run()
    {
        ActorLifecycleProducesStructuredAst();
        UnsupportedSyntaxIsPreserved();
        PreambleAndGenericShapeIsPreserved();
    }

    private static void ActorLifecycleProducesStructuredAst()
    {
        string sourcePath = Path.Combine(
            Directory.GetCurrentDirectory(),
            "Samples",
            "CSharp",
            "ActorLifecycle",
            "ActorLifecycleScript.cs");
        string source = File.ReadAllText(sourcePath);
        FrontendDocument document = FrontendAnalyzer.Analyze(source, "Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs");

        FrontendDeclaration scriptType = FindDeclaration(document.Syntax.Declarations, "ActorLifecycleScript");
        Assert(scriptType.Kind == "ClassDeclaration", "ActorLifecycleScript should be a class declaration");
        Assert(scriptType.Modifiers.SequenceEqual(new[] { "public", "static" }), "class modifiers should preserve source order");

        FrontendDeclaration elapsedField = FindDeclaration(scriptType.Members, "ElapsedSeconds");
        Assert(elapsedField.Kind == "FieldDeclaration" && elapsedField.Type == "float", "ElapsedSeconds should be a typed field");

        FrontendDeclaration tick = FindDeclaration(scriptType.Members, "Tick");
        Assert(tick.Kind == "MethodDeclaration" && tick.Type == "void", "Tick should retain its return type");
        Assert(tick.Parameters.Count == 1, "Tick should have one parameter");
        Assert(tick.Parameters[0].Name == "deltaSeconds" && tick.Parameters[0].Type == "float", "Tick parameter should be structured");
        FrontendAstNode tickBody = tick.Body
            ?? throw new InvalidOperationException("Tick body is missing");
        Assert(tickBody is { Kind: "Block", IsSupported: true }, "Tick should contain a supported block body");
        Assert(tickBody.Children.Any(node => node.Kind == "LocalDeclarationStatement" && node.Type == "FVector" && node.Name == "currentLocation"), "Tick should retain typed local declarations");
        Assert(Descendants(tickBody).Any(node => node.Kind == "AddAssignmentExpression" && node.Operator == "+="), "Tick should retain assignment operator structure");
        Assert(Descendants(tickBody).Any(node => node.Kind == "InvocationExpression" && node.Name == "SetActorLocation"), "Tick should retain invocation target names");

        FrontendDeclaration onInput = FindDeclaration(scriptType.Members, "OnInput");
        Assert(onInput.Parameters.Single().Type == "InputEvent", "OnInput should retain its generated event type");

        AssertDeclarationSpans(document.Syntax.Span, document.Syntax.Declarations);

        using JsonDocument json = JsonDocument.Parse(FrontendSerializer.Serialize(document));
        JsonElement syntaxJson = json.RootElement.GetProperty("syntax");
        Assert(syntaxJson.GetProperty("declarations").GetArrayLength() > 0, "serialized AST should expose declarations");
        Assert(syntaxJson.GetProperty("preamble").ValueKind == JsonValueKind.Array, "serialized AST should expose preamble nodes");
    }

    private static void UnsupportedSyntaxIsPreserved()
    {
        const string source = "class Script { void Tick() { lock (this) { } } }";
        FrontendDocument document = FrontendAnalyzer.Analyze(source, "Scripts/Unsupported.cs");
        FrontendDeclaration tick = FindDeclaration(
            FindDeclaration(document.Syntax.Declarations, "Script").Members,
            "Tick");
        FrontendAstNode unsupported = Descendants(tick.Body).First(node => node.Kind == "LockStatement");

        Assert(!unsupported.IsSupported, "unsupported syntax should be explicit");
        string unsupportedText = unsupported.Text
            ?? throw new InvalidOperationException("unsupported syntax text is missing");
        Assert(unsupportedText == "lock (this) { }", "unsupported syntax should preserve source text");
        Assert(unsupported.Span.Length == unsupportedText.Length, "unsupported syntax should retain its source span");
    }

    private static void PreambleAndGenericShapeIsPreserved()
    {
        const string source = "using Alias = System.String;\n[assembly: System.CLSCompliant(true)]\nclass Script<T> : Base<T>, IFace where T : class\n{\n    void IFace.Run<U>(U value) where U : struct { }\n    void Tick() { using var item = new Item { Value = 1 }; }\n}\n";
        FrontendDocument document = FrontendAnalyzer.Analyze(source, "Scripts/Generic.cs");
        FrontendDeclaration script = FindDeclaration(document.Syntax.Declarations, "Script");
        FrontendDeclaration run = FindDeclaration(script.Members, "Run");
        FrontendDeclaration tick = FindDeclaration(script.Members, "Tick");

        Assert(document.Syntax.Preamble.Any(node => node.Kind == "UsingDirective"), "compilation-unit using directives should not be dropped");
        Assert(document.Syntax.Preamble.Any(node => node.Kind == "AttributeList"), "compilation-unit attributes should not be dropped");
        Assert(script.TypeParameters.SequenceEqual(new[] { "T" }), "type parameters should be structured");
        Assert(script.BaseTypes.SequenceEqual(new[] { "Base<T>", "IFace" }), "base types and interfaces should be structured");
        Assert(script.Constraints.SequenceEqual(new[] { "where T : class" }), "type constraints should be retained");
        Assert(run.ExplicitInterface == "IFace", "explicit interface method qualifier should be retained");
        Assert(run.TypeParameters.SequenceEqual(new[] { "U" }), "method type parameters should be retained");
        Assert(run.Constraints.SequenceEqual(new[] { "where U : struct" }), "method constraints should be retained");

        FrontendAstNode tickBody = tick.Body
            ?? throw new InvalidOperationException("generic sample Tick body is missing");
        FrontendAstNode local = tickBody.Children.Single(node => node.Kind == "LocalDeclarationStatement");
        Assert(local.Modifiers.Contains("using"), "using local modifier should be retained");
        Assert(Descendants(local).Any(node => node.Kind == "ObjectInitializerExpression"), "object creation initializer should be retained");
    }

    private static FrontendDeclaration FindDeclaration(IEnumerable<FrontendDeclaration> declarations, string name)
    {
        FrontendDeclaration? result = TryFindDeclaration(declarations, name);
        return result ?? throw new InvalidOperationException($"declaration not found: {name}");
    }

    private static FrontendDeclaration? TryFindDeclaration(IEnumerable<FrontendDeclaration> declarations, string name)
    {
        foreach (FrontendDeclaration declaration in declarations)
        {
            if (declaration.Name == name)
            {
                return declaration;
            }

            FrontendDeclaration? nested = TryFindDeclaration(declaration.Members, name);
            if (nested is not null)
            {
                return nested;
            }
        }

        return null;
    }

    private static IEnumerable<FrontendAstNode> Descendants(FrontendAstNode? node)
    {
        if (node is null)
        {
            yield break;
        }

        yield return node;
        foreach (FrontendAstNode child in node.Children)
        {
            foreach (FrontendAstNode descendant in Descendants(child))
            {
                yield return descendant;
            }
        }
    }

    private static void AssertDeclarationSpans(FrontendSpan parent, IEnumerable<FrontendDeclaration> declarations)
    {
        foreach (FrontendDeclaration declaration in declarations)
        {
            AssertContains(parent, declaration.Span, declaration.Kind);
            foreach (FrontendParameter parameter in declaration.Parameters)
            {
                AssertContains(declaration.Span, parameter.Span, $"parameter {parameter.Name}");
                if (parameter.DefaultValue is not null)
                {
                    AssertNodeSpans(parameter.Span, parameter.DefaultValue);
                }
            }

            foreach (FrontendAttribute attribute in declaration.Attributes)
            {
                AssertContains(declaration.Span, attribute.Span, $"attribute {attribute.Name}");
                foreach (FrontendAstNode argument in attribute.Arguments)
                {
                    AssertNodeSpans(attribute.Span, argument);
                }
            }

            if (declaration.Initializer is not null)
            {
                AssertNodeSpans(declaration.Span, declaration.Initializer);
            }

            if (declaration.Body is not null)
            {
                AssertNodeSpans(declaration.Span, declaration.Body);
            }

            AssertDeclarationSpans(declaration.Span, declaration.Members);
        }
    }

    private static void AssertNodeSpans(FrontendSpan parent, FrontendAstNode node)
    {
        AssertContains(parent, node.Span, node.Kind);
        foreach (FrontendAstNode child in node.Children)
        {
            AssertNodeSpans(node.Span, child);
        }
    }

    private static void AssertContains(FrontendSpan parent, FrontendSpan child, string label)
    {
        Assert(child.Start >= parent.Start && child.End <= parent.End, $"span should be nested: {label}");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
