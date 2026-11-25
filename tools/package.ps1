param(
    [ValidateSet('UCRT64', 'MSVC')]
    [string]$Toolchain = 'UCRT64',
    [switch]$NoBuild,
    [switch]$Configure
)
$ErrorActionPreference = 'Stop'

# 脚本位于 tools/，项目根在上一级。
$root = Split-Path -Parent $PSScriptRoot

if ($Toolchain -eq 'UCRT64') {
    $buildDir = Join-Path $root 'build'
    $release = Join-Path $buildDir 'Release'

    # 工具位置一律从 PATH 解析，脚本里不写死任何安装目录。
    $cxx = Get-Command g++.exe -ErrorAction SilentlyContinue
    $make = Get-Command mingw32-make.exe -ErrorAction SilentlyContinue
    $windres = Get-Command windres.exe -ErrorAction SilentlyContinue
    if ($null -eq $cxx -or $null -eq $make -or $null -eq $windres) {
        throw "UCRT64 tools not found on PATH: need g++.exe, mingw32-make.exe, windres.exe"
    }
    if ($cxx.Source -notmatch '[\\/]ucrt64[\\/]bin[\\/]') {
        throw "g++ on PATH is not the UCRT64 one: $($cxx.Source)"
    }
    # CMake 的缓存文件是脚本语义，路径必须用正斜杠，否则反斜杠会被当转义符。
    $cxxPath = $cxx.Source -replace '\\', '/'
    $makePath = $make.Source -replace '\\', '/'
    $windresPath = $windres.Source -replace '\\', '/'

    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        $Configure = $true
    }
    if ($Configure) {
        & cmake --fresh -S $root -B $buildDir -G 'MinGW Makefiles' `
            "-DCMAKE_MAKE_PROGRAM=$makePath" `
            "-DCMAKE_BUILD_TYPE=Release" `
            "-DCMAKE_CXX_COMPILER=$cxxPath" `
            "-DCMAKE_RC_COMPILER=$windresPath" | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed: $LASTEXITCODE" }
    }

    if (-not $NoBuild) {
        & cmake --build $buildDir | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed: $LASTEXITCODE" }
    }
} else {
    $buildDir = Join-Path $root 'build-msvc'
    $release = Join-Path $buildDir 'Release'
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        $Configure = $true
    }
    if ($Configure) {
        & cmake -S $root -B $buildDir -G 'Visual Studio 17 2022' -A x64 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed: $LASTEXITCODE" }
    }
    if (-not $NoBuild) {
        & cmake --build $buildDir --config Release | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed: $LASTEXITCODE" }
    }
}

$exe = Join-Path $release 'pet-native-cpp.exe'
if (-not (Test-Path $exe)) { throw "missing: $exe" }

$stage = Join-Path $root 'dist\native-win-x64'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $exe (Join-Path $stage 'pet-native-cpp.exe') -Force
foreach ($dll in Get-ChildItem -LiteralPath $release -Filter *.dll -File) {
    Copy-Item $dll.FullName (Join-Path $stage $dll.Name) -Force
}

# FFmpeg 运行库已由 CMake POST_BUILD 复制到 exe 目录（两种工具链都会复制）。
if ($Toolchain -eq 'MSVC') {
    # MSVC 链接的 FFmpeg DLL 仍可能依赖 MinGW 运行库；从 PATH 上的 g++ 目录取，
    # 找不到就跳过并提示，不在脚本里写死 MSYS2 安装位置。
    $gxx = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($null -eq $gxx) {
        Write-Warning 'g++ not on PATH; MinGW runtime DLLs were not copied.'
    } else {
        $mingwBin = Split-Path -Parent $gxx.Source
        foreach ($f in @('libwinpthread-1.dll', 'libgcc_s_seh-1.dll')) {
            $src = Join-Path $mingwBin $f
            if (Test-Path $src) { Copy-Item $src (Join-Path $stage $f) -Force }
        }
    }
}

Copy-Item (Join-Path $root 'assets\pet-config.json') (Join-Path $stage 'pet-config.json') -Force
New-Item -ItemType Directory -Force (Join-Path $stage 'assets\clips') | Out-Null
Copy-Item (Join-Path $root 'assets\clips\*.webm') (Join-Path $stage 'assets\clips') -Force
New-Item -ItemType Directory -Force (Join-Path $stage 'assets\sound') | Out-Null
Copy-Item (Join-Path $root 'assets\sound\*.wav') (Join-Path $stage 'assets\sound') -Force

$zip = Join-Path $root 'dist\native-win-x64.zip'
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force

$all = @(Get-ChildItem $stage -Recurse -File)
$bytes = ($all | Measure-Object Length -Sum).Sum
Write-Output ("toolchain: $Toolchain")
Write-Output ("stage : $stage")
Write-Output ("files : " + $all.Count + "  total: " + [math]::Round($bytes / 1MB, 1) + " MB")
Write-Output ("zip   : $zip")
