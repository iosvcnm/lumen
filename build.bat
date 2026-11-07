@echo off
setlocal
set ROOT=%~dp0
rem Requires MinGW-w64 g++ and windres on PATH. Set PATH before calling this script.

if not exist "%ROOT%tools\makeicon.exe" (
  g++ -std=c++17 -O2 -municode -o "%ROOT%tools\makeicon.exe" "%ROOT%tools\makeicon.cpp" -lgdiplus -lole32 -luuid -static-libgcc -static-libstdc++ -s
)
if not exist "%ROOT%lumen.ico" "%ROOT%tools\makeicon.exe" "%ROOT%lumen.ico"
windres "%ROOT%lumen.rc" -O coff -o "%ROOT%lumen_res.o"
if errorlevel 1 exit /b 1

g++ -std=c++17 -O2 -municode -mwindows -Wall -Wextra ^
  -o "%ROOT%lumen.exe" "%ROOT%src\lumen.cpp" "%ROOT%lumen_res.o" ^
  -lgdiplus -ldxva2 -lshell32 -luser32 -lgdi32 -ladvapi32 ^
  -lwbemuuid -lole32 -loleaut32 -luuid -lwinhttp -lws2_32 ^
  -static -static-libgcc -static-libstdc++ -s
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo Built "%ROOT%lumen.exe"

rem 可选代码签名：设置 SIGN_PFX（pfx 路径）与 SIGN_PFX_PASS 后重跑本脚本
if defined SIGN_PFX (
  if defined SIGN_PFX_PASS (
    signtool sign /fd SHA256 /f "%SIGN_PFX%" /p "%SIGN_PFX_PASS%" "%ROOT%lumen.exe"
  )
)
