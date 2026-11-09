# Exact release inputs. Runtime directories and arbitrary build outputs are
# never copied. Update this list when adding Flutter assets or native plugins.
$ViewerPayloadFiles = @(
    'chast_viewer.exe', 'chast-tunnel.exe', 'flutter_windows.dll',
    'flutter_gpu_texture_renderer_plugin.dll', 'msquic.dll', 'native_assets.json',
    'data\app.so', 'data\icudtl.dat',
    'data\flutter_assets\AssetManifest.bin',
    'data\flutter_assets\FontManifest.json',
    'data\flutter_assets\NativeAssetsManifest.json',
    'data\flutter_assets\NOTICES.Z',
    'data\flutter_assets\fonts\MaterialIcons-Regular.otf',
    'data\flutter_assets\shaders\ink_sparkle.frag',
    'data\flutter_assets\shaders\stretch_effect.frag'
)
$DriverPayloadFiles = @(
    'deviceinstaller64.exe', 'usbmmidd.inf', 'usbmmidd.cat',
    'x64\usbmmidd.dll', 'win32\usbmmidd.dll', 'License.txt'
)

function Copy-PayloadFiles([string]$Source, [string]$Destination, [string[]]$Files) {
    foreach ($relative in $Files) {
        $path = Join-Path $Source $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "missing release input: $path"
        }
        # Refuse junctions/symlinks even at an allowed path.
        $item = Get-Item -LiteralPath $path -Force
        while ($null -ne $item) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "release input traverses a reparse point: $path"
            }
            if ($item -is [IO.FileInfo]) { $item = $item.Directory }
            else { $item = $item.Parent }
        }
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $path -Destination $target
    }
}

function Copy-DriverPayload([string]$Source, [string]$Destination) {
    $hashes = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $Source 'manifest.sha256')) {
        if ($line.Trim() -eq '' -or $line.TrimStart().StartsWith('#')) { continue }
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
            throw 'invalid driver manifest'
        }
        $name = $Matches[2].Replace('/', '\')
        if ($hashes.ContainsKey($name)) { throw 'duplicate driver manifest entry' }
        $hashes[$name] = $Matches[1].ToLowerInvariant()
    }
    Copy-PayloadFiles $Source $Destination $DriverPayloadFiles
    $manifest = foreach ($name in $DriverPayloadFiles) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $Destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hashes[$name] -ne $hash) { throw "driver manifest mismatch: $name" }
        "$hash  $($name.Replace('\', '/'))"
    }
    # Only list the redistributed driver files, after checking the input hashes.
    Set-Content -LiteralPath (Join-Path $Destination 'manifest.sha256') -Value $manifest -Encoding ASCII
}
