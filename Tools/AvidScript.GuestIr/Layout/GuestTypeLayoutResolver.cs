using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal sealed class GuestTypeLayoutResolver
{
    private readonly Dictionary<string, GuestType> declarations;
    private readonly Dictionary<string, GuestType> layouts = new(StringComparer.Ordinal);
    private readonly HashSet<string> visiting = new(StringComparer.Ordinal);
    private readonly HashSet<string> failed = new(StringComparer.Ordinal);
    private readonly List<GuestDiagnostic> diagnostics = new();

    public GuestTypeLayoutResolver(IReadOnlyList<GuestType> types)
    {
        declarations = new Dictionary<string, GuestType>(StringComparer.Ordinal);
        foreach (GuestType type in types)
        {
            if (string.IsNullOrWhiteSpace(type.Id) || !declarations.TryAdd(type.Id, type))
            {
                Add("ASIR2002", $"Type id '{type.Id}' is empty or duplicated during layout.");
            }
        }
    }

    public GuestTypeLayoutResult Compute()
    {
        foreach (string typeId in declarations.Keys.OrderBy(id => id, StringComparer.Ordinal))
        {
            Resolve(typeId);
        }

        GuestDiagnostic[] orderedDiagnostics = diagnostics
            .OrderBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Message, StringComparer.Ordinal)
            .ToArray();
        bool succeeded = orderedDiagnostics.Length == 0;
        GuestType[] orderedTypes = succeeded
            ? layouts.Values.OrderBy(type => type.Id, StringComparer.Ordinal).ToArray()
            : Array.Empty<GuestType>();
        return new GuestTypeLayoutResult(succeeded, orderedTypes, orderedDiagnostics);
    }

    private GuestType? Resolve(string typeId)
    {
        if (layouts.TryGetValue(typeId, out GuestType? existing))
        {
            return existing;
        }

        if (failed.Contains(typeId))
        {
            return null;
        }

        if (!declarations.TryGetValue(typeId, out GuestType? declaration))
        {
            Add("ASIR2002", $"Type layout references unknown type '{typeId}'.");
            failed.Add(typeId);
            return null;
        }

        if (!visiting.Add(typeId))
        {
            Add("ASIR2002", $"Recursive value type '{typeId}' is not supported.");
            failed.Add(typeId);
            return null;
        }

        GuestType? layout;
        try
        {
            layout = declaration.Kind switch
            {
                "void" => LayoutVoid(declaration),
                "scalar" => LayoutScalar(declaration),
                "enum" => LayoutEnum(declaration),
                "struct" => LayoutStruct(declaration),
                "string" => LayoutReference(declaration, requireElement: false),
                "array" => LayoutReference(declaration, requireElement: true),
                "handle" => LayoutHandle(declaration),
                "class_ref" => LayoutClassReference(declaration),
                _ => InvalidShape(declaration, $"unsupported kind '{declaration.Kind}'"),
            };
        }
        catch (OverflowException)
        {
            Add("ASIR2001", $"Type '{typeId}' layout overflowed the 32-bit address space.");
            layout = null;
        }
        finally
        {
            visiting.Remove(typeId);
        }

        if (layout is null)
        {
            failed.Add(typeId);
            return null;
        }

        layouts[typeId] = layout;
        return layout;
    }

    private GuestType? LayoutVoid(GuestType declaration)
    {
        if (declaration.Fields.Count != 0)
        {
            return InvalidShape(declaration, "void type declares fields");
        }

        return declaration with { Storage = "none", Size = 0, Alignment = 1 };
    }

    private GuestType? LayoutScalar(GuestType declaration)
    {
        bool valid = declaration.Fields.Count == 0
            && GuestLayoutMath.IsValidAlignment(declaration.Alignment)
            && declaration.Alignment <= Math.Max(declaration.Size, 1)
            && declaration.Storage switch
            {
                "i32" => declaration.Size is 1 or 2 or 4,
                "i64" => declaration.Size == 8,
                "f32" => declaration.Size == 4,
                "f64" => declaration.Size == 8,
                _ => false,
            };
        return valid ? declaration : InvalidShape(declaration, "invalid scalar storage, size, or alignment");
    }

    private GuestType? LayoutEnum(GuestType declaration)
    {
        if (declaration.UnderlyingTypeId is null)
        {
            return InvalidShape(declaration, "enum has no underlying type");
        }

        GuestType? underlying = Resolve(declaration.UnderlyingTypeId);
        if (underlying is null
            || underlying.Kind is not ("scalar" or "enum")
            || underlying.Storage is not ("i32" or "i64"))
        {
            return InvalidShape(declaration, "enum underlying type is not an integer type");
        }

        return declaration with
        {
            Storage = underlying.Storage,
            Fields = Array.Empty<GuestField>(),
            Size = underlying.Size,
            Alignment = underlying.Alignment,
        };
    }

    private GuestType? LayoutStruct(GuestType declaration)
    {
        if (!string.Equals(declaration.Storage, "memory", StringComparison.Ordinal))
        {
            return InvalidShape(declaration, "struct storage is not memory");
        }

        int offset = 0;
        int alignment = 1;
        GuestField[] fields = new GuestField[declaration.Fields.Count];
        for (int index = 0; index < declaration.Fields.Count; ++index)
        {
            GuestField field = declaration.Fields[index];
            GuestType? fieldType = Resolve(field.TypeId);
            if (fieldType is null)
            {
                return null;
            }

            offset = GuestLayoutMath.AlignUp(offset, fieldType.Alignment);
            fields[index] = field with { Offset = offset };
            offset = GuestLayoutMath.Add(offset, fieldType.Size);
            alignment = Math.Max(alignment, fieldType.Alignment);
        }

        int size = GuestLayoutMath.AlignUp(offset, alignment);
        return declaration with { Fields = fields, Size = size, Alignment = alignment };
    }

    private GuestType? LayoutReference(GuestType declaration, bool requireElement)
    {
        if (declaration.Fields.Count != 0
            || (requireElement
                && (declaration.ElementTypeId is null || !declarations.ContainsKey(declaration.ElementTypeId))))
        {
            return InvalidShape(declaration, "reference type has invalid fields or element type");
        }

        return declaration with { Storage = "i32", Size = 4, Alignment = 4 };
    }

    private GuestType? LayoutHandle(GuestType declaration)
    {
        if (declaration.Fields.Count != 0)
        {
            return InvalidShape(declaration, "handle type declares fields");
        }

        return declaration with { Storage = "i64", Size = 8, Alignment = 8 };
    }

    private GuestType? LayoutClassReference(GuestType declaration)
    {
        if (declaration.Fields.Count != 0
            || declaration.ElementTypeId is not null
            || declaration.UnderlyingTypeId is not null)
        {
            return InvalidShape(declaration, "class reference type has fields or related type metadata");
        }

        return declaration with { Storage = "i32", Size = 4, Alignment = 4 };
    }

    private GuestType? InvalidShape(GuestType declaration, string reason)
    {
        Add("ASIR2003", $"Type '{declaration.Id}' has {reason}.");
        return null;
    }

    private void Add(string code, string message)
    {
        diagnostics.Add(new GuestDiagnostic(code, "error", message, null));
    }
}
