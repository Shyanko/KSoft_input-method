param(
    [string]$QtPath = '',
    [string]$CompilerDirectory = '',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $QtPath) {
    $localQt = Join-Path $projectRoot 'build-qt\6.8.3\mingw_64'
    if (Test-Path -LiteralPath $localQt) {
        $QtPath = $localQt
    } elseif ($env:Qt6_DIR) {
        $QtPath = [IO.Path]::GetFullPath((Join-Path $env:Qt6_DIR '..\..\..'))
    } else {
        throw '请通过 -QtPath 指定与编译器匹配的 Qt 开发包目录。'
    }
}
$QtPath = [IO.Path]::GetFullPath($QtPath)
if (-not $CompilerDirectory) {
    $localCompiler = Join-Path $projectRoot 'build-qt\Tools\mingw1310_64\bin'
    if (Test-Path -LiteralPath $localCompiler) {
        $CompilerDirectory = $localCompiler
    } else {
        $CompilerDirectory = Split-Path -Parent (Get-Command g++.exe -ErrorAction Stop).Source
    }
}
$env:PATH = "$QtPath\bin;$CompilerDirectory;$env:PATH"
$env:QT_PLUGIN_PATH = Join-Path $QtPath 'plugins'
$buildDirectory = Join-Path $projectRoot 'build'
$compiler = Join-Path $CompilerDirectory 'g++.exe'
$qtConfig = Join-Path $QtPath 'lib\cmake\Qt6'
if (-not (Test-Path -LiteralPath $qtConfig)) {
    $qtConfig = Join-Path $QtPath 'lib\cmake\Qt5'
}
& cmake -S $projectRoot -B $buildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtPath" "-DQT_DIR=$qtConfig" "-DCMAKE_CXX_COMPILER=$compiler"
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }
& cmake --build $buildDirectory --parallel 4
if ($LASTEXITCODE -ne 0) { throw '编译失败' }
if (-not $SkipTests) {
    & ctest --test-dir $buildDirectory --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath (Join-Path $buildDirectory 'test-results.txt') -ErrorAction SilentlyContinue
        throw '测试失败'
    }
}
& (Join-Path $QtPath 'bin\windeployqt.exe') --release --no-translations --no-opengl-sw --no-system-d3d-compiler (Join-Path $buildDirectory 'ksipl.exe')
if ($LASTEXITCODE -ne 0) { throw 'Qt 运行库部署失败' }
foreach ($runtime in @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
    $runtimePath = Join-Path $CompilerDirectory $runtime
    if (Test-Path -LiteralPath $runtimePath) {
        Copy-Item -LiteralPath $runtimePath -Destination $buildDirectory -Force
    }
}
Write-Output "构建完成：$buildDirectory\ksipl.exe"
