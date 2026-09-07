$ErrorActionPreference = 'Stop'
$msvc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
$sdkDir = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdk = $sdkDir.FullName
$sdkVer = $sdkDir.Name
Write-Host "SDK: $sdk"

$src = 'D:\repos\binbuf\3d-preview-windows\interactive-viewer'
$out = "$src\test-loader.exe"
$objDir = "$src\obj_test"
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

& "$msvc\bin\Hostx64\x64\cl.exe" /nologo /EHsc /std:c++20 /W4 /utf-8 /D_UNICODE /DUNICODE `
    "/I$msvc\include" "/I$sdk\um" "/I$sdk\shared" "/I$sdk\ucrt" `
    "$src\test-loader.cpp" "$src\Model.cpp" `
    "/Fe:$out" `
    /Fo:"$src\obj_test\" `
    /link /SUBSYSTEM:CONSOLE `
    "/LIBPATH:$msvc\lib\x64" `
    "/LIBPATH:C:\Program Files (x86)\Windows Kits\10\Lib\$sdkVer\um\x64" `
    "/LIBPATH:C:\Program Files (x86)\Windows Kits\10\Lib\$sdkVer\ucrt\x64"

if ($LASTEXITCODE -ne 0) { throw "compile failed" }
Write-Host "built: $out"
& $out "$src\tri_tight.glb" "$src\tri_interleaved.glb"
exit $LASTEXITCODE
