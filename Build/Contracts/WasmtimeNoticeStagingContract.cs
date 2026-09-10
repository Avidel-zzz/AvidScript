using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

// Only UBT's registration surface is stubbed; the actual Wasmtime.Build.cs is compiled unchanged.
namespace UnrealBuildTool
{
    public enum UnrealTargetPlatform { Win64, Android }
    public enum UnrealArch { Arm64, X64 }
    public enum TargetType { Editor, Game, Client, Server }
    public enum ModuleType { External }
    public enum StagedFileType { NonUFS }
    public class BuildException : Exception { public BuildException(string message) : base(message) { } }
    public class ReadOnlyTargetRules
    {
        public UnrealTargetPlatform Platform = UnrealTargetPlatform.Win64;
        public UnrealArch Architecture = UnrealArch.X64;
        public TargetType Type = TargetType.Game;
    }
    public sealed class Dependencies : List<(string Destination, string Source, StagedFileType Type)>
    {
        public void Add(string destination, string source, StagedFileType type) => Add((destination, source, type));
    }
    public class ModuleRules
    {
        public static string FixtureModuleDirectory;
        public string ModuleDirectory => FixtureModuleDirectory;
        public ModuleType Type;
        public List<string> PublicDefinitions = new List<string>();
        public List<string> ExternalDependencies = new List<string>();
        public List<string> PublicIncludePaths = new List<string>();
        public List<string> PublicAdditionalLibraries = new List<string>();
        public List<string> PublicDelayLoadDLLs = new List<string>();
        public Dependencies RuntimeDependencies = new Dependencies();
        public ModuleRules(ReadOnlyTargetRules target) { }
    }
}

internal static class NoticeStagingContract
{
    private sealed class Fixture
    {
        public string Root;
        public string Install;
        public string Bundle;
        public string Text;
        public JsonObject Manifest;
        public void Save() => File.WriteAllText(Path.Combine(Bundle, "manifest.json"), Manifest.ToJsonString());
        public void OmitBundle()
        {
            // Preserve the fixture in a sibling directory instead of deleting evidence.
            Directory.Move(Bundle, Bundle + ".omitted");
        }
    }
    private static readonly List<object> Results = new List<object>();
    private static int Passed;
    private static string TestRoot;
    private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }
    private static Fixture CreateFixture()
    {
        Fixture fixture = new Fixture { Root = Path.Combine(TestRoot, Guid.NewGuid().ToString("N")) };
        fixture.Install = Path.Combine(fixture.Root, "installed", "Win64", "v45.0.0-avidscript.2");
        fixture.Bundle = Path.Combine(fixture.Install, "notices");
        Directory.CreateDirectory(Path.Combine(fixture.Install, "include"));
        Directory.CreateDirectory(Path.Combine(fixture.Install, "lib"));
        Directory.CreateDirectory(Path.Combine(fixture.Bundle, "texts"));
        File.WriteAllText(Path.Combine(fixture.Install, "include", "wasmtime.h"), "fixture header");
        File.WriteAllText(Path.Combine(fixture.Install, "LICENSE"), "fixture root license");
        File.WriteAllText(Path.Combine(fixture.Install, ".avidscript-wasmtime-performance-managed.json"), "{}");
        JsonObject runtime = new JsonObject();
        foreach (string library in new[] { "wasmtime.dll", "wasmtime.dll.lib", "wasmtime.lib" })
        {
            string path = Path.Combine(fixture.Install, "lib", library);
            File.WriteAllText(path, "fixture " + library);
            runtime[library] = Hash(path);
        }
        string temporary = Path.Combine(fixture.Bundle, "texts", "temporary");
        File.WriteAllText(temporary, "Shared MIT fixture notice.");
        string sha = Hash(temporary);
        fixture.Text = Path.Combine(fixture.Bundle, "texts", sha + ".txt");
        File.Move(temporary, fixture.Text);
        JsonObject reference = new JsonObject
        {
            ["path"] = "texts/" + sha + ".txt", ["sha256"] = sha,
            ["length"] = new FileInfo(fixture.Text).Length, ["origin_path"] = "LICENSE"
        };
        fixture.Manifest = new JsonObject
        {
            ["schema_version"] = 1, ["scope"] = "win64-wasmtime-build-notices", ["package_count"] = 2,
            ["runtime"] = runtime,
            ["packages"] = new JsonArray(
                new JsonObject { ["id"] = "wasmtime:fixture@1", ["notices"] = new JsonArray(reference.DeepClone()) },
                new JsonObject { ["id"] = "crates-io:dependency@1", ["notices"] = new JsonArray(reference.DeepClone()) }),
            ["rust"] = new JsonObject { ["toolchain"] = "fixture", ["notices"] = new JsonArray(reference.DeepClone()) }
        };
        fixture.Save();
        return fixture;
    }
    private static Wasmtime Build(string root, UnrealBuildTool.TargetType type = UnrealBuildTool.TargetType.Game)
    {
        UnrealBuildTool.ModuleRules.FixtureModuleDirectory = root;
        return new Wasmtime(new UnrealBuildTool.ReadOnlyTargetRules { Type = type });
    }
    private static void CreateDirectoryLink(string link, string target)
    {
        if (!OperatingSystem.IsWindows()) { Directory.CreateSymbolicLink(link, target); return; }
        string script = Path.Combine(TestRoot, "CreateFixtureJunction.ps1");
        File.WriteAllText(script, "param($Link,$Target)\n$ErrorActionPreference='Stop'\nNew-Item -ItemType Junction -Path $Link -Target $Target | Out-Null\n");
        var start = new ProcessStartInfo("pwsh") { UseShellExecute = false, CreateNoWindow = true };
        foreach (string argument in new[] { "-NoProfile", "-File", script, "-Link", link, "-Target", target }) start.ArgumentList.Add(argument);
        using (Process process = Process.Start(start))
        {
            process.WaitForExit();
            Assert(process.ExitCode == 0, "junction fixture creation failed");
        }
    }
    private static void Run(string name, Action<Fixture> action)
    {
        try { action(CreateFixture()); ++Passed; Results.Add(new { name, passed = true }); Console.WriteLine("PASS " + name); }
        catch (Exception error) { Results.Add(new { name, passed = false, message = error.Message }); Console.WriteLine("FAIL " + name + ": " + error.Message); }
    }
    private static void Reject(string name, Action<Fixture> mutate, UnrealBuildTool.TargetType type = UnrealBuildTool.TargetType.Game)
    {
        Run(name, fixture =>
        {
            mutate(fixture);
            try { Build(fixture.Root, type); }
            catch (UnrealBuildTool.BuildException error)
            {
                Assert(error.Message.Contains("notices are missing or invalid"), "wrong failure category");
                return;
            }
            throw new Exception("invalid notice bundle accepted");
        });
    }
    public static int Main(string[] args)
    {
        TestRoot = args[0];
        Run("complete_deduplicated_nonufs_inventory", fixture =>
        {
            Wasmtime rules = Build(fixture.Root);
            var notices = rules.RuntimeDependencies.Where(item => item.Destination.Contains("wasmtime.notices/")).ToArray();
            Assert(notices.Length == 2, "expected exactly manifest plus deduplicated notice text");
            foreach (var notice in notices)
            {
                string relative = Path.GetRelativePath(fixture.Bundle, notice.Source).Replace('\\', '/');
                Assert(notice.Destination == "$(PluginDir)/Binaries/Win64/wasmtime.notices/" + relative, "destination is not stable");
                Assert(notice.Type == UnrealBuildTool.StagedFileType.NonUFS && rules.ExternalDependencies.Contains(notice.Source), "missing staging or rebuild dependency");
            }
        });
        Run("legacy_editor_remains_buildable", fixture => { fixture.OmitBundle(); Build(fixture.Root, UnrealBuildTool.TargetType.Editor); });
        Reject("game_requires_notices", fixture => fixture.OmitBundle());
        Reject("client_requires_notices", fixture => fixture.OmitBundle(), UnrealBuildTool.TargetType.Client);
        Reject("server_requires_notices", fixture => fixture.OmitBundle(), UnrealBuildTool.TargetType.Server);
        Reject("incomplete_editor_bundle_rejected", fixture => File.Move(fixture.Text, fixture.Text + ".missing"), UnrealBuildTool.TargetType.Editor);
        Reject("missing_manifest", fixture => File.Move(Path.Combine(fixture.Bundle, "manifest.json"), Path.Combine(fixture.Bundle, "removed.json")));
        Reject("unsupported_schema", fixture => { fixture.Manifest["schema_version"] = 2; fixture.Save(); });
        Reject("wrong_scope", fixture => { fixture.Manifest["scope"] = "other"; fixture.Save(); });
        Reject("package_count_mismatch", fixture => { fixture.Manifest["package_count"] = 3; fixture.Save(); });
        Reject("duplicate_package", fixture => { fixture.Manifest["packages"][1]["id"] = "wasmtime:fixture@1"; fixture.Save(); });
        Reject("empty_package_notices", fixture => { fixture.Manifest["packages"][0]["notices"] = new JsonArray(); fixture.Save(); });
        Reject("missing_rust_notices", fixture => { fixture.Manifest.Remove("rust"); fixture.Save(); });
        Reject("empty_rust_notices", fixture => { fixture.Manifest["rust"]["notices"] = new JsonArray(); fixture.Save(); });
        Reject("dll_identity_mismatch", fixture => File.AppendAllText(Path.Combine(fixture.Install, "lib", "wasmtime.dll"), "changed"));
        Reject("import_library_identity_mismatch", fixture => File.AppendAllText(Path.Combine(fixture.Install, "lib", "wasmtime.dll.lib"), "changed"));
        Reject("static_library_identity_mismatch", fixture => File.AppendAllText(Path.Combine(fixture.Install, "lib", "wasmtime.lib"), "changed"));
        Reject("tampered_notice", fixture => File.AppendAllText(fixture.Text, "changed"));
        Reject("wrong_notice_length", fixture => { fixture.Manifest["packages"][0]["notices"][0]["length"] = 1; fixture.Save(); });
        Reject("conflicting_shared_reference", fixture => { fixture.Manifest["rust"]["notices"][0]["sha256"] = new string('0', 64); fixture.Save(); });
        Reject("path_traversal", fixture => { fixture.Manifest["packages"][0]["notices"][0]["path"] = "../LICENSE"; fixture.Save(); });
        Reject("unreferenced_file", fixture => File.WriteAllText(Path.Combine(fixture.Bundle, "extra.txt"), "unexpected"));
        Reject("empty_file", fixture => File.WriteAllText(fixture.Text, ""));
        Reject("duplicate_json_property", fixture => File.WriteAllText(Path.Combine(fixture.Bundle, "manifest.json"), fixture.Manifest.ToJsonString().Insert(1, "\"schema_version\":1,")));
        Reject("reparse_bundle_root", fixture =>
        {
            Directory.Move(fixture.Bundle, fixture.Bundle + ".target");
            CreateDirectoryLink(fixture.Bundle, fixture.Bundle + ".target");
        });
        Reject("reparse_notice_directory", fixture =>
        {
            string target = Path.Combine(fixture.Root, "outside-texts");
            string texts = Path.Combine(fixture.Bundle, "texts");
            Directory.Move(texts, target);
            CreateDirectoryLink(texts, target);
        });
        if (args.Length > 1)
        {
            Run("actual_neutral_runtime_complete_inventory", fixture =>
            {
                Wasmtime rules = Build(args[1]);
                string bundle = Path.Combine(args[1], "installed", "Win64", "v45.0.0-avidscript.2", "notices");
                var notices = rules.RuntimeDependencies.Where(item => item.Destination.Contains("wasmtime.notices/")).ToArray();
                Assert(notices.Length == Directory.GetFiles(bundle, "*", SearchOption.AllDirectories).Length, "actual notice inventory mismatch");
            });
        }
        File.WriteAllText(Path.Combine(TestRoot, "results.json"), JsonSerializer.Serialize(new { passed = Passed, total = Results.Count, results = Results }, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"Wasmtime notice staging contracts: {Passed}/{Results.Count}");
        return Passed == Results.Count ? 0 : 1;
    }
}
