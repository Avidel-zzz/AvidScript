using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestEventStateTests
{
    private const string Id = "6666666666666666666666666666666666666666666666666666666666666666";
    private const string Facade = """
        using System;
        using System.Runtime.InteropServices;
        using System.Runtime.CompilerServices;
        namespace AvidScript;
        [AttributeUsage(AttributeTargets.Field)]
        internal sealed class AvidEventContractAttribute : Attribute {
            public AvidEventContractAttribute(string id, string types, string directions, string result) { }
        }
        [AttributeUsage(AttributeTargets.Method)]
        internal sealed class AvidEventSubscriptionAttribute : Attribute {
            public AvidEventSubscriptionAttribute(string id, int ordinal) { }
        }
        public delegate void Handler(float value);
        public static class Events {
            [AvidEventContract("ID", "global::System.Single", "none", "global::System.Void")]
            public const string Signal = "ID";
            [AvidEventSubscription(Signal, 7)]
            [MethodImpl(MethodImplOptions.InternalCall)]
            public static extern long Subscribe(int slot, int generation, Handler handler);
            [DllImport("avidscript", EntryPoint="event_unsubscribe")]
            public static extern int Cancel(long token);
            [DllImport("avidscript", EntryPoint="owner_get_slot")]
            public static extern int Slot();
            [DllImport("avidscript", EntryPoint="owner_get_generation")]
            public static extern int Generation();
        }
        """;

    public static int Run()
    {
        int count = 0;
        foreach (string kind in new[] { "capture", "bound", "static", "list", "null", "refout" })
        {
            string setup = kind switch {
                "capture" => "int score = 40; Handler handler = value => { score += 2; Result = score; Count++; if (value < 0) Events.Cancel(Token); };",
                "bound" => "Counter counter = new Counter(); Handler handler = counter.Change;",
                "static" => "Result = 40; Handler handler = Change;",
                "list" => "int score = 40; Handler a = value => { score += 2; Result = score; Count++; }; Handler b = value => { score *= 2; Result = score; Count++; if (value < 0) Events.Cancel(Token); }; Handler handler = a + b;",
                "null" => "Handler handler = null;",
                _ => "int score = 40; Handler handler = (ref int value, out int doubled) => { score += value; value = score; doubled = score * 2; Result = score; Count++; return doubled + 1; };",
            };
            string source = """
                using AvidScript;
                using System.Runtime.InteropServices;
                public sealed class Counter {
                    public int Value; public Counter() { Value = 40; }
                    public void Change(float value) { Value += 2; Script.Result = Value; Script.Count++; if (value < 0) Events.Cancel(Script.Token); }
                }
                public static class Script {
                    public static int Count; public static int Result; public static long Token;
                    static void Change(float value) { Result += 2; Count++; if (value < 0) Events.Cancel(Token); }
                    [UnmanagedCallersOnly(EntryPoint="avid_on_begin_play")] public static void Begin() { }
                    [UnmanagedCallersOnly(EntryPoint="avid_on_tick")] public static void Tick(float delta) { SETUP Token = Events.Subscribe(Events.Slot(), Events.Generation(), handler); }
                    [UnmanagedCallersOnly(EntryPoint="cancel")] public static int Cancel() => Events.Cancel(Token);
                }
                """.Replace("SETUP", setup);
            string facade = Facade.Replace("ID", Id);
            if (kind == "refout") facade = facade.Replace("void Handler(float value)", "int Handler(ref int value, out int doubled)")
                .Replace("\"global::System.Single\", \"none\", \"global::System.Void\"", "\"global::System.Int32;global::System.Int32\", \"ref;out\", \"global::System.Int32\"");
            SemanticDocument document = Analyze(source, facade);
            Require(document.Succeeded, string.Join(" | ", document.Diagnostics.Select(d => d.Message)));
            Require(document.SchemaVersion == 30 && document.SemanticVersion == "1.34"
                && SemanticEventSubscriptionValidator.IsValid(document), "versioned event contract");
            Require(SemanticSerializer.Serialize(document).SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document)))), "canonical round trip");
            var lowered = CSharpGuestLowerer.Lower(document, new string('a', 64));
            Require(lowered.Succeeded, kind + ": " + string.Join(" | ", lowered.Diagnostics.Select(d => d.Message)));
            GuestModule module = lowered.Module!;
            Require(module.Exports.Any(e => e.Name == "avid_on_delegate_6666666666666666_state_v1"), "typed callback export");
            Require(module.MemoryLayout.StateSlots.Select(slot => slot.Offset).SequenceEqual(new[] { 16, 20, 24, 32 }), "observable state layout: " + string.Join(",", module.MemoryLayout.StateSlots.Select(slot => slot.ToString())));
            var stressed = module with { Functions = module.Functions.Select(function => function with {
                Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(i =>
                    i.Op is "managed_new" or "managed_event_read" ? new[] { i, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { i }).ToArray() }).ToArray() }).ToArray() };
            var wasm = WasmModuleCompiler.Compile(stressed);
            Require(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(d => d.Message)));
            Require(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(stressed).Bytes), "deterministic typed event compilation");
            string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
            if (!string.IsNullOrEmpty(directory)) {
                Directory.CreateDirectory(directory);
                File.WriteAllBytes(Path.Combine(directory, "csharp-event-" + kind + ".wasm"), wasm.Bytes);
                File.WriteAllBytes(Path.Combine(directory, "csharp-event-" + kind + ".guest.json"), GuestIrSerializer.Serialize(stressed));
            }
            if (kind == "capture") {
                var entry = document.EventSubscriptions.Single();
                foreach (var bad in new[] { entry with { SubscriptionId = "bad" }, entry with { EventOrdinal = -1 },
                    entry with { MethodSymbolId = "missing" }, entry with { DelegateTypeId = "type:int32" } })
                    Require(!CSharpGuestLowerer.Lower(document with { EventSubscriptions = new[] { bad } }, new string('a', 64)).Succeeded, "tampered subscription rejected");
                Require(!CSharpGuestLowerer.Lower(document with { SchemaVersion = 28, SemanticVersion = "1.32" }, new string('a', 64)).Succeeded, "old schema cannot carry new contracts");
                var prior = CSharpGuestLowerer.Lower(document with { SchemaVersion = 29, SemanticVersion = "1.33" }, new string('a', 64));
                Require(prior.Succeeded, "schema 29 typed subscription compatibility retained: " +
                    string.Join(" | ", prior.Diagnostics.Select(d => d.Message)));
                Require(!Analyze(source, facade.Replace("Signal, 7", "Signal, -1")).Succeeded, "negative ordinal rejected in source");
                Require(!Analyze(source, facade.Replace("global::System.Single", "global::System.Int32")).Succeeded, "signature mismatch rejected in source");
                Require(!Analyze(source, facade.Replace("public static extern long Subscribe", "public static extern int Subscribe")).Succeeded, "subscription return contract rejected");
                count += 9;
            }
            count++;
        }
        const string legacySource = "using System; using System.Runtime.InteropServices; public static class Script { static int Value() => 4; [UnmanagedCallersOnly(EntryPoint=\"run\")] public static int Run() { Func<int> handler = Value; return handler(); } }";
        var legacy = Analyze(legacySource, "") with { SchemaVersion = 28, SemanticVersion = "1.32" };
        Require(CSharpGuestLowerer.Lower(legacy, new string('a', 64)).Succeeded, "Semantic 28 static delegate compatibility retained");
        return count + 1;
    }

    public static int RunGeneratedFacade()
    {
        string directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR")
            ?? throw new InvalidOperationException("Set AVIDSCRIPT_MANAGED_HEAP_WASM_DIR to the native fixture directory.");
        string facade = File.ReadAllText(Path.Combine(directory, "event-facade.generated.cs"));
        foreach (string kind in new[] { "capture", "bound", "static", "list", "null", "refout", "singlecast" })
        {
            string callback = kind switch {
                "refout" => "Subscription = AvidSubscriptions.SubscribeOnRefOutSignal(UE.Self, (ref int value, out int doubled) => { score += value; value = score; doubled = score * 2; Count++; Result = score; });",
                "singlecast" => "Subscription = AvidSubscriptions.BindOnSinglecastSignal(UE.Self, (ref int value, out int doubled) => { score += value; value = score; doubled = score * 2; Count++; Result = score; return doubled + 1; });",
                "bound" => "Counter counter = new Counter(); Subscription = AvidSubscriptions.SubscribeOnScriptSignal(UE.Self, counter.Change);",
                "static" => "Result = 40; Subscription = AvidSubscriptions.SubscribeOnScriptSignal(UE.Self, Change);",
                "list" => "AvidEventHandlers.OnScriptSignal first = (actor, amount, scale) => { score += amount; Count++; }; AvidEventHandlers.OnScriptSignal last = (actor, amount, scale) => { score *= 2; Result = score; Count++; if (scale < 0) Subscription.Cancel(); }; Subscription = AvidSubscriptions.SubscribeOnScriptSignal(UE.Self, first + last);",
                "null" => "Subscription = AvidSubscriptions.SubscribeOnScriptSignal(UE.Self, null);",
                _ => "Subscription = AvidSubscriptions.SubscribeOnScriptSignal(UE.Self, (actor, amount, scale) => { score += amount; Result = score; Count++; if (scale < 0) Subscription.Cancel(); });",
            };
            string source = """
                using AvidScript;
                using System.Runtime.InteropServices;
                public sealed class Counter {
                    public int Value; public Counter() { Value = 40; }
                    public void Change(AActor actor, int amount, float scale) { Value += amount; Script.Result = Value; Script.Count++; if (scale < 0) Script.Subscription.Cancel(); }
                }
                public static class Script {
                    public static int Count; public static int Result; [AvidTransient] public static AvidSubscription Subscription;
                    static void Change(AActor actor, int amount, float scale) { Result += amount; Count++; if (scale < 0) Subscription.Cancel(); }
                    [UnmanagedCallersOnly(EntryPoint="avid_on_begin_play")] public static void Begin() { }
                    [UnmanagedCallersOnly(EntryPoint="avid_on_tick")] public static void Tick(float delta) { int score = 40; CALLBACK }
                    [UnmanagedCallersOnly(EntryPoint="cancel")] public static void Cancel() { Subscription.Cancel(); }
                }
                """.Replace("CALLBACK", callback);
            var semantic = Analyze(source, facade);
            Require(semantic.Succeeded, string.Join(" | ", semantic.Diagnostics.Select(d => d.Message)));
            Require(semantic.EventSubscriptions.Any(entry => entry.EventSymbolId is not null),
                "production generated facade should expose validated language event metadata");
            var lowered = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
            Require(lowered.Succeeded, kind + ": " + string.Join(" | ", lowered.Diagnostics.Select(d => d.Message)));
            var module = lowered.Module! with { Functions = lowered.Module!.Functions.Select(f => f with {
                Blocks = f.Blocks.Select(b => b with { Instructions = b.Instructions.SelectMany(i =>
                    i.Op is "managed_new" or "managed_event_read" ? new[] { i, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { i }).ToArray() }).ToArray() }).ToArray() };
            var wasm = WasmModuleCompiler.Compile(module);
            Require(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(d => d.Message)));
            File.WriteAllBytes(Path.Combine(directory, "csharp-event-generated-" + kind + ".wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-event-generated-" + kind + ".guest.json"), GuestIrSerializer.Serialize(module));
        }
        return 7;
    }

    public static int RunGeneratedLanguageFacade()
    {
        string directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR")
            ?? throw new InvalidOperationException("Set AVIDSCRIPT_MANAGED_HEAP_WASM_DIR to the native fixture directory.");
        string facade = File.ReadAllText(Path.Combine(directory, "event-facade.generated.cs"));
        const string source = """
            using AvidScript;
            using System.Runtime.InteropServices;
            public sealed class Counter
            {
                public void Handle(AActor actor, int amount, float scale)
                {
                    Script.Count += amount;
                    UE.Self.OnScriptSignal -= Handle;
                }
            }
            public static class Script
            {
                public static int Count;
                public static int Result;
                public static int SourceEvaluations;
                public static int HandlerEvaluations;
                public static int TargetSlot;
                public static int TargetGeneration;
                [AvidTransient] public static AvidSubscription Subscription;
                static void Handle(AActor actor, int amount, float scale) { Count += amount; }
                static AAvidScriptEditorDelegateEventTestActor GetSource()
                {
                    SourceEvaluations++;
                    return UE.Self;
                }
                static AvidEventHandlers.OnScriptSignal GetHandler()
                {
                    HandlerEvaluations++;
                    return Handle;
                }
                static void Other(AActor actor, int amount, float scale) { Count += 100; }
                static void Explicit(AActor actor, int amount, float scale) { Count += 1000; }
                static void AddDuringCallback(AActor actor, int amount, float scale)
                {
                    Count += amount;
                    UE.Self.OnScriptSignal += Other;
                    UE.Self.OnScriptSignal -= AddDuringCallback;
                }
                static void OnRefOut(ref int value, out int doubled)
                {
                    Count++;
                    Result += value;
                    value = Result;
                    doubled = Result * 2;
                }
                static int OnSinglecastFirst(ref int value, out int doubled)
                {
                    Count++;
                    value++;
                    doubled = value * 2;
                    return 111;
                }
                static int OnSinglecastLast(ref int value, out int doubled)
                {
                    Count++;
                    Result += value;
                    value = Result;
                    doubled = Result * 2;
                    return doubled + 1;
                }
                [UnmanagedCallersOnly(EntryPoint="avid_on_begin_play")]
                public static void Begin() { }
                [UnmanagedCallersOnly(EntryPoint="avid_on_tick")]
                public static void Tick(float delta)
                {
                    AvidEventHandlers.OnScriptSignal handler = Handle;
                    if (delta == 1.0f || delta == 2.0f) UE.Self.OnScriptSignal += handler;
                    if (delta == 3.0f || delta == 4.0f) UE.Self.OnScriptSignal -= handler;
                    if (delta == 5.0f)
                    {
                        int score = 40;
                        AvidEventHandlers.OnScriptSignal once = null;
                        once = (actor, amount, scale) =>
                        {
                            score += amount;
                            Result = score;
                            Count += amount;
                            UE.Self.OnScriptSignal -= once;
                        };
                        UE.Self.OnScriptSignal += once;
                    }
                    if (delta == 6.0f)
                    {
                        AvidEventHandlers.OnScriptSignal other = Other;
                        UE.Self.OnScriptSignal += handler + other + handler;
                        UE.Self.OnScriptSignal -= other + handler;
                    }
                    if (delta == 7.0f)
                    {
                        AvidEventHandlers.OnScriptSignal empty = null;
                        UE.Self.OnScriptSignal += empty;
                        UE.Self.OnScriptSignal -= empty;
                    }
                    if (delta == 8.0f) UE.Self.OnScriptSignal -= handler;
                    if (delta == 9.0f)
                    {
                        Counter counter = new Counter();
                        UE.Self.OnScriptSignal += counter.Handle;
                    }
                    if (delta == 10.0f) UE.Self.OnRefOutSignal += OnRefOut;
                    if (delta == 11.0f)
                    {
                        UE.Self.OnSinglecastSignal += OnSinglecastFirst;
                        UE.Self.OnSinglecastSignal += OnSinglecastLast;
                    }
                    if (delta == 12.0f) UE.Self.OnScriptSignal += AddDuringCallback;
                    if (delta == 13.0f) UE.Self.OnScriptSignal -= Other;
                    if (delta == 14.0f) Subscription = AvidSubscriptions.SubscribeOnScriptSignal(UE.Self, Explicit);
                    if (delta == 15.0f) UE.Self.OnScriptSignal += Handle;
                    if (delta == 16.0f) UE.Self.OnScriptSignal -= Handle;
                    if (delta == 17.0f) Subscription.Cancel();
                    if (delta == 18.0f) GetSource().OnScriptSignal += GetHandler();
                    if (delta == 19.0f)
                    {
                        AAvidScriptEditorDelegateEventTestActor invalid = default;
                        invalid.OnScriptSignal += Handle;
                    }
                    if (delta == 20.0f) GetSource().OnScriptSignal -= GetHandler();
                    if (delta == 21.0f)
                    {
                        Subscription = AvidSubscriptions.BindOnSinglecastSignal(UE.Self, OnSinglecastFirst);
                        Result = Subscription.IsValid ? 1 : 0;
                    }
                    if (delta == 22.0f)
                    {
                        var target = new AAvidScriptEditorDelegateEventTestActor(TargetSlot, TargetGeneration);
                        target.OnScriptSignal += Handle;
                    }
                }
            }
            """;
        SemanticDocument document = Analyze(source, facade);
        Require(document.Succeeded, string.Join(" | ", document.Diagnostics.Select(d => d.Message)));
        var lowered = CSharpGuestLowerer.Lower(document, new string('a', 64));
        Require(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(d => d.Message)));
        GuestModule module = lowered.Module!;
        Require(module.Functions.Count(function => function.Id.Contains("function:$event:language:", StringComparison.Ordinal)) >= 2
            && module.Imports.Any(import => import.Name == GuestEventState.LanguageSubscribeImport)
            && module.Imports.Any(import => import.Name == GuestEventState.LanguageLookupImport),
            "generated C# += and -= must lower through language event Host operations");
        var wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(d => d.Message)));
        File.WriteAllBytes(Path.Combine(directory, "csharp-event-language.wasm"), wasm.Bytes);
        File.WriteAllBytes(Path.Combine(directory, "csharp-event-language.guest.json"), GuestIrSerializer.Serialize(module));
        return 1;
    }

    private static SemanticDocument Analyze(string source, string facade)
    {
        var frontend = FrontendAnalyzer.Analyze(source, "Scripts/EventState.cs");
        return SemanticAnalyzer.Analyze(source, "Scripts/EventState.cs", frontend.Source.Sha256,
            new[] { new SemanticReferenceSource(facade, "generated://Events.cs", true) });
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
