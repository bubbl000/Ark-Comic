@echo off
chcp 65001 >nul
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "%~dp0"

set NINJA=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

echo === Clean old build dir ===
if exist "build" rmdir /s /q "build"

echo === Configure CMake (Release) ===
cmake -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% NEQ 0 goto error

echo === Build ===
cmake --build build
if %ERRORLEVEL% NEQ 0 goto error

echo ====== Build OK ======
echo.
echo Program: %~dp0build\ArkComic.exe
echo Config: %LOCALAPPDATA%\ArkComic\config.json
echo.
pause
goto :eof

:error
echo ====== Build FAILED ======
pause