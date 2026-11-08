param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$ExistingPrefix,
    [Parameter(Mandatory = $true)][string]$MsysRoot,
    [Parameter(Mandatory = $true)][string]$VisualStudioRoot,
    [string]$OutputRoot = (Join-Path $PSScriptRoot '../build/ffmpeg-recording')
)
$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$ExistingPrefix = (Resolve-Path -LiteralPath $ExistingPrefix).Path
$MsysRoot = (Resolve-Path -LiteralPath $MsysRoot).Path
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if ($OutputRoot -eq $ExistingPrefix -or $OutputRoot -eq $SourceRoot) {
    throw 'OutputRoot must be a separate build directory.'
}
# Keep the installed hardware codec libraries. Only rebuild the container
# library, using the identical public ABI and the same static MSVC runtime.
foreach ($version in @('version.h', 'version_major.h')) {
    foreach ($library in @('libavformat', 'libavcodec', 'libavutil')) {
        if ($library -eq 'libavutil' -and $version -eq 'version_major.h') { continue }
        $sourceVersion = Get-Content -LiteralPath (Join-Path $SourceRoot "$library/$version") |
            Select-String '^#define LIBAV.*VERSION_(MAJOR|MINOR|MICRO)\s'
        $installedVersion = Get-Content -LiteralPath (Join-Path $ExistingPrefix "include/$library/$version") |
            Select-String '^#define LIBAV.*VERSION_(MAJOR|MINOR|MICRO)\s'
        if (($sourceVersion -join "`n") -ne ($installedVersion -join "`n")) {
            throw "FFmpeg source and installed headers differ: $library/$version"
        }
    }
}
Import-Module (Join-Path $VisualStudioRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $VisualStudioRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$env:CHAST_FFMPEG_SOURCE = $SourceRoot
$env:CHAST_FFMPEG_OUTPUT = $OutputRoot
$env:PATH = (Join-Path $MsysRoot 'usr/bin') + ';' + $env:PATH
$compile = Join-Path $OutputRoot 'compile'
$prefix = Join-Path $OutputRoot 'prefix'
New-Item -ItemType Directory -Force -Path $compile, (Join-Path $prefix 'lib') | Out-Null
$script = @'
set -euo pipefail
source_root=$(cygpath -u "$CHAST_FFMPEG_SOURCE")
output_root=$(cygpath -u "$CHAST_FFMPEG_OUTPUT")
cd "$output_root/compile"
bash "$source_root/configure" --toolchain=msvc --target-os=win32 --arch=x86_64 \
  --disable-autodetect --disable-everything --disable-programs --disable-doc \
  --disable-shared --enable-static --disable-x86asm --disable-avdevice \
  --disable-avfilter --disable-swscale --disable-swresample --enable-avformat \
  --enable-muxer=mp4 --enable-demuxer=mov --enable-protocol=file \
  --extra-cflags=-MT --extra-cxxflags=-MT --enable-optimizations
make -j4 libavformat/avformat.lib
'@
$scriptPath = Join-Path $OutputRoot 'build.sh'
[IO.File]::WriteAllText($scriptPath, $script.Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($false))
& (Join-Path $MsysRoot 'usr/bin/bash.exe') $scriptPath
if ($LASTEXITCODE -ne 0) { throw "FFmpeg container build failed: $LASTEXITCODE" }
Copy-Item -LiteralPath (Join-Path $ExistingPrefix 'include') -Destination $prefix -Recurse -Force
foreach ($library in @('avcodec.lib', 'avutil.lib', 'swresample.lib', 'swscale.lib')) {
    Copy-Item -LiteralPath (Join-Path $ExistingPrefix "lib/$library") -Destination (Join-Path $prefix 'lib') -Force
}
Copy-Item -LiteralPath (Join-Path $compile 'libavformat/avformat.lib') -Destination (Join-Path $prefix 'lib') -Force
Write-Output "FFMPEG_ROOT=$prefix"
