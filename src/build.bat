@echo off
setlocal
rem Builds x64 Release version.dll (llvm-mingw clang++ or MSVC + MinHook).
set "HERE=%~dp0"
set "OUT=%HERE%..\build"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

set "LLVM_MINGW="
for /f "usebackq delims=" %%i in (`dir /b /ad /o-n "%HERE%..\tools\llvm-mingw-*" 2^>nul`) do if not defined LLVM_MINGW set "LLVM_MINGW=%HERE%..\tools\%%i"

if defined LLVM_MINGW if exist "%LLVM_MINGW%\bin\x86_64-w64-mingw32-clang++.exe" goto :build_mingw

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
if exist "%VS%\VC\Auxiliary\Build\vcvarsall.bat" if exist "%ProgramFiles(x86)%\Windows Kits\10\Include" (
    call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
    goto :build_msvc
)

echo Neither llvm-mingw nor full MSVC+Windows SDK was found.
exit /b 1

:build_mingw
set "CC=%LLVM_MINGW%\bin\x86_64-w64-mingw32-clang.exe"
set "CXX=%LLVM_MINGW%\bin\x86_64-w64-mingw32-clang++.exe"
pushd "%OUT%\obj"

"%CC%" -c -O2 -DNDEBUG -DWIN32_LEAN_AND_MEAN -I"%HERE%minhook\include" "%HERE%minhook\src\hook.c" -o hook.obj || goto :fail
"%CC%" -c -O2 -DNDEBUG -DWIN32_LEAN_AND_MEAN -I"%HERE%minhook\include" "%HERE%minhook\src\buffer.c" -o buffer.obj || goto :fail
"%CC%" -c -O2 -DNDEBUG -DWIN32_LEAN_AND_MEAN -I"%HERE%minhook\include" "%HERE%minhook\src\trampoline.c" -o trampoline.obj || goto :fail
"%CC%" -c -O2 -DNDEBUG -DWIN32_LEAN_AND_MEAN -I"%HERE%minhook\include" "%HERE%minhook\src\hde\hde64.c" -o hde64.obj || goto :fail
"%CC%" -c "%HERE%exports_gas.S" -o exports.obj || goto :fail
"%CXX%" -c -O2 -std=c++17 -DNDEBUG -DWIN32_LEAN_AND_MEAN -I"%HERE%minhook\include" "%HERE%dllmain.cpp" -o dllmain.obj || goto :fail

"%CXX%" -shared -O2 -static -s -Wl,--kill-at ^
    dllmain.obj hook.obj buffer.obj trampoline.obj hde64.obj exports.obj ^
    "%HERE%version.def" -lkernel32 -o "%OUT%\version.dll" || goto :fail

popd
echo.
echo Built (llvm-mingw x64 Release): %OUT%\version.dll
exit /b 0

:build_msvc
pushd "%OUT%\obj"

ml64 /nologo /c /Fo exports.obj "%HERE%exports.asm" || goto :fail

cl /nologo /c /O2 /MT /W3 /EHsc /std:c++17 /DNDEBUG /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /I"%HERE%minhook\include" ^
   "%HERE%dllmain.cpp" ^
   "%HERE%minhook\src\hook.c" "%HERE%minhook\src\buffer.c" "%HERE%minhook\src\trampoline.c" ^
   "%HERE%minhook\src\hde\hde64.c" || goto :fail

link /nologo /DLL /MACHINE:X64 /OPT:REF /OPT:ICF /DEF:"%HERE%version.def" /OUT:"%OUT%\version.dll" ^
   dllmain.obj hook.obj buffer.obj trampoline.obj hde64.obj exports.obj kernel32.lib || goto :fail

popd
echo.
echo Built (MSVC x64 Release): %OUT%\version.dll
exit /b 0

:fail
popd
echo BUILD FAILED
exit /b 1
