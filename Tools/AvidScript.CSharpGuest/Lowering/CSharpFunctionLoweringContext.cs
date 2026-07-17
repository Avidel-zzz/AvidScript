using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed class CSharpFunctionLoweringContext
{
    private readonly IReadOnlyDictionary<string, GuestType> guestTypes;
    private readonly Dictionary<string, GuestRegister> storageBySymbol = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> globalBySymbol = new(StringComparer.Ordinal);
    private readonly Dictionary<string, SemanticCallable> callablesBySymbol;
    private readonly Dictionary<string, SemanticCallableParameter> parametersBySymbol = new(StringComparer.Ordinal);
    private readonly Dictionary<string, GuestRegister> capturesById = new(StringComparer.Ordinal);
    private readonly Dictionary<string, CaptureAddressTarget> captureAddressTargetsById = new(StringComparer.Ordinal);
    private readonly List<GuestRegister> locals = new();
    private int nextTemporaryOrdinal;

    public CSharpFunctionLoweringContext(
        SemanticDocument document,
        SemanticCallable callable,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        CSharpGuestDataPool dataPool,
        IReadOnlyList<GuestRegister> parameters,
        List<GuestDiagnostic> diagnostics)
    {
        Document = document;
        Callable = callable;
        this.guestTypes = guestTypes;
        DataPool = dataPool;
        Diagnostics = diagnostics;
        callablesBySymbol = document.Callables
            .GroupBy(item => item.MethodSymbolId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        ThisRegister = callable.IsStatic ? null : parameters[0];

        foreach (SemanticCallableParameter parameter in callable.Parameters)
        {
            GuestRegister register = parameters.Single(item =>
                item.Id == CSharpGuestIds.Parameter(parameter.SymbolId));
            storageBySymbol.Add(parameter.SymbolId, register);
            parametersBySymbol.Add(parameter.SymbolId, parameter);
        }

        foreach (SemanticSymbol symbol in document.Symbols
            .Where(item => item.Kind == "local"
                && string.Equals(item.ContainingSymbolId, callable.MethodSymbolId, StringComparison.Ordinal))
            .OrderBy(item => item.Id, StringComparer.Ordinal))
        {
            if (symbol.TypeId is null || !guestTypes.ContainsKey(symbol.TypeId))
            {
                Add("ASCG1004", $"Local '{symbol.Id}' has no Guest value type.");
                continue;
            }

            GuestRegister register = new(CSharpGuestIds.Local(symbol.Id), symbol.TypeId);
            storageBySymbol.Add(symbol.Id, register);
            locals.Add(register);
        }

        foreach (SemanticSymbol symbol in document.Symbols
            .Where(item => item.Kind == "field" && item.IsStatic)
            .OrderBy(item => item.Id, StringComparer.Ordinal))
        {
            globalBySymbol.TryAdd(symbol.Id, CSharpGuestIds.Global(symbol.Id));
        }
    }

    public SemanticDocument Document { get; }

    public SemanticCallable Callable { get; }

    public List<GuestDiagnostic> Diagnostics { get; }

    public CSharpGuestDataPool DataPool { get; }

    public GuestRegister? ThisRegister { get; }

    public IReadOnlyList<GuestRegister> Locals => locals;

    public GuestRegister? CreateTemporary(string? typeId, int blockOrdinal)
    {
        if (typeId is null || !guestTypes.ContainsKey(typeId))
        {
            Add("ASCG1004", $"Block {blockOrdinal} expression has no Guest value type '{typeId}'.");
            return null;
        }

        GuestRegister temporary = new(
            CSharpGuestIds.Temporary(Callable.MethodSymbolId, blockOrdinal, nextTemporaryOrdinal++),
            typeId);
        locals.Add(temporary);
        return temporary;
    }

    public bool TryGetStorage(string? symbolId, out GuestRegister register)
    {
        if (symbolId is not null && storageBySymbol.TryGetValue(symbolId, out GuestRegister? found))
        {
            register = found;
            return true;
        }

        register = null!;
        return false;
    }

    public bool TryGetGlobal(string? symbolId, out string globalId)
    {
        if (symbolId is not null && globalBySymbol.TryGetValue(symbolId, out string? found))
        {
            globalId = found;
            return true;
        }

        globalId = null!;
        return false;
    }

    public bool TryGetGuestType(string? typeId, out GuestType type)
    {
        if (typeId is not null && guestTypes.TryGetValue(typeId, out GuestType? found))
        {
            type = found;
            return true;
        }

        type = null!;
        return false;
    }

    public bool TryGetParameter(string? symbolId, out SemanticCallableParameter parameter)
    {
        if (symbolId is not null
            && parametersBySymbol.TryGetValue(symbolId, out SemanticCallableParameter? found))
        {
            parameter = found;
            return true;
        }

        parameter = null!;
        return false;
    }

    public void TrackCaptureAddressTarget(
        string? captureId,
        SemanticOperation source,
        int blockOrdinal)
    {
        if (captureId is null)
        {
            return;
        }

        SemanticOperation target = source.Kind is "argument" or "declaration_expression"
            && source.Children.Count == 1
                ? source.Children[0]
                : source;
        if (target.Kind is not ("local_reference" or "parameter_reference")
            || !TryGetStorage(target.SymbolId, out GuestRegister storage))
        {
            return;
        }

        bool isAlreadyAddress = target.Kind == "parameter_reference"
            && TryGetParameter(target.SymbolId, out SemanticCallableParameter parameter)
            && parameter.RefKind != "none";
        CaptureAddressTarget candidate = new(storage, isAlreadyAddress);
        if (captureAddressTargetsById.TryGetValue(captureId, out CaptureAddressTarget? existing)
            && existing != candidate)
        {
            Add("ASCG1004", $"Block {blockOrdinal} flow capture '{captureId}' changed address target.");
            return;
        }

        captureAddressTargetsById.TryAdd(captureId, candidate);
    }

    public bool TryGetCaptureAddressTarget(
        string? captureId,
        out GuestRegister storage,
        out bool isAlreadyAddress)
    {
        if (captureId is not null
            && captureAddressTargetsById.TryGetValue(captureId, out CaptureAddressTarget? target))
        {
            storage = target.Storage;
            isAlreadyAddress = target.IsAlreadyAddress;
            return true;
        }

        storage = null!;
        isAlreadyAddress = false;
        return false;
    }
    public GuestRegister? GetOrCreateCapture(string? captureId, string? typeId, int blockOrdinal)
    {
        if (captureId is null)
        {
            Add("ASCG1004", $"Block {blockOrdinal} flow capture has no identity.");
            return null;
        }

        if (capturesById.TryGetValue(captureId, out GuestRegister? existing))
        {
            if (!string.Equals(existing.TypeId, typeId, StringComparison.Ordinal))
            {
                Add("ASCG1004", $"Block {blockOrdinal} flow capture '{captureId}' changed type.");
                return null;
            }

            return existing;
        }

        if (typeId is null || !guestTypes.ContainsKey(typeId))
        {
            Add("ASCG1004", $"Block {blockOrdinal} flow capture '{captureId}' has no Guest type.");
            return null;
        }

        GuestRegister capture = new(CSharpGuestIds.Capture(Callable.MethodSymbolId, captureId), typeId);
        capturesById.Add(captureId, capture);
        locals.Add(capture);
        return capture;
    }

    public bool TryGetPropertyGetter(string? propertySymbolId, out SemanticCallable callable, out string targetId)
    {
        SemanticCallable[] matches = callablesBySymbol.Values
            .Where(item => string.Equals(item.AssociatedSymbolId, propertySymbolId, StringComparison.Ordinal)
                && !string.Equals(item.ReturnTypeId, "type:void", StringComparison.Ordinal)
                && (item.Import is not null || item.HasBody))
            .OrderBy(item => item.MethodSymbolId, StringComparer.Ordinal)
            .ToArray();
        if (matches.Length == 1)
        {
            callable = matches[0];
            targetId = callable.Import is not null
                ? CSharpGuestIds.Import(callable.MethodSymbolId)
                : CSharpGuestIds.Function(callable.MethodSymbolId);
            return true;
        }

        callable = null!;
        targetId = null!;
        return false;
    }

    public bool TryGetCallTarget(string? methodSymbolId, out SemanticCallable callable, out string targetId)
    {
        if (methodSymbolId is not null
            && callablesBySymbol.TryGetValue(methodSymbolId, out SemanticCallable? found)
            && (found.Import is not null || found.HasBody))
        {
            callable = found;
            targetId = found.Import is not null
                ? CSharpGuestIds.Import(found.MethodSymbolId)
                : CSharpGuestIds.Function(found.MethodSymbolId);
            return true;
        }

        callable = null!;
        targetId = null!;
        return false;
    }

    public bool TryLowerConstant(
        string? typeId,
        SemanticConstant semanticConstant,
        out GuestConstant constant)
    {
        string kind = semanticConstant.Kind;
        if (typeId is not null
            && guestTypes.TryGetValue(typeId, out GuestType? type)
            && type.Kind == "enum"
            && type.UnderlyingTypeId is not null)
        {
            SemanticType? underlying = Document.Types.SingleOrDefault(
                item => item.Id == type.UnderlyingTypeId);
            if (underlying is null)
            {
                constant = null!;
                return false;
            }

            kind = underlying.CanonicalName;
        }

        string? value = kind == "bool"
            ? semanticConstant.Value switch
            {
                "true" => "1",
                "false" => "0",
                _ => semanticConstant.Value,
            }
            : semanticConstant.Value;
        constant = new GuestConstant(kind, value);
        return true;
    }
    private sealed record CaptureAddressTarget(GuestRegister Storage, bool IsAlreadyAddress);

    public void Add(string code, string message)
    {
        Diagnostics.Add(new GuestDiagnostic(
            code,
            "error",
            $"{Callable.MethodSymbolId}: {message}",
            null));
    }
}
