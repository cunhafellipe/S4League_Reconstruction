# Build s4client (Win32/x86). Usage: .\build.ps1 [Debug|Release]
param([string]$Config = "Debug")
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
    $msbuild = (& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" `
        -latest -requires Microsoft.Component.MSBuild `
        -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1)
}
& $msbuild "$PSScriptRoot\s4client.sln" /p:Configuration=$Config /p:Platform=Win32 /m /nologo /v:minimal
