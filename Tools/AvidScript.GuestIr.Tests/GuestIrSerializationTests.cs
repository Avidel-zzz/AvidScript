using System;
using System.IO;
using System.Linq;
using AvidScript.GuestIr;

internal static class GuestIrSerializationTests
{
    public static int Run()
    {
        SerializationIsDeterministicAndRoundTrips();
        ArtifactWriterValidatesAndAtomicallyReplaces();
        return 2;
    }

    private static void SerializationIsDeterministicAndRoundTrips()
    {
        GuestModule module = GuestModuleValidationTests.CreateMinimalModule();

        byte[] first = GuestIrSerializer.Serialize(module);
        byte[] second = GuestIrSerializer.Serialize(module);
        GuestModule restored = GuestIrSerializer.Deserialize(first);
        byte[] restoredBytes = GuestIrSerializer.Serialize(restored);

        Assert(first.SequenceEqual(second), "Guest IR serialization should be byte deterministic");
        Assert(first.SequenceEqual(restoredBytes), "Guest IR round-trip should preserve canonical bytes");
        Assert(first.Length > 0 && first[^1] == (byte)'\n', "Guest IR JSON should end with LF");
        Assert(restored.ModuleId == module.ModuleId && restored.Functions.Count == 1,
            "Guest IR round-trip should preserve module data");
    }

    private static void ArtifactWriterValidatesAndAtomicallyReplaces()
    {
        string path = Path.Combine(AppContext.BaseDirectory, "writer-test.guestir.json");
        GuestModule validModule = GuestModuleValidationTests.CreateMinimalModule();
        byte[] expected = GuestIrSerializer.Serialize(validModule);

        try
        {
            GuestIrArtifactWriter.Write(path, validModule);
            Assert(File.ReadAllBytes(path).SequenceEqual(expected),
                "Guest IR writer should persist canonical bytes");

            GuestModule invalidModule = validModule with
            {
                Provenance = validModule.Provenance with { SemanticSha256 = string.Empty },
            };
            bool rejected = false;
            try
            {
                GuestIrArtifactWriter.Write(path, invalidModule);
            }
            catch (InvalidDataException)
            {
                rejected = true;
            }

            Assert(rejected, "Guest IR writer should reject invalid modules");
            Assert(File.ReadAllBytes(path).SequenceEqual(expected),
                "failed Guest IR writes should preserve the previous artifact");
        }
        finally
        {
            File.Delete(path);
        }
    }
    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
