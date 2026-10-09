@echo off
rem Builds the Steam Link protocol code for Windows (MSVC) to test it against
rem Steam on this PC: build-host\steam_host_test.exe discover | pair | stream.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build-host\steam mkdir build-host\steam
cl /nologo /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Ivendor\mbedtls\include ^
  source\steam_proto.c source\steam_crypto.c source\steam_udp.c source\steam_remote.c source\steam_session.c ^
  tests\steam_host_test.c vendor\mbedtls\library\*.c ^
  /Fobuild-host\steam\ /Febuild-host\steam_host_test.exe /link ws2_32.lib bcrypt.lib advapi32.lib
