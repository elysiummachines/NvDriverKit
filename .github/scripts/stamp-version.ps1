<#
.SYNOPSIS
    Writes the tag's version into res/app.rc before a release build.

.DESCRIPTION
    res/app.rc carries 1.0.0.0 so a local build.bat always works without
    arguments. A tagged build overwrites it from the tag, so the exe's
    Properties > Details pane matches the release it shipped in.

    Each pattern must match exactly once. That catches a drifted or
    duplicated VERSIONINFO line, rather than shipping a binary stamped
    with the wrong version. It deliberately does not care whether the
    text changed: stamping v1.0.0 onto a file that already reads 1.0.0.0
    is a legitimate no-op.
#>
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$Rc = 'res\app.rc'
)

$ErrorActionPreference = 'Stop'

$version = $Tag -replace '^v', ''
if ($version -notmatch '^\d+(\.\d+)*$') {
    throw "Tag '$Tag' is not a version like v1.2.3"
}

# Pad to the four parts VERSIONINFO wants: v1.2 -> 1,2,0,0
$parts  = @($version -split '\.') + @('0', '0', '0', '0')
$quad   = ($parts[0..3]) -join ','
$dotted = ($parts[0..3]) -join '.'

# No end anchor on the first two: the file is CRLF, and a trailing "$" will
# not match because \r is left over after the digit class.
$edits = @(
    @{ Name = 'FILEVERSION'
       Pattern = '(?m)^FILEVERSION[ \t]+[\d, \t]+'
       Replace = "FILEVERSION    $quad" }
    @{ Name = 'PRODUCTVERSION'
       Pattern = '(?m)^PRODUCTVERSION[ \t]+[\d, \t]+'
       Replace = "PRODUCTVERSION $quad" }
    @{ Name = 'VALUE "FileVersion"'
       Pattern = '(VALUE[ \t]+"FileVersion",[ \t]*)"[^"]*"'
       Replace = '$1"' + $dotted + '"' }
    @{ Name = 'VALUE "ProductVersion"'
       Pattern = '(VALUE[ \t]+"ProductVersion",[ \t]*)"[^"]*"'
       Replace = '$1"' + $dotted + '"' }
)

$text = Get-Content $Rc -Raw
foreach ($edit in $edits) {
    $hits = [regex]::Matches($text, $edit.Pattern).Count
    if ($hits -ne 1) {
        throw "Expected exactly one $($edit.Name) line in $Rc, found $hits - the VERSIONINFO patterns have drifted"
    }
    $text = [regex]::Replace($text, $edit.Pattern, $edit.Replace)
}

Set-Content -Path $Rc -Value $text -NoNewline
Write-Host "Stamped $Rc with $dotted"
Select-String -Path $Rc -Pattern 'FILEVERSION|PRODUCTVERSION|"FileVersion"|"ProductVersion"' |
    ForEach-Object { Write-Host "  $($_.Line.Trim())" }
