using System;
using System.Buffers.Binary;
using System.Globalization;

namespace AvidScript.GuestIr;

internal static class GuestConstantCodec
{
    private const NumberStyles IntegerStyle = NumberStyles.AllowLeadingSign;
    private const NumberStyles FloatStyle = NumberStyles.AllowLeadingSign | NumberStyles.AllowDecimalPoint
        | NumberStyles.AllowExponent;

    public static bool TryEncode(GuestConstant constant, GuestType targetType, out byte[] bytes)
    {
        bytes = new byte[targetType.Size];
        Span<byte> destination = bytes;
        switch (constant.Kind)
        {
            case "zero" when targetType.Kind != "void" && constant.Value is null:
                return true;
            case "bool" when IsIntegerTarget(targetType, 1)
                && constant.Value is "0" or "1":
                destination[0] = constant.Value == "1" ? (byte)1 : (byte)0;
                return true;
            case "int8" when IsIntegerTarget(targetType, 1)
                && sbyte.TryParse(constant.Value, IntegerStyle, CultureInfo.InvariantCulture, out sbyte int8):
                destination[0] = unchecked((byte)int8);
                return true;
            case "uint8" when IsIntegerTarget(targetType, 1)
                && byte.TryParse(constant.Value, IntegerStyle, CultureInfo.InvariantCulture, out byte uint8):
                destination[0] = uint8;
                return true;
            case "int16" when IsIntegerTarget(targetType, 2)
                && short.TryParse(constant.Value, IntegerStyle, CultureInfo.InvariantCulture, out short int16):
                BinaryPrimitives.WriteInt16LittleEndian(destination, int16);
                return true;
            case "uint16" when IsIntegerTarget(targetType, 2)
                && ushort.TryParse(constant.Value, IntegerStyle, CultureInfo.InvariantCulture, out ushort uint16):
                BinaryPrimitives.WriteUInt16LittleEndian(destination, uint16);
                return true;
            case "int32" when IsIntegerTarget(targetType, 4)
                && int.TryParse(constant.Value, IntegerStyle, CultureInfo.InvariantCulture, out int int32):
                BinaryPrimitives.WriteInt32LittleEndian(destination, int32);
                return true;
            case "uint32" or "address" when IsIntegerTarget(targetType, 4)
                && uint.TryParse(constant.Value, IntegerStyle, CultureInfo.InvariantCulture, out uint uint32):
                BinaryPrimitives.WriteUInt32LittleEndian(destination, uint32);
                return true;
            case "int64" when IsIntegerTarget(targetType, 8)
                && long.TryParse(constant.Value, IntegerStyle, CultureInfo.InvariantCulture, out long int64):
                BinaryPrimitives.WriteInt64LittleEndian(destination, int64);
                return true;
            case "uint64" when IsIntegerTarget(targetType, 8)
                && ulong.TryParse(constant.Value, IntegerStyle, CultureInfo.InvariantCulture, out ulong uint64):
                BinaryPrimitives.WriteUInt64LittleEndian(destination, uint64);
                return true;
            case "float32" when targetType.Size == 4 && targetType.Storage == "f32"
                && float.TryParse(constant.Value, FloatStyle, CultureInfo.InvariantCulture, out float float32)
                && float.IsFinite(float32):
                BinaryPrimitives.WriteInt32LittleEndian(destination, BitConverter.SingleToInt32Bits(float32));
                return true;
            case "float64" when targetType.Size == 8 && targetType.Storage == "f64"
                && double.TryParse(constant.Value, FloatStyle, CultureInfo.InvariantCulture, out double float64)
                && double.IsFinite(float64):
                BinaryPrimitives.WriteInt64LittleEndian(destination, BitConverter.DoubleToInt64Bits(float64));
                return true;
            case "null" when IsIntegerTarget(targetType, 4) && constant.Value is null:
                return true;
            default:
                bytes = Array.Empty<byte>();
                return false;
        }
    }

    private static bool IsIntegerTarget(GuestType type, int size)
    {
        return type.Size == size
            && type.Storage is "i32" or "i64"
            && type.Kind is "scalar" or "enum" or "string" or "array" or "handle";
    }
}
