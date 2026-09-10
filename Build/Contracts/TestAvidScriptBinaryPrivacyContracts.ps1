[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $pluginRoot 'Build/ReleaseEngineering/AvidScriptBinaryPrivacy.ps1')
Initialize-AvidScriptBinaryPrivacy
$testRoot = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('binary-privacy-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testRoot)
$script:PrivacyAssertions = 0

function Assert-PrivacyContract([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASBPT1000 $Message" }
    $script:PrivacyAssertions++
}

function New-PrivacyFixture([string]$Name) {
    $path = Join-Path $testRoot $Name
    [void][IO.Directory]::CreateDirectory($path)
    return $path
}

function Invoke-PrivacyFixture([string]$Name, [string]$Root, [string]$ExpectedCode, [string[]]$PrivatePath = @()) {
    $reportPath = Join-Path $testRoot "$Name.report.json"
    $code = ''
    try { Invoke-AvidScriptBinaryPrivacyScan -Root $Root -ReportPath $reportPath -PrivatePath $PrivatePath | Out-Null }
    catch {
        if ($_.Exception.Message -match 'ASBP100[12456]') { $code = $Matches[0] }
        else { throw }
    }
    Assert-PrivacyContract ($code -ceq $ExpectedCode) "unexpected result for $Name : $code"
    $raw = Get-Content -LiteralPath $reportPath -Raw
    Assert-PrivacyContract ($raw | Test-Json -SchemaFile $script:BinaryPrivacySchemaPath -ErrorAction SilentlyContinue) "invalid report for $Name"
    foreach ($secret in @($Root, [Environment]::GetFolderPath('UserProfile')) + $PrivatePath) {
        $escaped = ($secret | ConvertTo-Json -Compress).Trim('"')
        Assert-PrivacyContract (-not $raw.Contains($secret) -and -not $raw.Contains($escaped)) "report leaked private input for $Name"
    }
    return $raw | ConvertFrom-Json -Depth 30
}

$clean = New-PrivacyFixture 'clean'
[IO.File]::WriteAllText((Join-Path $clean 'public.txt'), '/avidscript/source/lib.rs', [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllBytes((Join-Path $clean 'all-extensions.blob'), [byte[]]@(0, 1, 2, 3, 4, 255))
$cleanHashes = @(Get-ChildItem -LiteralPath $clean -File | Sort-Object Name | Get-FileHash | ForEach-Object Hash)
$report = Invoke-PrivacyFixture 'clean' $clean ''
Assert-PrivacyContract ($report.passed -and $report.inventory_verified -and $report.file_count -eq 2) 'clean complete inventory must pass'
foreach ($entry in $report.files) {
    $file = Join-Path $clean $entry.path
    Assert-PrivacyContract ($entry.sha256 -ceq (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant()) 'report hash must equal actual bytes'
    Assert-PrivacyContract ($entry.length -eq (Get-Item -LiteralPath $file).Length) 'report length must equal actual bytes'
}
$afterHashes = @(Get-ChildItem -LiteralPath $clean -File | Sort-Object Name | Get-FileHash | ForEach-Object Hash)
Assert-PrivacyContract (($cleanHashes -join ',') -ceq ($afterHashes -join ',')) 'scan must not modify inputs'

$secretPath = 'C:\Build Secret\Å测试'
$probeRoot = New-PrivacyFixture 'encoding-probes'
$probePath = Join-Path $probeRoot 'probe.bin'
$encodings = @([Text.UTF8Encoding]::new($false), [Text.UnicodeEncoding]::new($false, $false), [Text.UnicodeEncoding]::new($true, $false))
$rule = [AvidScript.Release.BinaryPrivacyV1.Rule]::new('private-probe', $secretPath, $false, $secretPath.Length)
$rejected = $false
try { [void][AvidScript.Release.BinaryPrivacyV1.Rule]::new('short-window', $secretPath, $false, 1) }
catch { $rejected = $_.Exception.GetBaseException().Message.Contains('ASBP1001') }
Assert-PrivacyContract $rejected 'literal window must cover the complete token'
foreach ($encoding in $encodings) {
    foreach ($offset in @(0, 1, 61, 63, 127, 255)) {
        $token = $encoding.GetBytes('c:\bUILD sECRET\å测试\source.rs')
        $bytes = [byte[]]::new($offset + $token.Length + 32)
        [Array]::Copy($token, 0, $bytes, $offset, $token.Length)
        [IO.File]::WriteAllBytes($probePath, $bytes)
        $stream = [IO.FileStream]::new($probePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            $scan = [AvidScript.Release.BinaryPrivacyV1.Scanner]::ScanStream($stream, @($rule), 64)
            Assert-PrivacyContract ($scan.MatchedViews.ContainsKey('private-probe')) 'encoding/parity/chunk boundary must be detected'
            Assert-PrivacyContract ($scan.Sha256 -ceq (Get-FileHash -LiteralPath $probePath).Hash.ToLowerInvariant()) 'stream hash must cover each byte once'
            $writeRejected = $false
            try {
                $writer = [IO.File]::Open($probePath, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
                $writer.Dispose()
            }
            catch { $writeRejected = $true }
            Assert-PrivacyContract $writeRejected 'held read handle must deny writes'
        }
        finally { $stream.Dispose() }
    }
}

$cases = [ordered]@{
    'win-backslash' = 'C:\Users\UnrelatedPerson\crate.rs'
    'win-slash' = 'c:/uSeRs/UnrelatedPerson/crate.rs'
    'win-escaped' = 'C:\\Users\\UnrelatedPerson\\crate.rs'
    'unix-home' = '/home/unrelated-person/crate.rs'
    'mac-home' = '/Users/unrelated-person/crate.rs'
    'private-root' = 'c:/bUILD sECRET/å测试/crate.rs'
}
foreach ($name in $cases.Keys) {
    $root = New-PrivacyFixture $name
    [IO.File]::WriteAllText((Join-Path $root 'payload.dll'), $cases[$name], [Text.UTF8Encoding]::new($false))
    $result = Invoke-PrivacyFixture $name $root 'ASBP1006' @($secretPath)
    Assert-PrivacyContract ($result.inventory_verified -and -not $result.passed -and $result.files[0].content_hits.Count -gt 0) 'privacy hit must fail after complete hashing'
}

$boundary = New-PrivacyFixture 'integrated-boundary'
$token = [Text.UnicodeEncoding]::new($true, $false).GetBytes($secretPath + '\file.rs')
$bytes = [byte[]]::new(65535 + $token.Length + 17)
[Array]::Copy($token, 0, $bytes, 65535, $token.Length)
[IO.File]::WriteAllBytes((Join-Path $boundary 'odd-offset.lib'), $bytes)
$result = Invoke-PrivacyFixture 'integrated-boundary' $boundary 'ASBP1006' @($secretPath)
Assert-PrivacyContract ($result.files[0].content_hits.Count -gt 0) 'production chunk boundary must detect odd-offset UTF16BE'

$named = New-PrivacyFixture 'private-filename'
$privateFileName = [Environment]::UserName + '.bin'
[IO.File]::WriteAllText((Join-Path $named $privateFileName), 'public bytes')
$result = Invoke-PrivacyFixture 'private-filename' $named 'ASBP1006'
Assert-PrivacyContract ($null -eq $result.files[0].path -and $result.files[0].name_rule_ids -contains 'current-user') 'private filenames must be redacted'
Assert-PrivacyContract (-not (Get-Content -LiteralPath (Join-Path $testRoot 'private-filename.report.json') -Raw).Contains($privateFileName)) 'report must not repeat the private filename'

$empty = New-PrivacyFixture 'empty'
[void](Invoke-PrivacyFixture 'empty' $empty 'ASBP1002')
[void](Invoke-PrivacyFixture 'missing' (Join-Path $testRoot 'absent-directory') 'ASBP1002')
[void](Invoke-PrivacyFixture 'relative-deny-path' $clean 'ASBP1001' @('relative/path'))

$locked = New-PrivacyFixture 'locked'
$lockedPath = Join-Path $locked 'locked.bin'
[IO.File]::WriteAllBytes($lockedPath, [byte[]]@(1, 2, 3))
$writer = [IO.File]::Open($lockedPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
try { [void](Invoke-PrivacyFixture 'locked' $locked 'ASBP1004') }
finally { $writer.Dispose() }

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
public static class PrivacyMutationFixture {
    public static readonly ManualResetEventSlim Ready = new ManualResetEventSlim(false);
    public static Task<bool> Start(string file, string added) => Task.Run(() => {
        var timer = Stopwatch.StartNew();
        Ready.Set();
        while (timer.ElapsedMilliseconds < 5000) {
            try { using (var writer = new FileStream(file, FileMode.Open, FileAccess.Write, FileShare.ReadWrite)) {} }
            catch (IOException error) {
                if ((error.HResult & 0xffff) == 32) {
                    File.WriteAllText(added, "added after the scanner acquired its read lock");
                    return true;
                }
                throw;
            }
            Thread.Sleep(1);
        }
        return false;
    });
}
'@
$mutation = New-PrivacyFixture 'mutation'
$mutationFile = Join-Path $mutation 'payload.bin'
$fixtureStream = [IO.File]::Create($mutationFile)
try { $fixtureStream.SetLength(16MB) }
finally { $fixtureStream.Dispose() }
$interference = [PrivacyMutationFixture]::Start($mutationFile, (Join-Path $mutation 'added.bin'))
Assert-PrivacyContract ([PrivacyMutationFixture]::Ready.Wait(5000)) 'mutation worker must start before scan'
[void](Invoke-PrivacyFixture 'mutation' $mutation 'ASBP1004')
Assert-PrivacyContract ($interference.GetAwaiter().GetResult()) 'directory must actually mutate while an input file is locked'

$ads = New-PrivacyFixture 'ads'
$adsPath = Join-Path $ads 'payload.bin'
[IO.File]::WriteAllText($adsPath, 'public')
Set-Content -LiteralPath $adsPath -Stream 'PrivateStream' -Value 'private'
[void](Invoke-PrivacyFixture 'ads' $ads 'ASBP1002')
$directoryAds = New-PrivacyFixture 'directory-ads'
[IO.File]::WriteAllText((Join-Path $directoryAds 'payload.bin'), 'public')
Set-Content -LiteralPath $directoryAds -Stream 'PrivateStream' -Value 'private'
[void](Invoke-PrivacyFixture 'directory-ads' $directoryAds 'ASBP1002')

$links = New-PrivacyFixture 'links'
[void](New-Item -ItemType Junction -Path (Join-Path $links 'link') -Target $clean)
[void](Invoke-PrivacyFixture 'links' $links 'ASBP1002')
[void](Invoke-PrivacyFixture 'linked-ancestor' (Join-Path $links 'link') 'ASBP1002')

$insidePath = Join-Path $clean 'report.json'
$rejected = $false
try { Invoke-AvidScriptBinaryPrivacyScan -Root $clean -ReportPath $insidePath | Out-Null }
catch { $rejected = $_.Exception.Message.Contains('ASBP1001') }
Assert-PrivacyContract ($rejected -and -not (Test-Path -LiteralPath $insidePath)) 'report inside input must be rejected without writes'
$existingReport = Join-Path $testRoot 'clean.report.json'
$beforeReportHash = (Get-FileHash -LiteralPath $existingReport).Hash
$rejected = $false
try { Invoke-AvidScriptBinaryPrivacyScan -Root $clean -ReportPath $existingReport | Out-Null }
catch { $rejected = $_.Exception.Message.Contains('ASBP1001') }
Assert-PrivacyContract ($rejected -and $beforeReportHash -ceq (Get-FileHash -LiteralPath $existingReport).Hash) 'existing report must not be overwritten'

$stream = [IO.FileStream]::new($probePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $rejected = $false
    try { [AvidScript.Release.BinaryPrivacyV1.Scanner]::AssertHandlePath($stream, (Join-Path $probeRoot 'wrong.bin')) }
    catch { $rejected = $_.Exception.GetBaseException().Message.Contains('ASBP1004') }
    Assert-PrivacyContract $rejected 'actual open handle must match the enumerated path'
}
finally { $stream.Dispose() }

function Invoke-PrivacyCli([string]$Root, [string]$ReportPath) {
    $start = [Diagnostics.ProcessStartInfo]::new((Get-Command pwsh).Source)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('-NoProfile', '-File', (Join-Path $pluginRoot 'Build/InvokeAvidScriptBinaryPrivacyScan.ps1'), '-Root', $Root, '-ReportPath', $ReportPath)) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'ASBPT1000 CLI did not start' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        [void]$stdout.GetAwaiter().GetResult()
        [void]$stderr.GetAwaiter().GetResult()
        return $process.ExitCode
    }
    finally { $process.Dispose() }
}
Assert-PrivacyContract ((Invoke-PrivacyCli $clean (Join-Path $testRoot 'cli-clean.json')) -eq 0) 'clean CLI must exit zero'
Assert-PrivacyContract ((Invoke-PrivacyCli (Join-Path $testRoot 'win-backslash') (Join-Path $testRoot 'cli-private.json')) -ne 0) 'private CLI must exit nonzero'

[ordered]@{
    result = 'binary_privacy_contracts_passed'
    assertions_passed = $script:PrivacyAssertions
    assertions_total = $script:PrivacyAssertions
    evidence_root = $testRoot
} | ConvertTo-Json
