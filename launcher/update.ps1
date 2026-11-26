[CmdletBinding()]
param(
    [ValidateSet('Check', 'Stage', 'Apply', 'Discard')][string]$Action,
    [string]$Version,
    [string]$StageId,
    [string]$Digest,
    [int]$LauncherPid
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$releaseApi = 'https://api.github.com/repos/MatyMeatBoy/ShiningSource/releases/latest'
$releaseDownloads = 'https://github.com/MatyMeatBoy/ShiningSource/releases/download'
$packageLimit = 128MB

function Get-UpdateVersion([string]$Value) {
    if ($Value -cmatch '^v?(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})(?:\.(0|[1-9][0-9]{0,8}))?$') {
        return [version]::new([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
    }
}

function Get-InstalledVersion($Root) {
    $marker = Join-Path $Root 'ShiningSource-version.txt'
    if ([IO.File]::Exists($marker)) {
        $installed = Get-UpdateVersion ([IO.File]::ReadAllText($marker).Trim())
        if ($installed) { return $installed }
    }
    return [version]'0.9.6'
}

function Get-UpdateRelease {
    $headers = @{ Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    $release = Invoke-RestMethod -UseBasicParsing -Uri $releaseApi -UserAgent 'ShiningSource Windows Updater' -Headers $headers -TimeoutSec 15
    if ($release.draft -or $release.prerelease -or !(Get-UpdateVersion $release.tag_name)) {
        throw 'GitHub did not return a stable release.'
    }
    $number = $release.tag_name.TrimStart('v')
    $name = "ShiningSource-$number-windows-x64.zip"
    $assets = @($release.assets | Where-Object { $_.name -ceq $name })
    if ($assets.Count -ne 1) { throw 'This release has no Windows x64 package.' }
    $asset = $assets[0]
    $expectedUrl = "$releaseDownloads/$($release.tag_name)/$name"
    if ($asset.state -cne 'uploaded' -or $asset.browser_download_url -cne $expectedUrl -or
        $asset.digest -cnotmatch '^sha256:[0-9a-f]{64}$' -or $asset.size -le 0 -or $asset.size -gt $packageLimit) {
        throw 'GitHub did not provide verification data for the Windows package.'
    }
    return @{ Version = $number; Url = $expectedUrl; Digest = $asset.digest.Substring(7); Size = [long]$asset.size }
}

function Assert-OrdinaryPath($Path) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "The update path is a link: $Path"
    }
}

function Get-StagePath($Root, $Id) {
    if ($Id -cnotmatch '^[a-f0-9]{32}$') { throw 'Invalid staging identifier.' }
    $path = Join-Path $Root ".ss-update-$Id"
    Assert-OrdinaryPath $path
    return $path
}

function Receive-UpdatePackage($Release, $Output) {
    $request = [Net.HttpWebRequest]::CreateHttp($Release.Url)
    $request.UserAgent = 'ShiningSource Windows Updater'
    $request.AllowAutoRedirect = $false
    $request.Timeout = 15000
    $request.ReadWriteTimeout = 15000
    $response = $request.GetResponse()
    try {
        if ([int]$response.StatusCode -in @(301, 302, 303, 307, 308)) {
            $redirect = [Uri]$response.Headers['Location']
            if (!$redirect.IsAbsoluteUri -or $redirect.Scheme -cne 'https' -or !$redirect.IsDefaultPort -or
                $redirect.UserInfo -or $redirect.Host -cnotin @('release-assets.githubusercontent.com', 'objects.githubusercontent.com')) {
                throw 'GitHub returned an unexpected download URL.'
            }
            $response.Dispose()
            $request = [Net.HttpWebRequest]::CreateHttp($redirect)
            $request.UserAgent = 'ShiningSource Windows Updater'
            $request.AllowAutoRedirect = $false
            $request.Timeout = 15000
            $request.ReadWriteTimeout = 15000
            $response = $request.GetResponse()
        }
        if ([int]$response.StatusCode -ne 200) { throw 'GitHub did not return the Windows ZIP.' }
        $stream = $response.GetResponseStream()
        $file = [IO.File]::Open($Output, 'CreateNew', 'Write', 'None')
        try {
            $buffer = New-Object byte[] 65536
            $total = 0L
            $elapsed = [Diagnostics.Stopwatch]::StartNew()
            while (($count = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $total += $count
                if ($total -gt $Release.Size -or $elapsed.Elapsed.TotalMinutes -gt 5) {
                    throw 'The download exceeded its allowed size or time.'
                }
                $file.Write($buffer, 0, $count)
            }
            if ($total -ne $Release.Size) { throw 'The download is incomplete.' }
        } finally { $file.Dispose() }
    } finally { $response.Dispose() }
}

function Open-VerifiedPackage($StagePath, $ExpectedDigest) {
    if ($ExpectedDigest -notmatch '^[a-f0-9]{64}$') { throw 'Invalid SHA-256 digest.' }
    $path = Join-Path $StagePath 'release.zip'
    Assert-OrdinaryPath $path
    $stream = [IO.File]::Open($path, 'Open', 'Read', 'Read')
    try {
        if ($stream.Length -le 0 -or $stream.Length -gt $packageLimit) { throw 'Invalid package size.' }
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
        finally { $sha.Dispose() }
        if ($actual -ne $ExpectedDigest) { throw 'The Windows package failed SHA-256 verification.' }
        $stream.Position = 0
        return [IO.Compression.ZipArchive]::new($stream, 'Read')
    } catch {
        $stream.Dispose()
        throw
    }
}

function Expand-UpdatePackage($Archive, $Destination, $Number) {
    $prefix = "ShiningSource-$Number/"
    $required = @('ShiningSource.exe', 'ss2_win64.exe', 'ss2_game.elf', 'SDL3.dll')
    $allowed = '^(ShiningSource\.exe|ss2_win64\.exe|ss2_game\.elf|SDL3\.dll|libusb-1\.0\.dll|ss_relay\.exe|ShiningSource-update\.ps1|assets/launcher/(cover|(title|title-pro|title-native|menu|shop|shop-native|brand|language|language-native|save|save-native|forest|forest-native|inventory|merchant|inspect)-sides)\.png|ra_set_[a-f0-9]{32}\.json|achievements/badges/badge_[0-9]+\.png)$'
    $seen = @{}
    $files = [Collections.Generic.List[string]]::new()
    $total = 0L
    foreach ($entry in $Archive.Entries) {
        if ($entry.FullName -ceq $prefix -and $entry.Length -eq 0) { continue }
        if (!$entry.FullName.StartsWith($prefix, [StringComparison]::Ordinal)) { throw 'Unexpected ZIP layout.' }
        $name = $entry.FullName.Substring($prefix.Length)
        if ($name -cnotmatch '^(?:[A-Za-z0-9_-][A-Za-z0-9_.-]*/)*[A-Za-z0-9_-][A-Za-z0-9_.-]*$' -or
            $name -match '(?:\.| )(/|$)' -or $seen.ContainsKey($name)) { throw "Invalid or duplicate ZIP path: $name" }
        $seen[$name] = $true
        if ($name -cnotmatch $allowed) { continue }
        if (($entry.ExternalAttributes -band 0xF0000000) -eq 0xA0000000 -or
            ($entry.ExternalAttributes -band 0x400)) { throw "Unexpected linked ZIP entry: $name" }
        $total += $entry.Length
        if ($entry.Length -le 0 -or $total -gt 256MB) { throw 'Invalid extracted package size.' }
        if ($Destination) {
            $file = Join-Path $Destination $name
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $file, $false)
        }
        $files.Add($name)
    }
    foreach ($name in $required) {
        if (!$files.Contains($name)) { throw "The ZIP is missing $name." }
    }
    return $files
}

function Install-Update($Root, $StagePath, $Number, $ExpectedDigest) {
    $incoming = Get-UpdateVersion $Number
    if (!$incoming) { throw 'Invalid update version.' }
    Assert-OrdinaryPath $Root
    Assert-OrdinaryPath $StagePath
    $lockPath = Join-Path $Root '.ss-update.lock'
    Assert-OrdinaryPath $lockPath
    $lock = [IO.File]::Open($lockPath, 'OpenOrCreate', 'Write', 'None')
    $transaction = Join-Path $StagePath ('install-' + [Guid]::NewGuid().ToString('N'))
    $swaps = [Collections.Generic.List[object]]::new()
    $recoverable = $true
    try {
        if ($incoming -le (Get-InstalledVersion $Root)) { throw 'The installed version is current.' }
        $payload = Join-Path $transaction 'payload'
        $backups = Join-Path $transaction 'backups'
        [void][IO.Directory]::CreateDirectory($payload)
        $archive = Open-VerifiedPackage $StagePath $ExpectedDigest
        try { $files = @(Expand-UpdatePackage $archive $payload $Number) }
        finally { $archive.Dispose() }
        $versionFile = 'ShiningSource-version.txt'
        [IO.File]::WriteAllText((Join-Path $payload $versionFile), "$Number`r`n")
        # Swap the launcher and version marker after the other files.
        $files = @($files | Where-Object { $_ -cne 'ShiningSource.exe' }) + @('ShiningSource.exe', $versionFile)
        foreach ($name in $files) {
            $target = $Root
            foreach ($part in $name.Split('/')) {
                $target = Join-Path $target $part
                Assert-OrdinaryPath $target
            }
            if ($name -match '^(ra_set_|achievements/)' -and [IO.File]::Exists($target)) { continue }
            if ([IO.Directory]::Exists($target)) { throw "A directory occupies $target." }
            $replacement = Join-Path $payload $name
            $backup = Join-Path $backups $name
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            if ([IO.File]::Exists($target)) {
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($backup))
                $swaps.Add(@{ Target = $target; Backup = $backup })
                [IO.File]::Replace($replacement, $target, $backup)
            } else {
                [IO.File]::Move($replacement, $target)
                $swaps.Add(@{ Target = $target; Backup = $null })
            }
        }
    } catch {
        $failure = $_
        for ($i = $swaps.Count - 1; $i -ge 0; $i--) {
            $swap = $swaps[$i]
            try {
                if (!$swap.Backup) {
                    [IO.File]::Delete($swap.Target)
                } elseif ([IO.File]::Exists($swap.Backup)) {
                    if ([IO.File]::Exists($swap.Target)) {
                        [IO.File]::Replace($swap.Backup, $swap.Target, [NullString]::Value)
                    } else {
                        [IO.File]::Move($swap.Backup, $swap.Target)
                    }
                }
            } catch { $recoverable = $false }
        }
        if (!$recoverable) { throw "Update failed. Recovery files are in $transaction. $failure" }
        throw $failure
    } finally {
        $lock.Dispose()
        if ($recoverable -and [IO.Directory]::Exists($transaction)) {
            try { [IO.Directory]::Delete($transaction, $true) } catch { }
        }
    }
}

if ($MyInvocation.InvocationName -eq '.') { return }
$root = $PSScriptRoot
$exitCode = 1
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    switch ($Action) {
        'Check' {
            $release = Get-UpdateRelease
            if ((Get-UpdateVersion $release.Version) -le (Get-InstalledVersion $root)) { exit 10 }
            [Console]::WriteLine($release.Version)
        }
        'Stage' {
            $path = Get-StagePath $root $StageId
            $release = Get-UpdateRelease
            if ($release.Version -cne $Version -or (Get-UpdateVersion $Version) -le (Get-InstalledVersion $root)) {
                throw 'The release changed. Check for updates again.'
            }
            if ([IO.Directory]::Exists($path)) { throw 'The staging directory already exists.' }
            Assert-OrdinaryPath $root
            [void][IO.Directory]::CreateDirectory($path)
            try {
                Receive-UpdatePackage $release (Join-Path $path 'release.zip')
                $archive = Open-VerifiedPackage $path $release.Digest
                try { [void](Expand-UpdatePackage $archive $null $Version) }
                finally { $archive.Dispose() }
                [Console]::WriteLine($release.Digest)
            } catch {
                try { [IO.Directory]::Delete($path, $true) } catch { }
                throw
            }
        }
        'Apply' {
            $launcher = Get-Process -Id $LauncherPid -ErrorAction SilentlyContinue
            if ($launcher -and $launcher.Name -eq 'ShiningSource' -and !$launcher.WaitForExit(30000)) {
                throw 'ShiningSource could not close for the update.'
            }
            $active = @(Get-Process -Name 'ShiningSource', 'ss2_win64' -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -and [IO.Path]::GetDirectoryName($_.Path) -eq $root })
            if ($active.Count) { throw 'Close the game and other launchers from this installation before updating.' }
            $path = Get-StagePath $root $StageId
            Install-Update $root $path $Version $Digest
            try { [IO.Directory]::Delete($path, $true) } catch { }
            Start-Process -FilePath (Join-Path $root 'ShiningSource.exe') -WorkingDirectory $root
        }
        'Discard' {
            $path = Get-StagePath $root $StageId
            if ([IO.Directory]::Exists($path)) { [IO.Directory]::Delete($path, $true) }
        }
        default { throw 'Missing updater action.' }
    }
    $exitCode = 0
} catch {
    if ($Action -eq 'Apply') {
        Add-Type -AssemblyName System.Windows.Forms
        [void][Windows.Forms.MessageBox]::Show($_.Exception.Message, 'ShiningSource', 'OK', 'Error')
    } else {
        [Console]::WriteLine('Update failed.')
    }
}
exit $exitCode
