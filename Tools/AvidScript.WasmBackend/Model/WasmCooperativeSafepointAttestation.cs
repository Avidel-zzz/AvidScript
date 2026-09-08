namespace AvidScript.WasmBackend;

public sealed record WasmCooperativeSafepointAttestation(
    int SchemaVersion,
    uint PollInterval,
    int LoopPollBlockCount,
    int RecursiveFunctionCount,
    int SiteCount,
    string SiteSha256,
    bool Verified);
