Set-StrictMode -Version Latest

function Get-AvidScriptAndroidApkIdentity {
    param([Parameter(Mandatory = $true)][string]$Path)

    $FullPath = (Resolve-Path -LiteralPath $Path).Path
    $Archive = [System.IO.Compression.ZipFile]::OpenRead($FullPath)
    try {
        $Libraries = @($Archive.Entries | Where-Object { $_.FullName -cmatch '^lib/[^/]+/[^/]+\.so$' })
        $Unreal = @($Libraries | Where-Object { $_.FullName -ceq 'lib/arm64-v8a/libUnreal.so' -and $_.Length -gt 0 })
        if ($Unreal.Count -ne 1 -or @($Libraries | Where-Object { -not $_.FullName.StartsWith('lib/arm64-v8a/', [StringComparison]::Ordinal) }).Count -gt 0) {
            throw 'APK must contain one non-empty arm64-v8a/libUnreal.so and no other native ABI.'
        }
        foreach ($Library in $Libraries) {
            $Stream = $Library.Open()
            try {
                $Header = [byte[]]::new(20)
                $Stream.ReadExactly($Header, 0, $Header.Length)
                if ([BitConverter]::ToString($Header[0..5]) -cne '7F-45-4C-46-02-01' -or
                    [BitConverter]::ToUInt16($Header, 18) -ne 183) {
                    throw "APK native library is not ELF64/AArch64: $($Library.FullName)"
                }
            }
            finally { $Stream.Dispose() }
        }
        return [pscustomobject]@{
            path = $FullPath
            sha256 = (Get-FileHash -LiteralPath $FullPath -Algorithm SHA256).Hash.ToLowerInvariant()
            architecture = 'arm64-v8a'
            native_library_count = $Libraries.Count
        }
    }
    finally { $Archive.Dispose() }
}

function Invoke-AvidScriptAndroidProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [ValidateRange(1, 41630)][int]$TimeoutSeconds = 60,
        [hashtable]$Environment = @{}
    )

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $Executable
    $StartInfo.WorkingDirectory = $WorkingDirectory
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in $Arguments) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }
    foreach ($Entry in $Environment.GetEnumerator()) {
        $StartInfo.Environment[$Entry.Key] = [string]$Entry.Value
    }
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    $Clock = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        if (-not $Process.Start()) {
            throw "Android tool could not start: $Executable"
        }
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
            $Process.Kill($true)
            $Process.WaitForExit()
            throw "Android tool exceeded $TimeoutSeconds seconds: $Executable"
        }
        return [pscustomobject]@{
            exit_code = $Process.ExitCode
            stdout = $StdoutTask.GetAwaiter().GetResult()
            stderr = $StderrTask.GetAwaiter().GetResult()
            elapsed_ms = $Clock.Elapsed.TotalMilliseconds
        }
    }
    finally {
        $Process.Dispose()
    }
}
