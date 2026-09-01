namespace AvidScript.WasmBackend;

internal static class Program
{
    private static int Main(string[] args)
    {
        return WasmBackendCommandLine.Run(args);
    }
}
