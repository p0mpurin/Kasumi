@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build-host\steam-video mkdir build-host\steam-video
cl /nologo /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Ivendor\mbedtls\include ^
  source\steam_proto.c source\steam_crypto.c tests\steam_video_test.c vendor\mbedtls\library\*.c ^
  /Fobuild-host\steam-video\ /Febuild-host\steam_video_test.exe /link bcrypt.lib advapi32.lib
if errorlevel 1 exit /b 1
build-host\steam_video_test.exe
