[CmdletBinding()]
param(
    [ValidateSet('Gate', 'Fixtures', 'Hashes')]
    [string]$Mode = 'Gate',
    [string]$PluginRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function New-ViolationList {
    return ,([System.Collections.Generic.List[string]]::new())
}

function Add-Violation {
    param(
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Message
    )

    $Violations.Add($Message)
}

function ConvertFrom-CppPhase2LineSplicing {
    param([string]$Source)

    return [regex]::Replace($Source, '\\(?:\r\n|\n|\r)', '')
}

function Remove-SourceComments {
    param(
        [string]$Source,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Builder = [System.Text.StringBuilder]::new($Source.Length)
    $State = 'code'
    for ($Index = 0; $Index -lt $Source.Length; ++$Index) {
        $Character = $Source[$Index]
        $NextCharacter = if ($Index + 1 -lt $Source.Length) {
            $Source[$Index + 1]
        }
        else {
            [char]0
        }

        switch ($State) {
            'code' {
                if ($Character -eq '/' -and $NextCharacter -eq '/') {
                    [void]$Builder.Append('  ')
                    ++$Index
                    $State = 'line-comment'
                }
                elseif ($Character -eq '/' -and $NextCharacter -eq '*') {
                    [void]$Builder.Append('  ')
                    ++$Index
                    $State = 'block-comment'
                }
                elseif ($Character -eq '"') {
                    [void]$Builder.Append($Character)
                    $State = 'string'
                }
                elseif ($Character -eq "'") {
                    [void]$Builder.Append($Character)
                    $State = 'character'
                }
                else {
                    [void]$Builder.Append($Character)
                }
            }
            'line-comment' {
                if ($Character -eq "`r" -or $Character -eq "`n") {
                    [void]$Builder.Append($Character)
                    $State = 'code'
                }
                else {
                    [void]$Builder.Append(' ')
                }
            }
            'block-comment' {
                if ($Character -eq '*' -and $NextCharacter -eq '/') {
                    [void]$Builder.Append('  ')
                    ++$Index
                    $State = 'code'
                }
                elseif ($Character -eq "`r" -or $Character -eq "`n") {
                    [void]$Builder.Append($Character)
                }
                else {
                    [void]$Builder.Append(' ')
                }
            }
            'string' {
                [void]$Builder.Append($Character)
                if ($Character -eq '\' -and $Index + 1 -lt $Source.Length) {
                    ++$Index
                    [void]$Builder.Append($Source[$Index])
                }
                elseif ($Character -eq '"') {
                    $State = 'code'
                }
            }
            'character' {
                [void]$Builder.Append($Character)
                if ($Character -eq '\' -and $Index + 1 -lt $Source.Length) {
                    ++$Index
                    [void]$Builder.Append($Source[$Index])
                }
                elseif ($Character -eq "'") {
                    $State = 'code'
                }
            }
        }
    }

    if ($State -eq 'block-comment' -or $State -eq 'string' -or $State -eq 'character') {
        Add-Violation $Violations "$Description contains an unterminated $State"
    }
    return $Builder.ToString()
}

function Remove-SourceStrings {
    param([string]$Source)

    $Builder = [System.Text.StringBuilder]::new($Source.Length)
    $Quote = [char]0
    for ($Index = 0; $Index -lt $Source.Length; ++$Index) {
        $Character = $Source[$Index]
        if ($Quote -eq [char]0) {
            if ($Character -eq '"' -or $Character -eq "'") {
                $Quote = $Character
                [void]$Builder.Append(' ')
            }
            else {
                [void]$Builder.Append($Character)
            }
            continue
        }

        if ($Character -eq '\' -and $Index + 1 -lt $Source.Length) {
            [void]$Builder.Append(' ')
            ++$Index
            [void]$Builder.Append(' ')
            continue
        }
        if ($Character -eq $Quote) {
            $Quote = [char]0
        }
        if ($Character -eq "`r" -or $Character -eq "`n") {
            [void]$Builder.Append($Character)
        }
        else {
            [void]$Builder.Append(' ')
        }
    }
    return $Builder.ToString()
}

function Get-BalancedSpan {
    param(
        [string]$Source,
        [int]$OpenIndex,
        [char]$OpenCharacter,
        [char]$CloseCharacter,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    if ($OpenIndex -lt 0 -or $OpenIndex -ge $Source.Length -or $Source[$OpenIndex] -ne $OpenCharacter) {
        Add-Violation $Violations "$Description has no '$OpenCharacter' delimiter"
        return $null
    }

    $Depth = 0
    $Quote = [char]0
    for ($Index = $OpenIndex; $Index -lt $Source.Length; ++$Index) {
        $Character = $Source[$Index]
        if ($Quote -ne [char]0) {
            if ($Character -eq '\' -and $Index + 1 -lt $Source.Length) {
                ++$Index
                continue
            }
            if ($Character -eq $Quote) {
                $Quote = [char]0
            }
            continue
        }

        if ($Character -eq '"' -or $Character -eq "'") {
            $Quote = $Character
            continue
        }
        if ($Character -eq $OpenCharacter) {
            ++$Depth
            continue
        }
        if ($Character -eq $CloseCharacter) {
            --$Depth
            if ($Depth -eq 0) {
                return [pscustomobject]@{
                    StartIndex = $OpenIndex
                    EndIndex = $Index
                    Text = $Source.Substring($OpenIndex, $Index - $OpenIndex + 1)
                    Inner = $Source.Substring($OpenIndex + 1, $Index - $OpenIndex - 1)
                }
            }
        }
    }

    Add-Violation $Violations "$Description has an unbalanced '$OpenCharacter$CloseCharacter' region"
    return $null
}

function Get-UniqueBraceRegion {
    param(
        [string]$Source,
        [string]$AnchorPattern,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Matches = [regex]::Matches(
        $Source,
        $AnchorPattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($Matches.Count -ne 1) {
        Add-Violation $Violations "$Description must have exactly one candidate; found $($Matches.Count)"
        return $null
    }

    $OpenIndex = $Source.IndexOf(
        '{',
        $Matches[0].Index + $Matches[0].Length,
        [System.StringComparison]::Ordinal)
    if ($OpenIndex -lt 0) {
        Add-Violation $Violations "$Description is missing its body"
        return $null
    }

    $Span = Get-BalancedSpan $Source $OpenIndex '{' '}' $Violations $Description
    if ($null -eq $Span) {
        return $null
    }
    return [pscustomobject]@{
        Text = $Source.Substring($Matches[0].Index, $Span.EndIndex - $Matches[0].Index + 1)
        Body = $Span.Inner
        StartIndex = $Matches[0].Index
        EndIndex = $Span.EndIndex
    }
}

function Split-TopLevel {
    param(
        [string]$Source,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Parts = [System.Collections.Generic.List[string]]::new()
    $StartIndex = 0
    $ParenDepth = 0
    $BraceDepth = 0
    $BracketDepth = 0
    $Quote = [char]0
    for ($Index = 0; $Index -lt $Source.Length; ++$Index) {
        $Character = $Source[$Index]
        if ($Quote -ne [char]0) {
            if ($Character -eq '\' -and $Index + 1 -lt $Source.Length) {
                ++$Index
                continue
            }
            if ($Character -eq $Quote) {
                $Quote = [char]0
            }
            continue
        }
        if ($Character -eq '"' -or $Character -eq "'") {
            $Quote = $Character
            continue
        }
        switch ($Character) {
            '(' { ++$ParenDepth }
            ')' { --$ParenDepth }
            '{' { ++$BraceDepth }
            '}' { --$BraceDepth }
            '[' { ++$BracketDepth }
            ']' { --$BracketDepth }
            ',' {
                if ($ParenDepth -eq 0 -and $BraceDepth -eq 0 -and $BracketDepth -eq 0) {
                    $Part = $Source.Substring($StartIndex, $Index - $StartIndex).Trim()
                    if ($Part.Length -gt 0) {
                        $Parts.Add($Part)
                    }
                    $StartIndex = $Index + 1
                }
            }
        }
        if ($ParenDepth -lt 0 -or $BraceDepth -lt 0 -or $BracketDepth -lt 0) {
            Add-Violation $Violations "$Description contains an unexpected closing delimiter"
            return @()
        }
    }

    if ($ParenDepth -ne 0 -or $BraceDepth -ne 0 -or $BracketDepth -ne 0 -or $Quote -ne [char]0) {
        Add-Violation $Violations "$Description contains an incomplete candidate"
        return @()
    }
    $Tail = $Source.Substring($StartIndex).Trim()
    if ($Tail.Length -gt 0) {
        $Parts.Add($Tail)
    }
    return @($Parts)
}

function ConvertFrom-CppStringArgument {
    param([string]$Expression)

    $Match = [regex]::Match(
        $Expression.Trim(),
        '^(?:TEXT\s*\(\s*"(?<text>(?:\\.|[^"\\])*)"\s*\)|"(?<plain>(?:\\.|[^"\\])*)")$')
    if (-not $Match.Success) {
        return $null
    }
    if ($Match.Groups['text'].Success) {
        return $Match.Groups['text'].Value
    }
    return $Match.Groups['plain'].Value
}

function ConvertFrom-CppEscapedString {
    param([string]$Value)

    $Builder = [System.Text.StringBuilder]::new($Value.Length)
    for ($Index = 0; $Index -lt $Value.Length; ++$Index) {
        $Character = $Value[$Index]
        if ($Character -ne '\' -or $Index + 1 -ge $Value.Length) {
            [void]$Builder.Append($Character)
            continue
        }
        ++$Index
        switch ($Value[$Index]) {
            'n' { [void]$Builder.Append("`n") }
            'r' { [void]$Builder.Append("`r") }
            't' { [void]$Builder.Append("`t") }
            '"' { [void]$Builder.Append('"') }
            '\' { [void]$Builder.Append('\') }
            default {
                [void]$Builder.Append('\')
                [void]$Builder.Append($Value[$Index])
            }
        }
    }
    return $Builder.ToString()
}

function Get-CppStringLiterals {
    param(
        [string]$Source,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Literals = [System.Collections.Generic.List[string]]::new()
    for ($Index = 0; $Index -lt $Source.Length; ++$Index) {
        if ($Source[$Index] -ne '"') {
            continue
        }
        $Builder = [System.Text.StringBuilder]::new()
        $Closed = $false
        for (++$Index; $Index -lt $Source.Length; ++$Index) {
            $Character = $Source[$Index]
            if ($Character -eq '\' -and $Index + 1 -lt $Source.Length) {
                [void]$Builder.Append($Character)
                ++$Index
                [void]$Builder.Append($Source[$Index])
                continue
            }
            if ($Character -eq '"') {
                $Closed = $true
                break
            }
            [void]$Builder.Append($Character)
        }
        if (-not $Closed) {
            Add-Violation $Violations "$Description contains an unterminated string candidate"
            break
        }
        $Literals.Add((ConvertFrom-CppEscapedString $Builder.ToString()))
    }
    return @($Literals)
}

function Test-RegexSequence {
    param(
        [string]$Source,
        [string[]]$Patterns,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Cursor = 0
    foreach ($Pattern in $Patterns) {
        $Regex = [regex]::new(
            $Pattern,
            [System.Text.RegularExpressions.RegexOptions]::Multiline)
        $Match = $Regex.Match($Source, $Cursor)
        if (-not $Match.Success) {
            Add-Violation $Violations "$Description is missing ordered pattern: $Pattern"
            return
        }
        $Cursor = $Match.Index + $Match.Length
    }
}

function Test-ExactMultiset {
    param(
        [string[]]$Actual,
        [string[]]$Expected,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $ActualCounts = @{}
    foreach ($Value in $Actual) {
        $Key = [string]$Value
        if (-not $ActualCounts.ContainsKey($Key)) {
            $ActualCounts[$Key] = 0
        }
        ++$ActualCounts[$Key]
    }
    $ExpectedCounts = @{}
    foreach ($Value in $Expected) {
        $Key = [string]$Value
        if (-not $ExpectedCounts.ContainsKey($Key)) {
            $ExpectedCounts[$Key] = 0
        }
        ++$ExpectedCounts[$Key]
    }

    $AllKeys = @($ActualCounts.Keys) + @($ExpectedCounts.Keys) | Sort-Object -Unique
    $Differences = [System.Collections.Generic.List[string]]::new()
    foreach ($Key in $AllKeys) {
        $ActualCount = if ($ActualCounts.ContainsKey($Key)) { $ActualCounts[$Key] } else { 0 }
        $ExpectedCount = if ($ExpectedCounts.ContainsKey($Key)) { $ExpectedCounts[$Key] } else { 0 }
        if ($ActualCount -ne $ExpectedCount) {
            $Differences.Add("$Key actual=$ActualCount expected=$ExpectedCount")
        }
    }
    if ($Differences.Count -gt 0) {
        Add-Violation $Violations "$Description differs from the strict allowlist | $($Differences -join '; ')"
    }
}

function Get-CallArguments {
    param(
        [string]$Source,
        [string]$FunctionName,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Pattern = '(?<![A-Za-z0-9_])' + [regex]::Escape($FunctionName) + '\s*\('
    $Matches = [regex]::Matches($Source, $Pattern)
    $Calls = [System.Collections.Generic.List[object]]::new()
    foreach ($Match in $Matches) {
        $OpenIndex = $Source.IndexOf('(', $Match.Index, [System.StringComparison]::Ordinal)
        $Span = Get-BalancedSpan $Source $OpenIndex '(' ')' $Violations $Description
        if ($null -eq $Span) {
            continue
        }
        $Calls.Add([pscustomobject]@{
            Text = $Source.Substring($Match.Index, $Span.EndIndex - $Match.Index + 1)
            Arguments = @(Split-TopLevel $Span.Inner $Violations "$Description arguments")
        })
    }
    return @($Calls)
}

function Get-DirectCallNames {
    param([string]$Body)

    $Names = [System.Collections.Generic.List[string]]::new()
    $Pattern = '(?<![A-Za-z0-9_])(?<callee>[A-Za-z_][A-Za-z0-9_]*(?:\s*(?:::|->|\.)\s*[A-Za-z_][A-Za-z0-9_]*)*)\s*(?:<[^;{}()]*>)?\s*\('
    $Code = Remove-SourceStrings $Body
    foreach ($Match in [regex]::Matches($Code, $Pattern)) {
        $Name = [regex]::Replace($Match.Groups['callee'].Value, '\s+', '')
        if ($Name -in @(
            'if', 'for', 'while', 'switch', 'return', 'sizeof',
            'static_cast', 'reinterpret_cast', 'const_cast', 'dynamic_cast',
            'TEXT', 'UE_ARRAY_COUNT'
        )) {
            continue
        }
        $Names.Add($Name)
    }
    return @($Names)
}

function Test-DirectCallAllowlist {
    param(
        [string]$Body,
        [string[]]$AllowedCalls,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Actual = @(Get-DirectCallNames $Body | Sort-Object -Unique)
    $Unexpected = @($Actual | Where-Object { $AllowedCalls -cnotcontains $_ })
    if ($Unexpected.Count -gt 0) {
        Add-Violation $Violations "$Description contains unreviewed helper calls: $($Unexpected -join ', ')"
    }
}

function Test-SingleCallInsideGuard {
    param(
        [string]$FunctionBody,
        [string]$GuardAnchorPattern,
        [string]$CallName,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Guard = Get-UniqueBraceRegion $FunctionBody $GuardAnchorPattern $Violations "$Description guard"
    if ($null -eq $Guard) {
        return
    }
    $AllCalls = @(Get-CallArguments $FunctionBody $CallName $Violations "$Description calls")
    $GuardCalls = @(Get-CallArguments $Guard.Body $CallName $Violations "$Description guarded calls")
    if ($AllCalls.Count -ne 1 -or $GuardCalls.Count -ne 1) {
        Add-Violation $Violations "$Description must call $CallName exactly once inside its guard; total=$($AllCalls.Count) guarded=$($GuardCalls.Count)"
    }
}

function Get-CanonicalCppTokenStream {
    param([string]$Source)

    $Tokens = [System.Collections.Generic.List[string]]::new()
    $MultiCharacterOperators = @(
        '<=>', '>>=', '<<=', '->*', '...',
        '##', '::', '->', '++', '--', '<<', '>>', '<=', '>=', '==', '!=',
        '&&', '||', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '.*'
    )
    for ($Index = 0; $Index -lt $Source.Length;) {
        $Character = $Source[$Index]
        if ([char]::IsWhiteSpace($Character)) {
            ++$Index
            continue
        }

        if ([char]::IsLetter($Character) -or $Character -eq '_') {
            $Start = $Index
            ++$Index
            while ($Index -lt $Source.Length -and
                ([char]::IsLetterOrDigit($Source[$Index]) -or $Source[$Index] -eq '_')) {
                ++$Index
            }
            $Tokens.Add($Source.Substring($Start, $Index - $Start))
            continue
        }

        if ([char]::IsDigit($Character) -or
            ($Character -eq '.' -and $Index + 1 -lt $Source.Length -and
                [char]::IsDigit($Source[$Index + 1]))) {
            $Start = $Index
            ++$Index
            while ($Index -lt $Source.Length) {
                $NumberCharacter = $Source[$Index]
                if ([char]::IsLetterOrDigit($NumberCharacter) -or
                    $NumberCharacter -eq '_' -or
                    $NumberCharacter -eq '.' -or
                    $NumberCharacter -eq "'") {
                    ++$Index
                    continue
                }
                if (($NumberCharacter -eq '+' -or $NumberCharacter -eq '-') -and
                    $Index -gt $Start -and
                    $Source[$Index - 1] -in @('e', 'E', 'p', 'P')) {
                    ++$Index
                    continue
                }
                break
            }
            $Tokens.Add($Source.Substring($Start, $Index - $Start))
            continue
        }

        if ($Character -eq '"' -or $Character -eq "'") {
            $Start = $Index
            $Quote = $Character
            ++$Index
            $Closed = $false
            while ($Index -lt $Source.Length) {
                if ($Source[$Index] -eq '\') {
                    $Index += [Math]::Min(2, $Source.Length - $Index)
                    continue
                }
                if ($Source[$Index] -eq $Quote) {
                    ++$Index
                    $Closed = $true
                    break
                }
                ++$Index
            }
            if (-not $Closed) {
                throw 'unterminated C++ string or character literal in canonical hash input'
            }
            $Tokens.Add($Source.Substring($Start, $Index - $Start))
            continue
        }

        $Operator = $null
        foreach ($Candidate in $MultiCharacterOperators) {
            if ($Index + $Candidate.Length -le $Source.Length -and
                $Source.Substring($Index, $Candidate.Length) -ceq $Candidate) {
                $Operator = $Candidate
                break
            }
        }
        if ($null -ne $Operator) {
            $Tokens.Add($Operator)
            $Index += $Operator.Length
            continue
        }

        $Tokens.Add([string]$Character)
        ++$Index
    }

    $Builder = [System.Text.StringBuilder]::new()
    foreach ($Token in $Tokens) {
        [void]$Builder.Append($Token.Length)
        [void]$Builder.Append(':')
        [void]$Builder.Append($Token)
        [void]$Builder.Append(';')
    }
    return $Builder.ToString()
}

function Get-CanonicalCodeSha256 {
    param([string]$Source)

    $CanonicalTokens = Get-CanonicalCppTokenStream $Source
    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($CanonicalTokens)
    $Sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString($Sha256.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $Sha256.Dispose()
    }
}

function Get-RendererFrozenRegions {
    return [ordered]@{
        'RenderMethod' = '\bbool\s+RenderMethod\s*\('
        'RenderPropertyGetter' = '\bbool\s+RenderPropertyGetter\s*\('
        'AppendVector' = '\bvoid\s+AppendVector\s*\('
        'AppendInputEvent' = '\bvoid\s+AppendInputEvent\s*\('
        'AppendRotator' = '\bvoid\s+AppendRotator\s*\('
        'AppendTransform' = '\bvoid\s+AppendTransform\s*\('
        'AppendObjectHandleProxy' = '\bvoid\s+AppendObjectHandleProxy\s*\('
        'EmitReferenceSource' = '\bbool\s+FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource\s*\('
    }
}

function Test-RendererFrozenRegions {
    param(
        [string]$Source,
        [System.Collections.Generic.List[string]]$Violations
    )

    $ExpectedHashes = [ordered]@{
        'RenderMethod' = '8de2161bc979d478083b62f8ce12c8eddcb7199cc5e48a191c83c3afb0f38723'
        'RenderPropertyGetter' = '4f607fe468130480bf2dfdd541d785134176fce3baa5e2dd31b07f7a659db215'
        'AppendVector' = 'a052bea5eda0769613a907e479549d6c9032b6ae9a9b90a110714105d07a4e3d'
        'AppendInputEvent' = '27014e57fb190bcd48d5028c4b1722e5f6ec11837962c6bae8cb52658a580f39'
        'AppendRotator' = '7a71fdf13edf712a20b7a6dcf5fd91ae284b36ed372feeef04011a2763deeacd'
        'AppendTransform' = 'd277ff9ceb802b8f5e24ee61a88c1fc8d2fc366a9231de145c8d224be7d0cd41'
        'AppendObjectHandleProxy' = '560156fdf7ad9344a2773cbcf9c733e81a36577c73abeaa6d136a7349c6c999d'
        'EmitReferenceSource' = 'c39e6ab40ed3ef4f192a9799922e3a85b59aa406280e4485bc7cefff4a0261f4'
    }
    foreach ($Entry in (Get-RendererFrozenRegions).GetEnumerator()) {
        $Region = Get-UniqueBraceRegion $Source $Entry.Value $Violations "frozen renderer region $($Entry.Key)"
        if ($null -eq $Region) {
            continue
        }
        $ActualHash = Get-CanonicalCodeSha256 $Region.Text
        if ($ActualHash -cne $ExpectedHashes[$Entry.Key]) {
            Add-Violation $Violations "frozen renderer region $($Entry.Key) changed: actual=$ActualHash expected=$($ExpectedHashes[$Entry.Key])"
        }
    }
}

function Test-GeneratedSurfaceConstructionClosure {
    param(
        [string]$RendererSource,
        [string]$StateContractRendererSource,
        [string]$DefaultValueFormatterSource,
        [string]$SyntaxSource,
        [string]$LifecycleBindingSource,
        [System.Collections.Generic.List[string]]$Violations
    )

    $ExpectedHashes = [ordered]@{
        'BindingRenderer' = '63891e9ee22959aee6351e1931c63826146918f8495c3573f2e616e7ab45b10f'
        'StateContractRenderer' = '8d24e315f424a1827b2cdf6358019785c9d7ccdf5322b10f6a8971cee29ce9b9'
        'DefaultValueFormatter' = '6cffc9ae4e299b1b3134380b5827ccb068d6bcc01b4ff5b1bb65e30627e0bbf7'
        'CSharpSyntax' = 'bf685b36a2cd07cfffb69e46aa1937322b92e3ec9350afac4f7225f1c037249f'
        'LifecycleBinding' = '4a7c325d2f16e119ea3ce08af777186fda194e61bd3e397e4b29bb6e2c50510e'
    }
    $ActualSources = [ordered]@{
        'BindingRenderer' = $RendererSource
        'StateContractRenderer' = $StateContractRendererSource
        'DefaultValueFormatter' = $DefaultValueFormatterSource
        'CSharpSyntax' = $SyntaxSource
        'LifecycleBinding' = $LifecycleBindingSource
    }
    foreach ($Entry in $ActualSources.GetEnumerator()) {
        $ActualHash = Get-CanonicalCodeSha256 $Entry.Value
        if ($ActualHash -cne $ExpectedHashes[$Entry.Key]) {
            Add-Violation $Violations "frozen generated surface source $($Entry.Key) changed: actual=$ActualHash expected=$($ExpectedHashes[$Entry.Key])"
        }
    }
}

function Test-RegistryPathSurface {
    param(
        [string]$RegistrySource,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Phase2Source = ConvertFrom-CppPhase2LineSplicing $RegistrySource
    $CodeWithoutStrings = Remove-SourceStrings $Phase2Source
    $PathIdentifierCount = [regex]::Matches($CodeWithoutStrings, '\bGetPathName\b').Count
    if ($PathIdentifierCount -ne 2) {
        Add-Violation $Violations "$Description must contain exactly two reviewed GetPathName identifiers; actual=$PathIdentifierCount"
    }
    $NonIncludeDirectives = @(
        [regex]::Matches($CodeWithoutStrings, '(?m)^\s*#\s*(?!include\b)[A-Za-z_]+'))
    if ($NonIncludeDirectives.Count -gt 0) {
        Add-Violation $Violations "$Description contains non-include preprocessor directives"
    }
    if ($CodeWithoutStrings.Contains('##')) {
        Add-Violation $Violations "$Description contains token-paste and can synthesize unreviewed path calls"
    }
    if ($CodeWithoutStrings.Contains('%:')) {
        Add-Violation $Violations "$Description contains alternative preprocessor tokens"
    }
    if ($CodeWithoutStrings.Contains('??/')) {
        Add-Violation $Violations "$Description contains a trigraph that can synthesize line splicing"
    }
}

function Test-DiagnosticForwarding {
    param(
        [string]$FunctionBody,
        [string]$FunctionName,
        [System.Collections.Generic.List[string]]$Violations
    )

    $Phase2Body = ConvertFrom-CppPhase2LineSplicing $FunctionBody
    $FailureCalls = @(Get-CallArguments $Phase2Body 'SetFailure' $Violations "$FunctionName failure calls")
    foreach ($FailureCall in $FailureCalls) {
        if ($FailureCall.Arguments.Count -ne 6 -or
            $FailureCall.Arguments[4].Trim() -cne 'bIncludeObjectPath') {
            Add-Violation $Violations "$FunctionName failure diagnostics must forward bIncludeObjectPath"
        }
    }
    $SuccessCalls = @(Get-CallArguments $Phase2Body 'SetSuccess' $Violations "$FunctionName success calls")
    foreach ($SuccessCall in $SuccessCalls) {
        if ($SuccessCall.Arguments.Count -ne 4 -or
            $SuccessCall.Arguments[3].Trim() -cne 'bIncludeObjectPath') {
            Add-Violation $Violations "$FunctionName success diagnostics must forward bIncludeObjectPath"
        }
    }
    if ($FailureCalls.Count -eq 0 -or $SuccessCalls.Count -eq 0) {
        Add-Violation $Violations "$FunctionName diagnostic leaves are incomplete"
    }
}

function Test-GeneratedLiteralStream {
    param(
        [string]$Source,
        [string[]]$AllowedAvidTokens,
        [string[]]$AllowedDeclarationNames,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Literals = @(Get-CppStringLiterals $Source $Violations $Description)
    $LiteralStream = $Literals -join ''
    $UnexpectedAvidTokens = @(
        [regex]::Matches($LiteralStream, '\bavid_[a-z0-9_]+\b') |
            ForEach-Object { $_.Value } |
            Where-Object { $AllowedAvidTokens -cnotcontains $_ } |
            Sort-Object -Unique)
    if ($UnexpectedAvidTokens.Count -gt 0) {
        Add-Violation $Violations "$Description contains bespoke split-capable avid_* literals: $($UnexpectedAvidTokens -join ', ')"
    }

    $UnexpectedDeclarations = @(
        [regex]::Matches(
            $LiteralStream,
            '\b(?:public|internal)\s+(?:(?:readonly|static)\s+)?(?:struct|class|enum)\s+(?<name>[A-Za-z_][A-Za-z0-9_]*)') |
            ForEach-Object { $_.Groups['name'].Value } |
            Where-Object { $AllowedDeclarationNames -cnotcontains $_ } |
            Sort-Object -Unique)
    if ($UnexpectedDeclarations.Count -gt 0) {
        Add-Violation $Violations "$Description contains bespoke split-capable wrapper declarations: $($UnexpectedDeclarations -join ', ')"
    }
}

function ConvertTo-NativeSymbolRecord {
    param(
        [string]$Candidate,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Trimmed = $Candidate.Trim()
    if (-not ($Trimmed.StartsWith('{') -and $Trimmed.EndsWith('}'))) {
        Add-Violation $Violations "$Description has an unparsed initializer candidate: $Trimmed"
        return $null
    }
    $Fields = @(Split-TopLevel $Trimmed.Substring(1, $Trimmed.Length - 2) $Violations $Description)
    if ($Fields.Count -ne 4) {
        Add-Violation $Violations "$Description initializer must have four fields: $Trimmed"
        return $null
    }
    $Name = ConvertFrom-CppStringArgument $Fields[0]
    $Signature = ConvertFrom-CppStringArgument $Fields[2]
    $FunctionMatch = [regex]::Match(
        $Fields[1],
        '^reinterpret_cast\s*<\s*void\s*\*\s*>\s*\(\s*(?<function>[A-Za-z_][A-Za-z0-9_]*)\s*\)$')
    if ($null -eq $Name -or $null -eq $Signature -or -not $FunctionMatch.Success -or $Fields[3].Trim() -cne 'nullptr') {
        Add-Violation $Violations "$Description has an unparsed or non-canonical initializer: $Trimmed"
        return $null
    }
    return "$Name|$($FunctionMatch.Groups['function'].Value)|$Signature|nullptr"
}

function Test-NativeSymbolArray {
    param(
        [string]$Source,
        [string]$ArrayName,
        [string[]]$ExpectedRecords,
        [System.Collections.Generic.List[string]]$Violations
    )

    $Description = "$ArrayName native initializer"
    $Region = Get-UniqueBraceRegion `
        $Source `
        ('\bNativeSymbol\s+' + [regex]::Escape($ArrayName) + '\s*\[\s*\]\s*=') `
        $Violations `
        $Description
    if ($null -eq $Region) {
        return
    }
    $Candidates = @(Split-TopLevel $Region.Body $Violations $Description)
    $ActualRecords = [System.Collections.Generic.List[string]]::new()
    foreach ($Candidate in $Candidates) {
        $Record = ConvertTo-NativeSymbolRecord $Candidate $Violations $Description
        if ($null -ne $Record) {
            $ActualRecords.Add($Record)
        }
    }
    if ($ActualRecords.Count -ne $Candidates.Count) {
        Add-Violation $Violations "$Description parse completeness failed: candidates=$($Candidates.Count) parsed=$($ActualRecords.Count)"
    }
    Test-ExactMultiset @($ActualRecords) $ExpectedRecords $Violations $Description
}

function ConvertTo-SpecRecord {
    param(
        [string]$Candidate,
        [string]$FactoryName,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Calls = @(Get-CallArguments $Candidate $FactoryName $Violations $Description)
    if ($Calls.Count -ne 1) {
        Add-Violation $Violations "$Description candidate is not exactly one $FactoryName call: $Candidate"
        return $null
    }
    $WithoutWhitespace = [regex]::Replace($Candidate, '\s+', '')
    $CallWithoutWhitespace = [regex]::Replace($Calls[0].Text, '\s+', '')
    if ($WithoutWhitespace -cne $CallWithoutWhitespace -or $Calls[0].Arguments.Count -ne 4) {
        Add-Violation $Violations "$Description candidate is not a complete four-argument factory call: $Candidate"
        return $null
    }

    $Kind = [regex]::Replace($Calls[0].Arguments[0], '\s+', '')
    $Identity = ConvertFrom-CppStringArgument $Calls[0].Arguments[1]
    $ImportName = ConvertFrom-CppStringArgument $Calls[0].Arguments[2]
    $Signature = ConvertFrom-CppStringArgument $Calls[0].Arguments[3]
    if ($null -eq $Identity -or $null -eq $ImportName -or $null -eq $Signature) {
        Add-Violation $Violations "$Description has an unparsed identity/import/signature: $Candidate"
        return $null
    }
    return "$Kind|$Identity|$ImportName|$Signature"
}

function Test-SpecInitializer {
    param(
        [string]$Source,
        [string]$GetSpecsPattern,
        [string]$FactoryName,
        [string[]]$ExpectedRecords,
        [System.Collections.Generic.List[string]]$Violations,
        [string]$Description
    )

    $Function = Get-UniqueBraceRegion $Source $GetSpecsPattern $Violations "$Description GetSpecs"
    if ($null -eq $Function) {
        return
    }
    $Specs = Get-UniqueBraceRegion `
        $Function.Body `
        'static\s+const\s+TArray\s*<[^>]+>\s+Specs\s*=' `
        $Violations `
        "$Description initializer"
    if ($null -eq $Specs) {
        return
    }

    $Candidates = @(Split-TopLevel $Specs.Body $Violations "$Description initializer")
    $ActualRecords = [System.Collections.Generic.List[string]]::new()
    foreach ($Candidate in $Candidates) {
        $Record = ConvertTo-SpecRecord $Candidate $FactoryName $Violations $Description
        if ($null -ne $Record) {
            $ActualRecords.Add($Record)
        }
    }
    if ($ActualRecords.Count -ne $Candidates.Count) {
        Add-Violation $Violations "$Description parse completeness failed: candidates=$($Candidates.Count) parsed=$($ActualRecords.Count)"
    }
    Test-ExactMultiset @($ActualRecords) $ExpectedRecords $Violations $Description
}

function Test-WamrRegistration {
    param(
        [string]$Source,
        [System.Collections.Generic.List[string]]$Violations
    )

    $RegisterFunction = Get-UniqueBraceRegion `
        $Source `
        '\bbool\s+RegisterAvidScriptWamrHostBindings\s*\(' `
        $Violations `
        'WAMR registration function'
    $UnregisterFunction = Get-UniqueBraceRegion `
        $Source `
        '\bvoid\s+UnregisterAvidScriptWamrHostBindings\s*\(' `
        $Violations `
        'WAMR unregistration function'
    if ($null -eq $RegisterFunction -or $null -eq $UnregisterFunction) {
        return
    }

    $RegisterCalls = @(Get-CallArguments `
        $RegisterFunction.Body `
        'wasm_runtime_register_natives' `
        $Violations `
        'WAMR registration call')
    $ActualRegistration = [System.Collections.Generic.List[string]]::new()
    foreach ($Call in $RegisterCalls) {
        if ($Call.Arguments.Count -ne 3) {
            Add-Violation $Violations "WAMR registration call has $($Call.Arguments.Count) arguments"
            continue
        }
        $ActualRegistration.Add(
            (@($Call.Arguments | ForEach-Object { [regex]::Replace($_, '\s+', '') }) -join '|'))
    }
    Test-ExactMultiset `
        @($ActualRegistration) `
        @(
            'CanonicalModuleName|GNativeSymbols|UE_ARRAY_COUNT(GNativeSymbols)',
            'CompatibilityModuleName|GCompatibilityNativeSymbols|UE_ARRAY_COUNT(GCompatibilityNativeSymbols)'
        ) `
        $Violations `
        'WAMR registration calls'

    $RegisterRollbackCalls = @(Get-CallArguments `
        $RegisterFunction.Body `
        'wasm_runtime_unregister_natives' `
        $Violations `
        'WAMR registration rollback')
    $ActualRollback = @(
        $RegisterRollbackCalls | ForEach-Object {
            if ($_.Arguments.Count -ne 2) {
                Add-Violation $Violations 'WAMR registration rollback must have two arguments'
                ''
            }
            else {
                @($_.Arguments | ForEach-Object { [regex]::Replace($_, '\s+', '') }) -join '|'
            }
        } | Where-Object { $_.Length -gt 0 })
    Test-ExactMultiset `
        $ActualRollback `
        @('CanonicalModuleName|GNativeSymbols') `
        $Violations `
        'WAMR registration rollback'

    $UnregisterCalls = @(Get-CallArguments `
        $UnregisterFunction.Body `
        'wasm_runtime_unregister_natives' `
        $Violations `
        'WAMR unregistration call')
    $ActualUnregistration = @(
        $UnregisterCalls | ForEach-Object {
            if ($_.Arguments.Count -ne 2) {
                Add-Violation $Violations 'WAMR unregistration call must have two arguments'
                ''
            }
            else {
                @($_.Arguments | ForEach-Object { [regex]::Replace($_, '\s+', '') }) -join '|'
            }
        } | Where-Object { $_.Length -gt 0 })
    Test-ExactMultiset `
        $ActualUnregistration `
        @(
            'CompatibilityModuleName|GCompatibilityNativeSymbols',
            'CanonicalModuleName|GNativeSymbols'
        ) `
        $Violations `
        'WAMR unregistration calls'

    Test-DirectCallAllowlist `
        $RegisterFunction.Body `
        @('wasm_runtime_register_natives', 'wasm_runtime_unregister_natives') `
        $Violations `
        'WAMR registration function'
    Test-DirectCallAllowlist `
        $UnregisterFunction.Body `
        @('wasm_runtime_unregister_natives') `
        $Violations `
        'WAMR unregistration function'
}

function Test-StaticImportPolicy {
    param(
        [string]$Source,
        [string[]]$CompatibilityNames,
        [System.Collections.Generic.List[string]]$Violations
    )

    $Policy = Get-UniqueBraceRegion `
        $Source `
        '\bbool\s+IsAvidScriptVmStaticHostImport\s*\(' `
        $Violations `
        'static host import policy'
    if ($null -eq $Policy) {
        return
    }
    $Initializer = Get-UniqueBraceRegion `
        $Policy.Body `
        'static\s+const\s+TSet\s*<\s*FString\s*>\s+StaticImports\s*=' `
        $Violations `
        'static host import policy initializer'
    if ($null -eq $Initializer) {
        return
    }
    $Candidates = @(Split-TopLevel $Initializer.Body $Violations 'static host import policy initializer')
    $ActualNames = [System.Collections.Generic.List[string]]::new()
    foreach ($Candidate in $Candidates) {
        $Name = ConvertFrom-CppStringArgument $Candidate
        if ($null -eq $Name) {
            Add-Violation $Violations "static host import policy has an unparsed candidate: $Candidate"
        }
        else {
            $ActualNames.Add($Name)
        }
    }
    if ($ActualNames.Count -ne $Candidates.Count) {
        Add-Violation $Violations "static host import policy parse completeness failed: candidates=$($Candidates.Count) parsed=$($ActualNames.Count)"
    }
    Test-ExactMultiset @($ActualNames) $CompatibilityNames $Violations 'static host import policy'
    Test-RegexSequence `
        $Policy.Text `
        @(
            'ModuleName\s*!=\s*TEXT\s*\(\s*"avidscript"\s*\)',
            'ModuleName\s*!=\s*TEXT\s*\(\s*"env"\s*\)',
            'StaticImports\s*\.\s*Contains\s*\(\s*ImportName\s*\)',
            'ModuleName\s*==\s*TEXT\s*\(\s*"avidscript"\s*\)',
            'ImportName\s*==\s*TEXT\s*\(\s*"avid_owner_get_handle"\s*\)'
        ) `
        $Violations `
        'static host import module/name policy'
}

function Test-RendererCandidates {
    param(
        [string]$Source,
        [string]$StateContractRendererSource,
        [string]$DefaultValueFormatterSource,
        [string]$SyntaxSource,
        [string]$LifecycleBindingSource,
        [System.Collections.Generic.List[string]]$Violations
    )

    $Literals = @(Get-CppStringLiterals $Source $Violations 'C# renderer')
    Test-RendererFrozenRegions $Source $Violations
    Test-GeneratedSurfaceConstructionClosure `
        $Source `
        $StateContractRendererSource `
        $DefaultValueFormatterSource `
        $SyntaxSource `
        $LifecycleBindingSource `
        $Violations
    $LiteralStream = $Literals -join ''
    $EntryPointTokenCount = [regex]::Matches($LiteralStream, '\bEntryPoint\b').Count
    $EntryPointMatches = @(
        [regex]::Matches($LiteralStream, 'EntryPoint\s*=\s*"(?<name>[^"]+)"'))
    if ($EntryPointMatches.Count -ne $EntryPointTokenCount) {
        Add-Violation $Violations "renderer EntryPoint parse completeness failed: candidates=$EntryPointTokenCount parsed=$($EntryPointMatches.Count)"
    }
    $ParsedEntryPoints = @($EntryPointMatches | ForEach-Object { $_.Groups['name'].Value })
    Test-ExactMultiset `
        @($ParsedEntryPoints) `
        @(
            '%s',
            '%s',
            '%s',
            '%s',
            'avid_owner_get_handle',
            'timer_set_once',
            'timer_cancel',
            'avid_object_type_is_a'
        ) `
        $Violations `
        'renderer EntryPoint candidates'

    $AllowedAvidLiterals = @(
        'avid_owner_get_handle',
        'avid_object_type_is_a'
    )
    $AvidTokens = [System.Collections.Generic.List[string]]::new()
    foreach ($Literal in $Literals) {
        foreach ($Match in [regex]::Matches($Literal, '\bavid_[a-z0-9_]+\b')) {
            $AvidTokens.Add($Match.Value)
        }
    }
    $UnexpectedAvidTokens = @(
        $AvidTokens | Where-Object { $AllowedAvidLiterals -cnotcontains $_ } | Sort-Object -Unique)
    if ($UnexpectedAvidTokens.Count -gt 0) {
        Add-Violation $Violations "renderer contains bespoke avid_* literals: $($UnexpectedAvidTokens -join ', ')"
    }
    Test-GeneratedLiteralStream `
        $Source `
        $AllowedAvidLiterals `
        @(
            'FVector',
            'InputEvent',
            'FRotator',
            'FTransform',
            'AvidScriptBindingPackage',
            'TSubclassOfAActor',
            'ProjectClasses',
            'FAvidScriptObjectHandle',
            'UE',
            'AvidScriptRuntimeNative',
            'AvidScriptNative'
        ) `
        $Violations `
        'C# renderer'

    $DeclarationCandidates = @(
        $Literals | Where-Object {
            $_ -match '\b(?:public|internal)\s+(?:(?:readonly|static)\s+)?(?:struct|class|enum)\b'
        })
    $ParsedDeclarations = [System.Collections.Generic.List[string]]::new()
    foreach ($Candidate in $DeclarationCandidates) {
        $Matches = [regex]::Matches(
            $Candidate,
            '\b(?<access>public|internal)\s+(?:(?<modifier>readonly|static)\s+)?(?<kind>struct|class|enum)(?:\s+(?<name>[A-Za-z_][A-Za-z0-9_]*))?')
        if ($Matches.Count -ne 1) {
            Add-Violation $Violations "renderer has an unparsed wrapper candidate: $Candidate"
            continue
        }
        $Modifier = if ($Matches[0].Groups['modifier'].Success) {
            $Matches[0].Groups['modifier'].Value
        }
        else {
            '-'
        }
        $Name = if ($Matches[0].Groups['name'].Success) {
            $Matches[0].Groups['name'].Value
        }
        else {
            '<dynamic>'
        }
        $ParsedDeclarations.Add(
            "$($Matches[0].Groups['access'].Value)|$Modifier|$($Matches[0].Groups['kind'].Value)|$Name")
    }
    if ($ParsedDeclarations.Count -ne $DeclarationCandidates.Count) {
        Add-Violation $Violations "renderer wrapper parse completeness failed: candidates=$($DeclarationCandidates.Count) parsed=$($ParsedDeclarations.Count)"
    }
    Test-ExactMultiset `
        @($ParsedDeclarations) `
        @(
            'public|readonly|struct|FVector',
            'public|readonly|struct|InputEvent',
            'public|readonly|struct|FRotator',
            'public|readonly|struct|FTransform',
            'internal|static|class|AvidScriptBindingPackage',
            'public|readonly|struct|<dynamic>',
            'public|readonly|struct|TSubclassOfAActor',
            'public|readonly|struct|<dynamic>',
            'public|static|class|ProjectClasses',
            'public|-|enum|<dynamic>',
            'internal|readonly|struct|FAvidScriptObjectHandle',
            'public|static|class|<dynamic>',
            'public|static|class|UE',
            'internal|static|class|AvidScriptRuntimeNative',
            'internal|static|class|AvidScriptNative'
        ) `
        $Violations `
        'renderer generated wrapper declarations'
}

function Test-GeneratedImportLiterals {
    param(
        [string]$Source,
        [System.Collections.Generic.List[string]]$Violations
    )

    $Literals = @(Get-CppStringLiterals $Source $Violations 'descriptor import generator')
    $LiteralStream = $Literals -join ''
    $Candidates = [System.Collections.Generic.List[string]]::new()
    foreach ($Literal in $Literals) {
        foreach ($Match in [regex]::Matches($Literal, '\bavid_[a-z0-9_]*\b')) {
            $Candidates.Add($Match.Value)
        }
    }
    if ($Candidates.Count -eq 0) {
        Add-Violation $Violations 'descriptor import generator has no avid_ namespace candidate'
        return
    }
    $Unexpected = @($Candidates | Where-Object { $_ -cne 'avid_ue_' } | Sort-Object -Unique)
    if ($Unexpected.Count -gt 0) {
        Add-Violation $Violations "descriptor import generator contains bespoke avid_* literals: $($Unexpected -join ', ')"
    }
    $UnexpectedSplit = @(
        [regex]::Matches($LiteralStream, '\bavid_[a-z0-9_]+') |
            ForEach-Object { $_.Value } |
            Where-Object { -not $_.StartsWith('avid_ue_', [System.StringComparison]::Ordinal) } |
            Sort-Object -Unique)
    if ($UnexpectedSplit.Count -gt 0) {
        Add-Violation $Violations "descriptor import generator contains bespoke split-capable avid_* literals: $($UnexpectedSplit -join ', ')"
    }
}

function Test-TypedCastDispatch {
    param(
        [string]$InvocationSource,
        [string]$RegistryHeader,
        [string]$RegistrySource,
        [System.Collections.Generic.List[string]]$Violations
    )

    $Dispatch = Get-UniqueBraceRegion `
        $InvocationSource `
        '\bbool\s+DispatchAvidScriptObjectType\s*\(' `
        $Violations `
        'typed cast dispatch'
    $ResolveObject = Get-UniqueBraceRegion `
        $RegistrySource `
        '\bUObject\s*\*\s*FAvidScriptObjectRegistry::ResolveObject\s*\(' `
        $Violations `
        'typed cast UObject resolver'
    $RegisterObject = Get-UniqueBraceRegion `
        $RegistrySource `
        '\bFAvidScriptObjectHandle\s+FAvidScriptObjectRegistry::RegisterObject\s*\(' `
        $Violations `
        'typed cast UObject registration'
    $ReleaseHandle = Get-UniqueBraceRegion `
        $RegistrySource `
        '\bbool\s+FAvidScriptObjectRegistry::ReleaseHandle\s*\(' `
        $Violations `
        'typed cast UObject release'
    $ResolveType = Get-UniqueBraceRegion `
        $InvocationSource `
        '\bbool\s+FAvidScriptBindingPackage::TryResolveObjectType\s*\(' `
        $Violations `
        'typed cast UClass plan resolver'
    $DispatchFailure = Get-UniqueBraceRegion `
        $InvocationSource `
        '\bvoid\s+SetAvidScriptBindingDispatchFailure\s*\(' `
        $Violations `
        'typed cast dispatch failure leaf'
    $SetSuccess = Get-UniqueBraceRegion `
        $RegistrySource `
        '\bvoid\s+FAvidScriptObjectRegistry::SetSuccess\s*\(' `
        $Violations `
        'typed cast registry success leaf'
    $SetFailure = Get-UniqueBraceRegion `
        $RegistrySource `
        '\bvoid\s+FAvidScriptObjectRegistry::SetFailure\s*\(' `
        $Violations `
        'typed cast registry failure leaf'
    if ($null -eq $Dispatch -or
        $null -eq $ResolveObject -or
        $null -eq $RegisterObject -or
        $null -eq $ReleaseHandle -or
        $null -eq $ResolveType -or
        $null -eq $DispatchFailure -or
        $null -eq $SetSuccess -or
        $null -eq $SetFailure) {
        return
    }

    Test-RegistryPathSurface $RegistrySource $Violations 'typed cast object registry'
    $ExpectedRegistryHashes = [ordered]@{
        'ObjectRegistryHeader' = '2d535d0d14eb057679cc7c6c208afbda3ad7610d1c74a25c88ef06cb4d4045f2'
        'ObjectRegistrySource' = 'a1100acc84ee22dd77c0e00d13c1167ff4cdde03273f9b584cf30e9b0e0e36c3'
    }
    foreach ($RegistryHashEntry in ([ordered]@{
        'ObjectRegistryHeader' = $RegistryHeader
        'ObjectRegistrySource' = $RegistrySource
    }).GetEnumerator()) {
        $ActualHash = Get-CanonicalCodeSha256 $RegistryHashEntry.Value
        if ($ActualHash -cne $ExpectedRegistryHashes[$RegistryHashEntry.Key]) {
            Add-Violation $Violations "frozen registry implementation $($RegistryHashEntry.Key) changed: actual=$ActualHash expected=$($ExpectedRegistryHashes[$RegistryHashEntry.Key])"
        }
    }
    $Closure = Remove-SourceStrings (
        $Dispatch.Text + "`n" +
        $ResolveObject.Text + "`n" +
        $ResolveType.Text + "`n" +
        $DispatchFailure.Text)
    foreach ($ForbiddenLookup in @('FindObject', 'LoadObject', 'StaticLoadObject', 'GetPathName')) {
        if ($Closure -match ('\b' + $ForbiddenLookup + '\s*(?:<[^>]+>)?\s*\(')) {
            Add-Violation $Violations "typed cast reviewed closure contains path lookup $ForbiddenLookup"
        }
    }
    if ($Closure -match '\bAActor\b') {
        Add-Violation $Violations 'typed cast reviewed closure contains Actor-only dispatch'
    }

    Test-DirectCallAllowlist `
        $Dispatch.Body `
        @(
            'SetAvidScriptBindingDispatchFailure',
            'Context.ObjectRegistry->ResolveObject',
            'Package.TryResolveObjectType',
            'Object->IsA'
        ) `
        $Violations `
        'typed cast dispatch'
    Test-DirectCallAllowlist `
        $ResolveObject.Body `
        @(
            'Handle.IsValid',
            'Slots.IsValidIndex',
            'SetFailure',
            'Slot.Object.Get',
            'IsValid',
            'SetSuccess'
        ) `
        $Violations `
        'typed cast UObject resolver'
    Test-DirectCallAllowlist `
        $ResolveType.Body `
        @('Impl->ObjectTypePlans.IsValidIndex') `
        $Violations `
        'typed cast UClass plan resolver'
    Test-DirectCallAllowlist `
        $DispatchFailure.Body `
        @('FAvidScriptDynamicHostCallResult', 'FString::Printf', 'Source.IsEmpty') `
        $Violations `
        'typed cast dispatch failure leaf'
    Test-DirectCallAllowlist `
        $SetSuccess.Body `
        @('FAvidScriptObjectHandleResult', 'Object->GetPathName') `
        $Violations `
        'typed cast registry success leaf'
    Test-DirectCallAllowlist `
        $SetFailure.Body `
        @('FAvidScriptObjectHandleResult', 'Object->GetPathName', 'FString::Printf', 'OutResult.ObjectPath.IsEmpty') `
        $Violations `
        'typed cast registry failure leaf'

    foreach ($DiagnosticLeaf in @($SetSuccess, $SetFailure)) {
        Test-RegexSequence `
            $DiagnosticLeaf.Text `
            @(
                'bool\s+bIncludeObjectPath',
                'if\s*\(\s*bIncludeObjectPath\s*&&\s*Object\s*!=\s*nullptr\s*\)',
                'OutResult\s*\.\s*ObjectPath\s*=\s*Object\s*->\s*GetPathName\s*\(\s*\)'
            ) `
            $Violations `
            'typed cast registry diagnostic path guard'
        Test-SingleCallInsideGuard `
            $DiagnosticLeaf.Body `
            '\bif\s*\(\s*bIncludeObjectPath\s*&&\s*Object\s*!=\s*nullptr\s*\)' `
            'GetPathName' `
            $Violations `
            'typed cast registry diagnostic path'
    }
    $ExpectedDiagnosticLeafHashes = [ordered]@{
        'SetSuccess' = '2beff04f9ef5cb619939fd36487500155c02cec7866426ea3c080aa461473317'
        'SetFailure' = '2673b4a7d2edc663de8c13b7de7612065cfbb472bee40a39dae405e5fbf8afa4'
    }
    foreach ($DiagnosticLeafEntry in ([ordered]@{
        'SetSuccess' = $SetSuccess
        'SetFailure' = $SetFailure
    }).GetEnumerator()) {
        $ActualHash = Get-CanonicalCodeSha256 $DiagnosticLeafEntry.Value.Text
        if ($ActualHash -cne $ExpectedDiagnosticLeafHashes[$DiagnosticLeafEntry.Key]) {
            Add-Violation $Violations "frozen registry diagnostic leaf $($DiagnosticLeafEntry.Key) changed: actual=$ActualHash expected=$($ExpectedDiagnosticLeafHashes[$DiagnosticLeafEntry.Key])"
        }
    }
    foreach ($RegistryEntry in @(
        [pscustomobject]@{ Name = 'RegisterObject'; Region = $RegisterObject },
        [pscustomobject]@{ Name = 'ResolveObject'; Region = $ResolveObject },
        [pscustomobject]@{ Name = 'ReleaseHandle'; Region = $ReleaseHandle }
    )) {
        Test-DiagnosticForwarding `
            $RegistryEntry.Region.Body `
            "typed cast $($RegistryEntry.Name)" `
            $Violations
    }
    Test-RegexSequence `
        $RegistryHeader `
        @(
            'SetFailure\s*\(',
            'Object\s*,\s*bIncludeObjectPath\s*,\s*TEXT\s*\(\s*"Use a handle API that matches the registered UObject type\."\s*\)',
            'static\s+void\s+SetFailure\s*\(',
            'const\s+UObject\s*\*\s*Object\s*,\s*bool\s+bIncludeObjectPath\s*,\s*const\s+TCHAR\s*\*\s*NextAction'
        ) `
        $Violations `
        'typed registry template diagnostic forwarding'

    Test-RegexSequence `
        $Dispatch.Text `
        @(
            'UObject\s*\*\s*Object\s*=\s*Context\s*\.\s*ObjectRegistry\s*->\s*ResolveObject\s*\([^;]+false\s*\)',
            'UClass\s*\*\s*CachedClass\s*=\s*nullptr',
            'Package\s*\.\s*TryResolveObjectType\s*\(',
            'Object\s*->\s*IsA\s*\(\s*CachedClass\s*\)'
        ) `
        $Violations `
        'typed cast UObject/UClass dispatch'
    if ($ResolveObject.Text -notmatch '\bUObject\s*\*\s*FAvidScriptObjectRegistry::ResolveObject\s*\(' -or
        $ResolveObject.Text -notmatch '\bUObject\s*\*\s*Object\s*=\s*Slot\s*\.\s*Object\s*\.\s*Get\s*\(' -or
        $ResolveType.Text -notmatch '\bUClass\s*\*&\s*OutClass\b') {
        Add-Violation $Violations 'typed cast helper contracts must preserve UObject input and cached UClass output'
    }
}

function Invoke-Phase50Contracts {
    param(
        [hashtable]$Inputs,
        [System.Collections.Generic.List[string]]$Violations
    )

    $DescriptorHeader = $Inputs['Source/AvidScriptBindings/Public/AvidScriptBindingDescriptor.h'].Code
    $DescriptorSource = $Inputs['Source/AvidScriptBindings/Private/AvidScriptBindingDescriptor.cpp'].Code
    $DescriptorGenerator = $Inputs['Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDescriptorGenerator.cpp'].Code
    $BindingEmitter = $Inputs['Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpBindingEmitter.cpp'].Code
    $ObjectTypeBindings = $Inputs['Source/AvidScriptBindings/Private/AvidScriptObjectTypeBinding.cpp'].Code
    $LifecycleBindings = $Inputs['Source/AvidScriptBindings/Private/AvidScriptObjectLifecycleBinding.cpp'].Code
    $HostBindings = $Inputs['Source/AvidScriptVM/Private/AvidScriptWamrHostBindings.cpp'].Code
    $DynamicRegistry = $Inputs['Source/AvidScriptVM/Private/AvidScriptWamrDynamicRegistry.cpp'].Code
    $Renderer = $Inputs['Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpBindingRenderer.cpp'].Code
    $StateContractRenderer = $Inputs['Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpStateContractRenderer.cpp'].Code
    $DefaultValueFormatter = $Inputs['Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpDefaultValueFormatter.cpp'].Code
    $Syntax = $Inputs['Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpSyntax.cpp'].Code
    $OperationLowerer = $Inputs['Tools/AvidScript.CSharpGuest/Lowering/CSharpOperationLowerer.cs'].Code
    $Invocation = $Inputs['Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp'].Code
    $ObjectRegistryHeader = $Inputs['Source/AvidScriptBindings/Public/AvidScriptObjectRegistry.h'].Code
    $ObjectRegistry = $Inputs['Source/AvidScriptBindings/Private/AvidScriptObjectRegistry.cpp'].Code
    $Runtime = $Inputs['Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp'].Code

    foreach ($Field in @('ObjectTypeOrdinal', 'SelfTypeId', 'ResultTypeId')) {
        if ($DescriptorHeader -notmatch ('\b' + [regex]::Escape($Field) + '\b')) {
            Add-Violation $Violations "descriptor v6 model is missing $Field"
        }
    }

    $SelectionHash = Get-UniqueBraceRegion `
        $DescriptorSource `
        '\bFString\s+FAvidScriptBindingDescriptorIdentity::MakeSelectionHash\s*\(' `
        $Violations `
        'descriptor selection hash builder'
    $PackageHash = Get-UniqueBraceRegion `
        $DescriptorSource `
        '\bFString\s+FAvidScriptBindingDescriptorIdentity::MakePackageHash\s*\(' `
        $Violations `
        'descriptor package hash builder'
    $GenerateDescriptor = Get-UniqueBraceRegion `
        $DescriptorGenerator `
        '\bbool\s+GenerateBindingDescriptor\s*\(' `
        $Violations `
        'descriptor canonical generator'
    if ($null -ne $SelectionHash) {
        Test-RegexSequence `
            $SelectionHash.Text `
            @(
                'SelectionKeys\s*\.\s*Sort\s*\(',
                '"descriptor_selection_v6"',
                '"self_type_id"[\s\S]*Package\s*\.\s*SelfTypeId',
                '"object_type_ordinal"[\s\S]*Type\s*\.\s*ObjectTypeOrdinal',
                '"object_class_path"[\s\S]*Type\s*\.\s*ClassPath',
                '"object_base_type_id"[\s\S]*Type\s*\.\s*BaseTypeId',
                '"result_type_id"[\s\S]*Reference\s*\.\s*ResultTypeId',
                'return\s+FAvidScriptHash::Sha256HexUtf8\s*\(\s*Identity\s*\)'
            ) `
            $Violations `
            'descriptor v6 selection identity'
    }
    if ($null -ne $PackageHash) {
        Test-RegexSequence `
            $PackageHash.Text `
            @(
                '"descriptor_package_v6"',
                '"self_type_id"[\s\S]*Package\s*\.\s*SelfTypeId',
                '"object_type_ordinal"[\s\S]*Type\s*\.\s*ObjectTypeOrdinal',
                '"object_class_path"[\s\S]*Type\s*\.\s*ClassPath',
                '"object_base_type_id"[\s\S]*Type\s*\.\s*BaseTypeId',
                '"result_type_id"[\s\S]*Reference\s*\.\s*ResultTypeId',
                'return\s+FAvidScriptHash::Sha256HexUtf8\s*\(\s*Identity\s*\)'
            ) `
            $Violations `
            'descriptor v6 package identity'
    }
    if ($null -ne $GenerateDescriptor) {
        Test-RegexSequence `
            $GenerateDescriptor.Text `
            @(
                'Bindings\s*\.\s*Sort\s*\(',
                'Package\s*\.\s*ClassReferences\s*\.\s*Sort\s*\(',
                'Package\s*\.\s*Types\s*\.\s*Sort\s*\(',
                'Package\s*\.\s*SelfTypeId\s*=\s*SelfNode\s*->\s*TypeId',
                'Reference\s*\.\s*ResultTypeId\s*=\s*ResultNode\s*->\s*TypeId',
                'Package\s*\.\s*SelectionHash\s*=\s*FAvidScriptBindingDescriptorIdentity::MakeSelectionHash\s*\(\s*Package\s*\)',
                'Package\s*\.\s*PackageHash\s*=\s*FAvidScriptBindingDescriptorIdentity::MakePackageHash\s*\(\s*Package\s*\)'
            ) `
            $Violations `
            'descriptor stable ordering and hash publication'
    }

    $CanonicalValidation = Get-UniqueBraceRegion `
        $BindingEmitter `
        '\bbool\s+ValidateCanonicalDescriptor\s*\(' `
        $Violations `
        'canonical descriptor emission validation'
    if ($null -ne $CanonicalValidation) {
        Test-RegexSequence `
            $CanonicalValidation.Text `
            @(
                'Package\s*\.\s*SelfTypeId\s*\.\s*IsEmpty\s*\(',
                'Package\s*\.\s*Types\s*\.\s*FindByPredicate\s*\(',
                'Type\s*\.\s*StableId\s*==\s*Package\s*\.\s*SelfTypeId',
                'SelfType\s*->\s*ClassPath\s*\.\s*IsEmpty\s*\(',
                'Profile\s*\.\s*SelfClassPath\s*=\s*SelfType\s*->\s*ClassPath',
                'FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile\s*\(',
                'Profile\s*,\s*ClassReferences\s*,\s*ObjectFactories\s*,\s*CanonicalDescriptorJson\s*,\s*SelectionResult\s*,\s*RegenerationResult'
            ) `
            $Violations `
            'custom Self canonical regeneration path'
        if ($CanonicalValidation.Text -match 'GenerateWithClassReferences\s*\(') {
            Add-Violation $Violations 'canonical descriptor validation regressed to GenerateWithClassReferences and may discard custom Self'
        }
    }
    if ($null -ne $CanonicalValidation) {
        Test-RegexSequence `
            $CanonicalValidation.Text `
            @(
                'Package\s*\.\s*SchemaVersion\s*<\s*6',
                'FAvidScriptBindingPackage::LoadDescriptor\s*\(',
                'FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical\s*\(',
                'CanonicalDescriptorJson\s*!=\s*DescriptorJson',
                'FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile\s*\('
            ) `
            $Violations `
            'version-aware canonical descriptor compatibility'
    }

    $ObjectTypeFactory = Get-UniqueBraceRegion `
        $ObjectTypeBindings `
        '\bFAvidScriptObjectTypeBindingSpec\s+MakeObjectTypeSpec\s*\(' `
        $Violations `
        'object type spec factory'
    $LifecycleFactory = Get-UniqueBraceRegion `
        $LifecycleBindings `
        '\bFAvidScriptObjectLifecycleBindingSpec\s+MakeLifecycleSpec\s*\(' `
        $Violations `
        'object lifecycle spec factory'
    foreach ($Factory in @($ObjectTypeFactory, $LifecycleFactory)) {
        if ($null -ne $Factory) {
            Test-RegexSequence `
                $Factory.Text `
                @(
                    'Spec\s*\.\s*Kind\s*=\s*Kind',
                    'Spec\s*\.\s*StableId\s*=\s*FAvidScriptHash::Sha256HexUtf8\s*\(\s*CanonicalIdentity\s*\)',
                    'Spec\s*\.\s*ModuleName\s*=\s*TEXT\s*\(\s*"avidscript"\s*\)',
                    'Spec\s*\.\s*ImportName\s*=\s*ImportName',
                    'Spec\s*\.\s*Signature\s*=\s*Signature',
                    'return\s+Spec'
                ) `
                $Violations `
                'descriptor capability factory'
        }
    }
    Test-SpecInitializer `
        $ObjectTypeBindings `
        '\bTConstArrayView\s*<\s*FAvidScriptObjectTypeBindingSpec\s*>\s+FAvidScriptObjectTypeBindings::GetSpecs\s*\(' `
        'MakeObjectTypeSpec' `
        @(
            'EAvidScriptBindingInvocationKind::ObjectTypeIsA|avidscript.object_type.v1|is_a|object_handle,object_type_ordinal->i32|avid_object_type_is_a|(iii)i'
        ) `
        $Violations `
        'object type capability'
    Test-SpecInitializer `
        $LifecycleBindings `
        '\bTConstArrayView\s*<\s*FAvidScriptObjectLifecycleBindingSpec\s*>\s+FAvidScriptObjectLifecycleBindings::GetSpecs\s*\(' `
        'MakeLifecycleSpec' `
        @(
            'EAvidScriptBindingInvocationKind::ObjectSpawnActor|avidscript.lifecycle.v1|spawn_actor|class_ref,transform_ptr,handle_ptr->i32|avid_object_spawn_actor|(iii)i',
            'EAvidScriptBindingInvocationKind::ObjectDestroyActor|avidscript.lifecycle.v1|destroy_actor|object_handle->i32|avid_object_destroy_actor|(ii)i',
            'EAvidScriptBindingInvocationKind::ObjectIsA|avidscript.lifecycle.v1|is_a|object_handle,class_ref->i32|avid_object_is_a|(iii)i'
        ) `
        $Violations `
        'object lifecycle capability'

    $CanonicalNativeRecords = @(
        'host_add_i32|HostAddI32|(i)i|nullptr',
        'host_fail_i32|HostFailI32|(i)i|nullptr',
        'actor_get_location|ActorGetLocation|(iii)i|nullptr',
        'actor_set_location|ActorSetLocation|(iifff)i|nullptr',
        'actor_add_location_offset|ActorAddLocationOffset|(iifff)i|nullptr',
        'actor_get_rotation|ActorGetRotation|(iii)i|nullptr',
        'actor_set_rotation|ActorSetRotation|(iifff)i|nullptr',
        'actor_get_scale|ActorGetScale|(iii)i|nullptr',
        'actor_set_scale|ActorSetScale|(iifff)i|nullptr',
        'actor_get_transform_batch|ActorGetTransformBatch|(iii)i|nullptr',
        'actor_get_root_component|ActorGetRootComponent|(iii)i|nullptr',
        'scene_component_get_world_location|SceneComponentGetWorldLocation|(iii)i|nullptr',
        'scene_component_set_world_location|SceneComponentSetWorldLocation|(iifff)i|nullptr',
        'owner_get_slot|OwnerGetSlot|()i|nullptr',
        'owner_get_generation|OwnerGetGeneration|()i|nullptr',
        'avid_owner_get_handle|OwnerGetHandle|()I|nullptr',
        'timer_set_once|TimerSetOnce|(fi)i|nullptr',
        'timer_cancel|TimerCancel|(i)i|nullptr',
        'avid_data_lane_epoch|DataLaneGetEpoch|()I|nullptr',
        'avid_data_lane_submit|DataLaneSubmit|(ii)i|nullptr'
    )
    $CompatibilityNativeRecords = @(
        $CanonicalNativeRecords | Where-Object {
            -not $_.StartsWith('avid_owner_get_handle|') `
                -and -not $_.StartsWith('avid_data_lane_epoch|') `
                -and -not $_.StartsWith('avid_data_lane_submit|')
        })
    $CompatibilityNames = @(
        $CompatibilityNativeRecords | ForEach-Object { ($_ -split '\|', 2)[0] })
    Test-NativeSymbolArray $HostBindings 'GNativeSymbols' $CanonicalNativeRecords $Violations
    Test-NativeSymbolArray $HostBindings 'GCompatibilityNativeSymbols' $CompatibilityNativeRecords $Violations
    Test-WamrRegistration $HostBindings $Violations
    Test-StaticImportPolicy $HostBindings $CompatibilityNames $Violations

    $OwnerFunction = Get-UniqueBraceRegion `
        $HostBindings `
        '\bint64_t\s+OwnerGetHandle\s*\(' `
        $Violations `
        'packed owner host function'
    if ($null -ne $OwnerFunction) {
        Test-RegexSequence `
            $OwnerFunction.Text `
            @(
                'Call\s*\.\s*BindingId\s*=\s*EAvidScriptHostBindingId::OwnerGetHandle',
                'Dispatch\s*\(\s*ExecEnv\s*,\s*"avid_owner_get_handle"\s*,\s*Call\s*,\s*Result\s*\)',
                'Result\s*\.\s*ReturnValueI64'
            ) `
            $Violations `
            'packed owner host function'
    }
    $RuntimeOwner = Get-UniqueBraceRegion `
        $Runtime `
        '\bint64\s+FAvidScriptWasmRuntimeInstance::HandleOwnerGetHandleImport\s*\(' `
        $Violations `
        'packed owner runtime handler'
    $RuntimeDispatch = Get-UniqueBraceRegion `
        $Runtime `
        '\bbool\s+FAvidScriptWasmRuntimeInstance::DispatchHostCall\s*\(' `
        $Violations `
        'runtime host dispatch'
    if ($null -ne $RuntimeOwner) {
        Test-RegexSequence `
            $RuntimeOwner.Text `
            @(
                'FAvidScriptObjectHandle\s+OwnerHandle\s*=\s*HostContext\s*\.\s*OwnerHandle',
                'static_cast\s*<\s*uint64\s*>\s*\(\s*OwnerHandle\s*\.\s*Slot\s*\)',
                'static_cast\s*<\s*uint64\s*>\s*\(\s*OwnerHandle\s*\.\s*Generation\s*\)\s*<<\s*32',
                'return\s+static_cast\s*<\s*int64\s*>\s*\(\s*PackedHandle\s*\)'
            ) `
            $Violations `
            'packed owner runtime handler'
    }
    if ($null -ne $RuntimeDispatch) {
        Test-RegexSequence `
            $RuntimeDispatch.Text `
            @(
                'case\s+EAvidScriptHostBindingId::OwnerGetHandle\s*:',
                'HandleOwnerGetHandleImport\s*\(',
                'FinishI64\s*\(\s*Value\s*,\s*Value\s*!=\s*0\s*\)'
            ) `
            $Violations `
            'packed owner runtime dispatch'
    }

    Test-GeneratedImportLiterals $DescriptorGenerator $Violations
    Test-RendererCandidates `
        $Renderer `
        $StateContractRenderer `
        $DefaultValueFormatter `
        $Syntax `
        $LifecycleBindings `
        $Violations
    foreach ($RendererFunctionSpec in @(
        [pscustomobject]@{
            Pattern = '\bbool\s+RenderMethod\s*\('
            Description = 'generated method renderer'
        },
        [pscustomobject]@{
            Pattern = '\bbool\s+RenderPropertyGetter\s*\('
            Description = 'generated property renderer'
        }
    )) {
        $RendererFunction = Get-UniqueBraceRegion `
            $Renderer `
            $RendererFunctionSpec.Pattern `
            $Violations `
            $RendererFunctionSpec.Description
        if ($null -ne $RendererFunction) {
            Test-RegexSequence `
                $RendererFunction.Text `
                @(
                    'EscapeCSharpString\s*\(\s*Binding\s*\.\s*HostImport\s*\.\s*Module\s*\)',
                    'EscapeCSharpString\s*\(\s*Binding\s*\.\s*HostImport\s*\.\s*Name\s*\)'
                ) `
                $Violations `
                "$($RendererFunctionSpec.Description) descriptor import"
        }
    }
    $ReferenceEmitter = Get-UniqueBraceRegion `
        $Renderer `
        '\bbool\s+FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource\s*\(' `
        $Violations `
        'generated C# facade publisher'
    if ($null -ne $ReferenceEmitter) {
        Test-RegexSequence `
            $ReferenceEmitter.Text `
            @(
                'for\s*\(\s*const\s+FAvidScriptBindingClassReferenceModel\s*&\s*Reference\s*:\s*Package\s*\.\s*ClassReferences\s*\)',
                'for\s*\(\s*const\s+FAvidScriptBindingTypeModel\s*\*\s*Type\s*:\s*ObjectTypes\s*\)',
                'AppendObjectHandleProxy\s*\(',
                'EscapeCSharpString\s*\(\s*Spec\s*\.\s*ModuleName\s*\)',
                'EscapeCSharpString\s*\(\s*Spec\s*\.\s*ImportName\s*\)'
            ) `
            $Violations `
            'descriptor-driven typed facade'
    }
    $ObjectHandleProxyRenderer = Get-UniqueBraceRegion `
        $Renderer `
        '\bvoid\s+AppendObjectHandleProxy\s*\(' `
        $Violations `
        'object handle proxy renderer'
    if ($null -ne $ObjectHandleProxyRenderer) {
        Test-RegexSequence `
            $ObjectHandleProxyRenderer.Text `
            @(
                'public static %s TryCast\(%s value\)',
                'AvidScriptNative\.ObjectTypeIsA\(value\.Slot, value\.Generation, %d\)'
            ) `
            $Violations `
            'static Derived.TryCast(Base) renderer'
        if ($ObjectHandleProxyRenderer.Text -match 'public\s+%s\s+TryCast\s*\(\s*\)') {
            Add-Violation $Violations 'typed facade regressed to inverse instance TryCast'
        }
    }
    if ($Renderer -notmatch 'internal static extern int ObjectTypeIsA\(int slot, int generation, int targetOrdinal\);') {
        Add-Violation $Violations 'typed facade is missing the ObjectTypeIsA native declaration'
    }
    foreach ($DynamicContract in @(
        'Import.ModuleName != TEXT("avidscript")',
        'IsAvidScriptDynamicSafeToken(Import.ImportName)',
        'IsAvidScriptVmStaticHostImport(Import.ModuleName, Import.ImportName)',
        'InvokeAvidScriptDynamicRawImport'
    )) {
        if (-not $DynamicRegistry.Contains($DynamicContract)) {
            Add-Violation $Violations "dynamic import registry is missing generic contract: $DynamicContract"
        }
    }

    $LowerConversion = Get-UniqueBraceRegion `
        $OperationLowerer `
        '\bprivate\s+static\s+GuestRegister\?\s+LowerConversion\s*\(' `
        $Violations `
        'C# conversion lowering'
    if ($null -ne $LowerConversion) {
        $UserDefinedBranch = Get-UniqueBraceRegion `
            $LowerConversion.Body `
            '\bif\s*\(\s*operation\s*\.\s*Conversion\s*\.\s*IsUserDefined\s*\)' `
            $Violations `
            'user-defined conversion branch'
        if ($null -ne $UserDefinedBranch) {
            Test-RegexSequence `
                $UserDefinedBranch.Text `
                @(
                    'operation\s*\.\s*Conversion\s*\.\s*IsUserDefined',
                    'operation\s*\.\s*Conversion\s*\.\s*MethodSymbolId',
                    'context\s*\.\s*TryGetCallTarget\s*\(',
                    '!\s*callable\s*\.\s*IsStatic',
                    'callable\s*\.\s*IsConstructor',
                    '!\s*callable\s*\.\s*HasBody',
                    'callable\s*\.\s*Import\s+is\s+not\s+null',
                    'callable\s*\.\s*Parameters\s*\.\s*Count\s*!=\s*1',
                    'callable\s*\.\s*Parameters\s*\[\s*0\s*\]\s*\.\s*RefKind',
                    'callable\s*\.\s*Parameters\s*\[\s*0\s*\]\s*\.\s*TypeId',
                    'callable\s*\.\s*ReturnTypeId',
                    'return\s+EmitCall\s*\('
                ) `
                $Violations `
                'validated SemanticConversion.IsUserDefined to Guest EmitCall'
        }
    }

    Test-TypedCastDispatch $Invocation $ObjectRegistryHeader $ObjectRegistry $Violations
}

function Invoke-CheckerFixtures {
    $FixtureFailures = New-ViolationList

    $PositiveNative = @'
NativeSymbol GNativeSymbols [] =
{
    // Equivalent formatting must not change the parsed record.
    {
        "alpha",
        reinterpret_cast < void * > ( Alpha ),
        "(i)i",
        nullptr
    }
};
'@
    $PositiveCommentFailures = New-ViolationList
    $PositiveCode = Remove-SourceComments $PositiveNative $PositiveCommentFailures 'positive native fixture'
    Test-NativeSymbolArray `
        $PositiveCode `
        'GNativeSymbols' `
        @('alpha|Alpha|(i)i|nullptr') `
        $PositiveCommentFailures
    if ($PositiveCommentFailures.Count -ne 0) {
        Add-Violation $FixtureFailures "equivalent formatting fixture failed: $($PositiveCommentFailures -join '; ')"
    }
    else {
        Write-Host 'Fixture passed: equivalent native formatting is accepted.'
    }

    $BespokeNative = @'
NativeSymbol GNativeSymbols[] = {
    { "alpha", reinterpret_cast<void*>(Alpha), "(i)i", nullptr },
    { "avid_enemy_spawn", reinterpret_cast<void*>(EnemySpawn), "(i)i", nullptr }
};
'@
    $BespokeFailures = New-ViolationList
    $BespokeCode = Remove-SourceComments $BespokeNative $BespokeFailures 'bespoke native fixture'
    Test-NativeSymbolArray `
        $BespokeCode `
        'GNativeSymbols' `
        @('alpha|Alpha|(i)i|nullptr') `
        $BespokeFailures
    if ($BespokeFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'bespoke native regression fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: a real bespoke native import regression is rejected.'
    }

    $IncompleteNative = @'
NativeSymbol GNativeSymbols[] = {
    { "alpha", reinterpret_cast<void*>(Alpha), "(i)i", nullptr },
    AVID_NATIVE("avid_enemy_spawn", EnemySpawn, "(i)i")
};
'@
    $IncompleteFailures = New-ViolationList
    $IncompleteCode = Remove-SourceComments $IncompleteNative $IncompleteFailures 'incomplete native fixture'
    Test-NativeSymbolArray `
        $IncompleteCode `
        'GNativeSymbols' `
        @('alpha|Alpha|(i)i|nullptr') `
        $IncompleteFailures
    if ($IncompleteFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'unparsed native candidate fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: an unparsed initializer candidate is rejected.'
    }

    $CommentSequenceFailures = New-ViolationList
    $CommentCode = Remove-SourceComments `
        'alpha(); /* beta(); */ gamma();' `
        $CommentSequenceFailures `
        'comment sequence fixture'
    Test-RegexSequence `
        $CommentCode `
        @('alpha\s*\(', 'beta\s*\(') `
        $CommentSequenceFailures `
        'comment-stripped sequence'
    if ($CommentSequenceFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'comment-only token incorrectly satisfied a sequence'
    }
    else {
        Write-Host 'Fixture passed: comment-only tokens cannot satisfy a sequence.'
    }

    $TypedPositiveFailures = New-ViolationList
    $TypedPositive = @'
SetAvidScriptBindingDispatchFailure ( OutResult );
Context.ObjectRegistry
    ->ResolveObject(Handle, ResolveResult, false);
Package.TryResolveObjectType(Ordinal, CachedClass);
Object->IsA(CachedClass);
// LoadObject must not count from comments.
'@
    $TypedPositiveCode = Remove-SourceComments $TypedPositive $TypedPositiveFailures 'typed formatting fixture'
    Test-DirectCallAllowlist `
        $TypedPositiveCode `
        @(
            'SetAvidScriptBindingDispatchFailure',
            'Context.ObjectRegistry->ResolveObject',
            'Package.TryResolveObjectType',
            'Object->IsA'
        ) `
        $TypedPositiveFailures `
        'typed formatting fixture'
    if ((Remove-SourceStrings $TypedPositiveCode) -match '\b(?:FindObject|LoadObject|StaticLoadObject|GetPathName)\s*(?:<[^>]+>)?\s*\(') {
        Add-Violation $TypedPositiveFailures 'comment stripping left a forbidden lookup'
    }
    if ($TypedPositiveFailures.Count -ne 0) {
        Add-Violation $FixtureFailures "typed equivalent formatting fixture failed: $($TypedPositiveFailures -join '; ')"
    }
    else {
        Write-Host 'Fixture passed: equivalent typed-dispatch formatting is accepted.'
    }

    $TypedRegressionFailures = New-ViolationList
    $TypedRegression = 'ResolveTypeByPath(); LoadObject<UClass>(nullptr, TEXT("/Game/Enemy"));'
    Test-DirectCallAllowlist `
        $TypedRegression `
        @() `
        $TypedRegressionFailures `
        'typed helper regression fixture'
    if ($TypedRegression -match '\b(?:FindObject|LoadObject|StaticLoadObject|GetPathName)\s*(?:<[^>]+>)?\s*\(') {
        Add-Violation $TypedRegressionFailures 'typed helper regression contains forbidden path lookup'
    }
    if ($TypedRegressionFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'typed helper/path lookup regression fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: an unreviewed helper and path lookup regression is rejected.'
    }

    $SplitSurface = @'
OutSource += TEXT("public readonly ");
OutSource += TEXT("struct ProjectEnemy");
OutSource += TEXT("\n");
OutSource += TEXT("avid_");
OutSource += TEXT("project_enemy_call");
'@
    $SplitSurfaceFailures = New-ViolationList
    Test-GeneratedLiteralStream `
        $SplitSurface `
        @('avid_owner_get_handle', 'avid_object_type_is_a') `
        @('FVector') `
        $SplitSurfaceFailures `
        'split generated surface fixture'
    if ($SplitSurfaceFailures.Count -lt 2) {
        Add-Violation $FixtureFailures 'split wrapper/import regression fixture was not fully rejected'
    }
    else {
        Write-Host 'Fixture passed: split bespoke wrapper and import literals are rejected.'
    }

    $StringCallFailures = New-ViolationList
    $StringCallFixture = 'FString Diagnostic = TEXT("LoadObject( ResolveTypeByPath("); SafeLeaf();'
    Test-DirectCallAllowlist `
        $StringCallFixture `
        @('SafeLeaf') `
        $StringCallFailures `
        'string call fixture'
    if ($StringCallFailures.Count -ne 0) {
        Add-Violation $FixtureFailures "string literal call fixture produced false positives: $($StringCallFailures -join '; ')"
    }
    else {
        Write-Host 'Fixture passed: call-like text inside strings is ignored.'
    }

    $HelperLeafFailures = New-ViolationList
    Test-DirectCallAllowlist `
        'FString::Printf(TEXT("failure")); ResolveTypeByPath();' `
        @('FString::Printf') `
        $HelperLeafFailures `
        'reviewed helper leaf fixture'
    if ($HelperLeafFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'reviewed helper leaf regression fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: reviewed helper leaves reject new transitive calls.'
    }

    $FrozenRendererBase = 'void Render() { Out += TEXT("public readonly struct %s"); }'
    $FrozenRendererFormatting = @'
void Render()
{
    Out += TEXT("public readonly struct %s");
}
'@
    $FrozenRendererMutation = @'
void Render()
{
    const FString Name = TEXT("ProjectEnemy");
    const FString Declaration = TEXT("public readonly struct ");
    Out += Declaration + Name;
}
'@
    $BaseHash = Get-CanonicalCodeSha256 $FrozenRendererBase
    if ($BaseHash -cne (Get-CanonicalCodeSha256 $FrozenRendererFormatting) -or
        $BaseHash -ceq (Get-CanonicalCodeSha256 $FrozenRendererMutation)) {
        Add-Violation $FixtureFailures 'canonical renderer hash fixture did not ignore formatting while rejecting variable reorder'
    }
    else {
        Write-Host 'Fixture passed: canonical renderer hashes ignore formatting and reject reordered variable assembly.'
    }

    $OperatorBase = 'void Render() { ++ParameterIndex; }'
    $OperatorFormatting = @'
void Render()
{
    ++ ParameterIndex;
}
'@
    $OperatorSplit = 'void Render() { + +ParameterIndex; }'
    $OperatorHash = Get-CanonicalCodeSha256 $OperatorBase
    if ($OperatorHash -cne (Get-CanonicalCodeSha256 $OperatorFormatting) -or
        $OperatorHash -ceq (Get-CanonicalCodeSha256 $OperatorSplit)) {
        Add-Violation $FixtureFailures 'canonical C++ token hash did not preserve the ++ operator boundary'
    }
    else {
        Write-Host 'Fixture passed: canonical C++ token hashes preserve multi-character operator boundaries.'
    }

    $TransitiveBase = @'
FString ConvertToStorage(FString Value) { return Value + TEXT(".Slot"); }
void Render() { Out += ConvertToStorage(Value); }
'@
    $TransitiveFormatting = @'
FString ConvertToStorage(FString Value)
{
    return Value + TEXT(".Slot");
}

void Render()
{
    Out += ConvertToStorage(Value);
}
'@
    $TransitiveMutation = @'
FString ConvertToStorage(FString Value) { return Value + TEXT(".Generation"); }
void Render() { Out += ConvertToStorage(Value); }
'@
    $TransitiveHash = Get-CanonicalCodeSha256 $TransitiveBase
    if ($TransitiveHash -cne (Get-CanonicalCodeSha256 $TransitiveFormatting) -or
        $TransitiveHash -ceq (Get-CanonicalCodeSha256 $TransitiveMutation)) {
        Add-Violation $FixtureFailures 'generated surface closure hash did not cover a transitive helper mutation'
    }
    else {
        Write-Host 'Fixture passed: generated surface closure hashes include transitive helper behavior.'
    }

    $GuardedLeafFailures = New-ViolationList
    $GuardedLeafRegression = @'
if (bIncludeObjectPath && Object != nullptr)
{
    OutResult.ObjectPath = Object->GetPathName();
}
Audit = Object->GetPathName();
'@
    Test-SingleCallInsideGuard `
        $GuardedLeafRegression `
        '\bif\s*\(\s*bIncludeObjectPath\s*&&\s*Object\s*!=\s*nullptr\s*\)' `
        'GetPathName' `
        $GuardedLeafFailures `
        'guarded leaf regression fixture'
    if ($GuardedLeafFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'extra unguarded allowlisted call fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: an extra unguarded allowlisted path call is rejected.'
    }

    $RegistryMacroFailures = New-ViolationList
    $RegistryMacroRegression = @'
#include "Registry.h"
#define AVID_PATH (Object->GetPathName())
void SetSuccess()
{
    if (bIncludeObjectPath && Object != nullptr)
    {
        OutResult.ObjectPath = Object->GetPathName();
    }
}
void SetFailure()
{
    if (bIncludeObjectPath && Object != nullptr)
    {
        OutResult.ObjectPath = Object->GetPathName();
    }
    Audit = AVID_PATH;
}
'@
    Test-RegistryPathSurface `
        $RegistryMacroRegression `
        $RegistryMacroFailures `
        'registry macro regression fixture'
    if ($RegistryMacroFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'object-like macro path regression fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: registry path macros and extra path identifiers are rejected.'
    }

    $RegistryPhase2Failures = New-ViolationList
    $RegistryPhase2Regression = @'
#include "Registry.h"
void SetSuccess()
{
    if (bIncludeObjectPath && Object != nullptr)
    {
        OutResult.ObjectPath = Object->GetPathName();
    }
}
void SetFailure()
{
    if (bIncludeObjectPath && Object != nullptr)
    {
        OutResult.ObjectPath = Object->GetPathName();
    }
    Audit = Object->GetPath\
Name();
}
'@
    Test-RegistryPathSurface `
        $RegistryPhase2Regression `
        $RegistryPhase2Failures `
        'registry phase-2 regression fixture'
    if ($RegistryPhase2Failures.Count -eq 0) {
        Add-Violation $FixtureFailures 'phase-2 line-spliced path regression fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: C++ phase-2 line-spliced path identifiers are rejected.'
    }

    $AlternativePreprocessorFailures = New-ViolationList
    $AlternativePreprocessorRegression = @'
%:define AVID_PATH Object->GetPathName()
Object->GetPathName();
'@
    Test-RegistryPathSurface `
        $AlternativePreprocessorRegression `
        $AlternativePreprocessorFailures `
        'alternative preprocessor regression fixture'
    if ($AlternativePreprocessorFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'alternative preprocessor token regression fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: alternative preprocessor tokens are rejected.'
    }

    $ForwardingFailures = New-ViolationList
    $ForwardingRegression = @'
SetFail\
ure(OutResult, Handle, TEXT("invalid"), Object, true, TEXT("next"));
SetSuccess(OutResult, Handle, Object, bIncludeObjectPath);
'@
    Test-DiagnosticForwarding `
        $ForwardingRegression `
        'ReleaseHandle fixture' `
        $ForwardingFailures
    if ($ForwardingFailures.Count -eq 0) {
        Add-Violation $FixtureFailures 'phase-2 public registry entry forwarding regression fixture was not rejected'
    }
    else {
        Write-Host 'Fixture passed: phase-2 calls in every public registry entry must forward bIncludeObjectPath.'
    }

    if ($FixtureFailures.Count -gt 0) {
        Write-Host "Phase 50 architecture checker fixtures failed with $($FixtureFailures.Count) violation(s):"
        foreach ($Failure in $FixtureFailures) {
            Write-Host " - $Failure"
        }
        return $false
    }
    Write-Host 'Phase 50 architecture checker fixtures passed.'
    return $true
}

if ($Mode -eq 'Fixtures') {
    if (Invoke-CheckerFixtures) {
        exit 0
    }
    exit 1
}

if ($Mode -eq 'Hashes') {
    $HashViolations = New-ViolationList
    $RendererPath = Join-Path $PluginRoot 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpBindingRenderer.cpp'
    $RendererSource = [System.IO.File]::ReadAllText($RendererPath)
    $RendererCode = Remove-SourceComments (ConvertFrom-CppPhase2LineSplicing $RendererSource) $HashViolations 'C# renderer hash input'
    $StateContractRendererPath = Join-Path $PluginRoot 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpStateContractRenderer.cpp'
    $StateContractRendererSource = [System.IO.File]::ReadAllText($StateContractRendererPath)
    $StateContractRendererCode = Remove-SourceComments (ConvertFrom-CppPhase2LineSplicing $StateContractRendererSource) $HashViolations 'C# state contract renderer hash input'
    $DefaultValueFormatterPath = Join-Path $PluginRoot 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpDefaultValueFormatter.cpp'
    $DefaultValueFormatterSource = [System.IO.File]::ReadAllText($DefaultValueFormatterPath)
    $DefaultValueFormatterCode = Remove-SourceComments (ConvertFrom-CppPhase2LineSplicing $DefaultValueFormatterSource) $HashViolations 'C# default value formatter hash input'
    $SyntaxPath = Join-Path $PluginRoot 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpSyntax.cpp'
    $SyntaxSource = [System.IO.File]::ReadAllText($SyntaxPath)
    $SyntaxCode = Remove-SourceComments (ConvertFrom-CppPhase2LineSplicing $SyntaxSource) $HashViolations 'C# syntax hash input'
    $LifecycleBindingPath = Join-Path $PluginRoot 'Source/AvidScriptBindings/Private/AvidScriptObjectLifecycleBinding.cpp'
    $LifecycleBindingSource = [System.IO.File]::ReadAllText($LifecycleBindingPath)
    $LifecycleBindingCode = Remove-SourceComments (ConvertFrom-CppPhase2LineSplicing $LifecycleBindingSource) $HashViolations 'object lifecycle binding hash input'
    $RegistryPath = Join-Path $PluginRoot 'Source/AvidScriptBindings/Private/AvidScriptObjectRegistry.cpp'
    $RegistrySource = [System.IO.File]::ReadAllText($RegistryPath)
    $RegistryCode = Remove-SourceComments (ConvertFrom-CppPhase2LineSplicing $RegistrySource) $HashViolations 'object registry diagnostic hash input'
    $RegistryHeaderPath = Join-Path $PluginRoot 'Source/AvidScriptBindings/Public/AvidScriptObjectRegistry.h'
    $RegistryHeaderSource = [System.IO.File]::ReadAllText($RegistryHeaderPath)
    $RegistryHeaderCode = Remove-SourceComments (ConvertFrom-CppPhase2LineSplicing $RegistryHeaderSource) $HashViolations 'object registry header hash input'
    foreach ($Entry in (Get-RendererFrozenRegions).GetEnumerator()) {
        $Region = Get-UniqueBraceRegion $RendererCode $Entry.Value $HashViolations "renderer hash region $($Entry.Key)"
        if ($null -ne $Region) {
            Write-Host "$($Entry.Key)=$(Get-CanonicalCodeSha256 $Region.Text)"
        }
    }
    Write-Host "BindingRenderer=$(Get-CanonicalCodeSha256 $RendererCode)"
    Write-Host "StateContractRenderer=$(Get-CanonicalCodeSha256 $StateContractRendererCode)"
    Write-Host "DefaultValueFormatter=$(Get-CanonicalCodeSha256 $DefaultValueFormatterCode)"
    Write-Host "CSharpSyntax=$(Get-CanonicalCodeSha256 $SyntaxCode)"
    Write-Host "LifecycleBinding=$(Get-CanonicalCodeSha256 $LifecycleBindingCode)"
    Write-Host "ObjectRegistryHeader=$(Get-CanonicalCodeSha256 $RegistryHeaderCode)"
    Write-Host "ObjectRegistrySource=$(Get-CanonicalCodeSha256 $RegistryCode)"
    foreach ($DiagnosticLeafSpec in ([ordered]@{
        'SetSuccess' = '\bvoid\s+FAvidScriptObjectRegistry::SetSuccess\s*\('
        'SetFailure' = '\bvoid\s+FAvidScriptObjectRegistry::SetFailure\s*\('
    }).GetEnumerator()) {
        $DiagnosticLeaf = Get-UniqueBraceRegion `
            $RegistryCode `
            $DiagnosticLeafSpec.Value `
            $HashViolations `
            "registry diagnostic hash region $($DiagnosticLeafSpec.Key)"
        if ($null -ne $DiagnosticLeaf) {
            Write-Host "$($DiagnosticLeafSpec.Key)=$(Get-CanonicalCodeSha256 $DiagnosticLeaf.Text)"
        }
    }
    if ($HashViolations.Count -gt 0) {
        foreach ($Violation in $HashViolations) {
            Write-Host " - $Violation"
        }
        exit 1
    }
    exit 0
}

$PluginRoot = [System.IO.Path]::GetFullPath($PluginRoot)
$Violations = New-ViolationList
$InputPaths = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
$Inputs = @{}

function Read-ArchitectureInput {
    param(
        [string]$RelativePath,
        [ValidateSet('Source', 'Json', 'PowerShell')]
        [string]$Kind = 'Source'
    )

    $NormalizedPath = $RelativePath.Replace('\', '/')
    [void]$InputPaths.Add($NormalizedPath)
    $FullPath = [System.IO.Path]::GetFullPath((Join-Path $PluginRoot $RelativePath))
    $RootPrefix = $PluginRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $FullPath.StartsWith($RootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Add-Violation $Violations "architecture input escapes PluginRoot: $RelativePath"
        return [pscustomobject]@{ Raw = ''; Code = ''; Bytes = [byte[]]@() }
    }
    if (-not [System.IO.File]::Exists($FullPath)) {
        Add-Violation $Violations "missing architecture input: $NormalizedPath"
        return [pscustomobject]@{ Raw = ''; Code = ''; Bytes = [byte[]]@() }
    }

    $Bytes = [System.IO.File]::ReadAllBytes($FullPath)
    $Raw = [System.Text.Encoding]::UTF8.GetString($Bytes)
    if ($Raw.Length -gt 0 -and $Raw[0] -eq [char]0xFEFF) {
        $Raw = $Raw.Substring(1)
    }
    $Code = if ($Kind -eq 'Source') {
        $CommentInput = if ([System.IO.Path]::GetExtension($NormalizedPath) -ceq '.cs') {
            $Raw
        }
        else {
            ConvertFrom-CppPhase2LineSplicing $Raw
        }
        Remove-SourceComments $CommentInput $Violations $NormalizedPath
    }
    else {
        $Raw
    }
    return [pscustomobject]@{
        Raw = $Raw
        Code = $Code
        Bytes = $Bytes
    }
}

$InputManifest = [ordered]@{
    'Build/TestPhase50Architecture.ps1' = 'PowerShell'
    'AvidScript.uplugin' = 'Json'
    'Source/AvidScriptBindings/Public/AvidScriptBindingDescriptor.h' = 'Source'
    'Source/AvidScriptBindings/Public/AvidScriptObjectRegistry.h' = 'Source'
    'Source/AvidScriptBindings/Private/AvidScriptBindingDescriptor.cpp' = 'Source'
    'Source/AvidScriptBindings/Private/AvidScriptObjectLifecycleBinding.cpp' = 'Source'
    'Source/AvidScriptBindings/Private/AvidScriptObjectTypeBinding.cpp' = 'Source'
    'Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp' = 'Source'
    'Source/AvidScriptBindings/Private/AvidScriptObjectRegistry.cpp' = 'Source'
    'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDescriptorGenerator.cpp' = 'Source'
    'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpBindingEmitter.cpp' = 'Source'
    'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpDefaultValueFormatter.cpp' = 'Source'
    'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpBindingRenderer.cpp' = 'Source'
    'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpStateContractRenderer.cpp' = 'Source'
    'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpSyntax.cpp' = 'Source'
    'Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp' = 'Source'
    'Source/AvidScriptVM/Private/AvidScriptWamrDynamicRegistry.cpp' = 'Source'
    'Source/AvidScriptVM/Private/AvidScriptWamrHostBindings.cpp' = 'Source'
    'Tools/AvidScript.CSharpGuest/Lowering/CSharpOperationLowerer.cs' = 'Source'
}
foreach ($Entry in $InputManifest.GetEnumerator()) {
    $Inputs[$Entry.Key] = Read-ArchitectureInput $Entry.Key $Entry.Value
}

try {
    $PluginDescriptor = $Inputs['AvidScript.uplugin'].Raw | ConvertFrom-Json
    $ModuleNames = @($PluginDescriptor.Modules | ForEach-Object { $_.Name })
    foreach ($RequiredModule in @(
        'AvidScriptCore',
        'AvidScriptBindings',
        'AvidScriptVM',
        'AvidScriptRuntime',
        'AvidScriptEditor'
    )) {
        if ($ModuleNames -cnotcontains $RequiredModule) {
            Add-Violation $Violations "plugin descriptor is missing module $RequiredModule"
        }
    }
}
catch {
    Add-Violation $Violations "AvidScript.uplugin is not valid JSON: $($_.Exception.Message)"
}

Invoke-Phase50Contracts $Inputs $Violations

$EvidenceCommitOutput = @(& git -C $PluginRoot rev-parse HEAD 2>&1)
if ($LASTEXITCODE -ne 0) {
    Add-Violation $Violations 'Phase 50 architecture evidence requires a readable Git HEAD'
    $EvidenceCommit = 'unavailable'
}
else {
    $EvidenceCommit = ($EvidenceCommitOutput -join '').Trim()
}
$EvidenceTreeOutput = @(& git -C $PluginRoot rev-parse 'HEAD^{tree}' 2>&1)
if ($LASTEXITCODE -ne 0) {
    Add-Violation $Violations 'Phase 50 architecture evidence requires a readable Git tree'
    $EvidenceTree = 'unavailable'
}
else {
    $EvidenceTree = ($EvidenceTreeOutput -join '').Trim()
}

$TrackedInputsOutput = @(& git -C $PluginRoot ls-files -- 2>&1)
if ($LASTEXITCODE -ne 0) {
    Add-Violation $Violations 'Phase 50 architecture evidence could not enumerate tracked files'
    $TrackedInputsOutput = @()
}
$TrackedInputs = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($TrackedPath in $TrackedInputsOutput) {
    [void]$TrackedInputs.Add(([string]$TrackedPath).Trim().Replace('\', '/'))
}
foreach ($InputPath in $InputPaths) {
    if (-not $TrackedInputs.Contains($InputPath)) {
        Add-Violation $Violations "architecture input is not tracked by Git: $InputPath"
    }
}

$StatusOutput = @(& git -C $PluginRoot status --porcelain=v1 --untracked-files=all 2>&1)
if ($LASTEXITCODE -ne 0) {
    Add-Violation $Violations 'Phase 50 architecture evidence could not inspect worktree cleanliness'
    $StatusOutput = @()
}
$DirtyPaths = @($StatusOutput | ForEach-Object { ([string]$_).TrimEnd() })
if ($DirtyPaths.Count -gt 0) {
    Add-Violation `
        $Violations `
        "Phase 50 architecture Gate requires a clean committed tree: $($DirtyPaths -join '; ')"
}

$Sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $CanonicalCheckerText = $Inputs['Build/TestPhase50Architecture.ps1'].Raw -replace "`r`n?", "`n"
    $CanonicalCheckerBytes = [System.Text.UTF8Encoding]::new($false).GetBytes($CanonicalCheckerText)
    $CheckerSha256 = [System.BitConverter]::ToString(
        $Sha256.ComputeHash($CanonicalCheckerBytes)
    ).Replace('-', '').ToLowerInvariant()
}
finally {
    $Sha256.Dispose()
}

Write-Host "Evidence commit: $EvidenceCommit"
Write-Host "Evidence tree: $EvidenceTree"
Write-Host "Evidence checker SHA-256: $CheckerSha256"
Write-Host "Evidence registered inputs: $($InputPaths.Count)"
$EvidenceInputState = if ($DirtyPaths.Count -eq 0) { 'clean' } else { 'dirty' }
Write-Host "Evidence committed tree: $EvidenceInputState"

if ($Violations.Count -gt 0) {
    Write-Host "Phase 50 architecture check failed with $($Violations.Count) violation(s):"
    foreach ($Violation in $Violations) {
        Write-Host " - $Violation"
    }
    exit 1
}

Write-Host 'Phase 50 architecture check passed.'
exit 0
