<#
.SYNOPSIS
    Prints the CHANGELOG.md section for one version.

.DESCRIPTION
    The release workflow uses this to build the release body, so the notes on
    a release and the entry in the changelog are the same text by
    construction rather than by someone remembering to paste it.

    Throws when the tag has no section. That fails the release rather than
    publishing one with empty notes or, worse, another version's.
#>
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$Path = 'CHANGELOG.md'
)

$ErrorActionPreference = 'Stop'

$version = $Tag -replace '^v', ''
$text    = Get-Content $Path -Raw

# From this version's "## [x.y.z]" heading up to the next "## " heading, or
# the end of the file for the newest entry.
$pattern = '(?ms)^##[ \t]*\[' + [regex]::Escape($version) + '\][^\r\n]*\r?\n(.*?)(?=^##[ \t]|\z)'
$match   = [regex]::Match($text, $pattern)

if (-not $match.Success) {
    throw "No section for $version in $Path. Add a '## [$version] - <date>' heading before tagging."
}

$body = $match.Groups[1].Value.Trim()
if (-not $body) { throw "The section for $version in $Path is empty." }

$body
