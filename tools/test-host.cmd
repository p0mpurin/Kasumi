@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build-host mkdir build-host
cl /nologo /std:c11 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /Iinclude source\ws_protocol.c tests\ws_protocol_test.c /Fobuild-host\ /Febuild-host\ws_protocol_test.exe
if errorlevel 1 exit /b 1
build-host\ws_protocol_test.exe
