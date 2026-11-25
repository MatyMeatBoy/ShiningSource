$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../launcher/update.ps1')

function Assert-True($Condition, $Message) {
    if (!$Condition) { throw $Message }
}

Assert-True ((Get-UpdateVersion 'v0.10.0').CompareTo((Get-UpdateVersion '0.9.3')) -gt 0) 'Version comparison must be numeric'
Assert-True ($null -eq (Get-UpdateVersion 'v1.0.0-rc1')) 'Prerelease tags must be rejected'

$root = Join-Path ([IO.Path]::GetTempPath()) ('ss-updater-test-' + [Guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($root)
try {
    $install = Join-Path $root 'install'
    $work = Join-Path $install '.ss-update-0123456789abcdef0123456789abcdef'
    [void][IO.Directory]::CreateDirectory($work)
    $package = Join-Path $work 'release.zip'
    $version = '0.10.0'
    function Write-Package([string[]]$Extra) {
        if ([IO.File]::Exists($package)) { [IO.File]::Delete($package) }
        $zip = [IO.Compression.ZipFile]::Open($package, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($name in (@('ShiningSource.exe', 'ss2_win64.exe', 'ss2_game.elf', 'SDL3.dll') + $Extra)) {
                $entry = $zip.CreateEntry("ShiningSource-$version/$name")
                $writer = New-Object IO.StreamWriter($entry.Open())
                try { $writer.Write("new $name") } finally { $writer.Dispose() }
            }
        } finally { $zip.Dispose() }
    }
    foreach ($name in @('ShiningSource.exe', 'ss2_win64.exe', 'shiningsource.ini', 'game.gba', 'game.sav')) {
        [IO.File]::WriteAllText((Join-Path $install $name), 'old')
    }

    Write-Package @('../elsewhere', 'game.sav')
    try { Install-Update $install $work $version ('0' * 64); throw 'accepted invalid digest' }
    catch { if ($_.Exception.Message -eq 'accepted invalid digest') { throw } }
    $digest = (Get-FileHash -LiteralPath $package).Hash
    try { Install-Update $install $work $version $digest; throw 'accepted traversal' }
    catch { if ($_.Exception.Message -eq 'accepted traversal') { throw } }
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'ss2_win64.exe') -Raw) -eq 'old') 'Invalid packages changed the host'

    $wideArt = @('forest-native-sides.png', 'language-native-sides.png', 'title-native-sides.png', 'shop-native-sides.png')
    Write-Package (@('shiningsource.ini', 'game.gba', 'game.sav') + @($wideArt | ForEach-Object { "assets/launcher/$_" }))
    $digest = (Get-FileHash -LiteralPath $package).Hash
    $locked = [IO.File]::Open((Join-Path $install 'ShiningSource.exe'), 'Open', 'Read', 'None')
    try {
        try { Install-Update $install $work $version $digest; throw 'accepted locked install' }
        catch { if ($_.Exception.Message -eq 'accepted locked install') { throw } }
    } finally { $locked.Dispose() }
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'ss2_win64.exe') -Raw) -eq 'old') 'Failed install did not restore the host'
    Assert-True (!(Test-Path -LiteralPath (Join-Path $install 'SDL3.dll'))) 'Failed install left a new DLL behind'

    Install-Update $install $work $version $digest
    Assert-True ((Get-Content -LiteralPath (Join-Path $install 'ShiningSource.exe') -Raw) -eq 'new ShiningSource.exe') 'Launcher was not replaced'
    foreach ($name in $wideArt) {
        Assert-True (Test-Path -LiteralPath (Join-Path $install "assets/launcher/$name")) "Wide artwork was not installed ($name)"
    }
    foreach ($name in @('shiningsource.ini', 'game.gba', 'game.sav')) {
        Assert-True ((Get-Content -LiteralPath (Join-Path $install $name) -Raw) -eq 'old') "Installation changed $name"
    }
} finally { [IO.Directory]::Delete($root, $true) }
Write-Output 'Updater transaction tests passed'
