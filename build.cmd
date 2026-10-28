@echo off
setlocal
set MSVC=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0
set MOD=%~dp0

set INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt
set LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64
set PATH=%MSVC%\bin\Hostx64\x64;%PATH%

if not exist "%MOD%\build" mkdir "%MOD%\build"

cl /nologo /std:c++17 /O2 /EHsc /MD /W3 /utf-8 /DWIN32 /D_WINDOWS /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%MOD%\src" /I"%MOD%\vendor\imgui" /I"%MOD%\vendor\imgui\backends" ^
   /I"%MOD%\vendor\ffx-include" /I"%MOD%\vendor\minhook\include" ^
   "%MOD%\src\dllmain.cpp" "%MOD%\src\log.cpp" "%MOD%\src\config.cpp" ^
   "%MOD%\src\ffx_upscaler.cpp" "%MOD%\src\menu.cpp" "%MOD%\src\il2cpp_demosaic.cpp" ^
   "%MOD%\src\il2cpp_mvec.cpp" ^
   "%MOD%\vendor\imgui\imgui.cpp" "%MOD%\vendor\imgui\imgui_draw.cpp" ^
   "%MOD%\vendor\imgui\imgui_tables.cpp" "%MOD%\vendor\imgui\imgui_widgets.cpp" ^
   "%MOD%\vendor\imgui\backends\imgui_impl_dx11.cpp" ^
   "%MOD%\vendor\imgui\backends\imgui_impl_win32.cpp" ^
   "%MOD%\vendor\minhook\src\buffer.c" "%MOD%\vendor\minhook\src\hook.c" ^
   "%MOD%\vendor\minhook\src\trampoline.c" ^
   "%MOD%\vendor\minhook\src\hde\hde64.c" ^
   /Fo"%MOD%\build\\" /Fe"%MOD%\build\dxgi.dll" ^
   /link /DEF:"%MOD%\src\lillia.def" /DLL /MACHINE:X64 ^
   d3d11.lib d3d12.lib dxgi.lib user32.lib gdi32.lib shlwapi.lib /SUBSYSTEM:WINDOWS

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD OK: %MOD%\build\dxgi.dll
endlocal
