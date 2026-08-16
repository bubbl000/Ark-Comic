@echo off
chcp 65001 >nul
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cd /d "%~dp0"

set DST=%TEMP%\arkcomic_obj
set INC=/I src /I third_party\sqlite3 /I third_party\nlohmann /I third_party\miniz
set DEF=/DNOMINMAX /DWIN32_LEAN_AND_MEAN /DHAS_NVJPEG
set OPT=/nologo /c /std:c++20 /utf-8 /O2 /EHsc /Zi /FS /Fd%TEMP%\arkcomic_vc140.pdb

echo === Prepare temp obj dir ===
if not exist "%DST%" mkdir "%DST%"

echo === Compile src *.cpp ===
cl %OPT% %DEF% %INC% /Fo%DST%\src__ActivityLog.obj        src\ActivityLog.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__AppConfig.obj          src\AppConfig.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__ArchiveExtractor.obj   src\ArchiveExtractor.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__ComicLibraryService.obj src\ComicLibraryService.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__ComicStorageService.obj src\ComicStorageService.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__Dialogs.obj            src\Dialogs.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__I18n.obj               src\I18n.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__JsonUtil.obj           src\JsonUtil.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__LibraryManager.obj     src\LibraryManager.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__main.obj               src\main.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__MainWindow.obj         src\MainWindow.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__MsgBox.obj             src\MsgBox.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__ReaderWindow.obj       src\ReaderWindow.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__SettingsWindow.obj     src\SettingsWindow.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__TrayIcon.obj           src\TrayIcon.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__Ui.obj                 src\Ui.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__WindowBase.obj         src\WindowBase.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__decoders__FileMapping.obj     src\decoders\FileMapping.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__decoders__ImageCodecs.obj     src\decoders\ImageCodecs.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__decoders__JpegDecoder.obj     src\decoders\JpegDecoder.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__decoders__NvjpegHardDecoder.obj src\decoders\NvjpegHardDecoder.cpp
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\src__decoders__WicDecoder.obj     src\decoders\WicDecoder.cpp
if %ERRORLEVEL% NEQ 0 goto error

echo === Compile third_party ===
cl %OPT% %DEF% %INC% /Fo%DST%\third_party__sqlite3__sqlite3.obj third_party\sqlite3\sqlite3.c
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\third_party__miniz__miniz.obj     third_party\miniz\miniz.c
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\third_party__miniz__miniz_tinfl.obj third_party\miniz\miniz_tinfl.c
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\third_party__miniz__miniz_tdef.obj third_party\miniz\miniz_tdef.c
if %ERRORLEVEL% NEQ 0 goto error
cl %OPT% %DEF% %INC% /Fo%DST%\third_party__miniz__miniz_zip.obj third_party\miniz\miniz_zip.c
if %ERRORLEVEL% NEQ 0 goto error

echo === Compile resources ===
rc /nologo /fo%TEMP%\arkcomic_app.rc.res resources\app.rc
if %ERRORLEVEL% NEQ 0 goto error

echo === Link ArkComic.exe ===
link /nologo /OUT:build\ArkComic.exe /SUBSYSTEM:WINDOWS /DEBUG /INCREMENTAL:NO ^
    %DST%\src__ActivityLog.obj ^
    %DST%\src__AppConfig.obj ^
    %DST%\src__ArchiveExtractor.obj ^
    %DST%\src__ComicLibraryService.obj ^
    %DST%\src__ComicStorageService.obj ^
    %DST%\src__Dialogs.obj ^
    %DST%\src__I18n.obj ^
    %DST%\src__JsonUtil.obj ^
    %DST%\src__LibraryManager.obj ^
    %DST%\src__main.obj ^
    %DST%\src__MainWindow.obj ^
    %DST%\src__MsgBox.obj ^
    %DST%\src__ReaderWindow.obj ^
    %DST%\src__SettingsWindow.obj ^
    %DST%\src__TrayIcon.obj ^
    %DST%\src__Ui.obj ^
    %DST%\src__WindowBase.obj ^
    %DST%\src__decoders__FileMapping.obj ^
    %DST%\src__decoders__ImageCodecs.obj ^
    %DST%\src__decoders__JpegDecoder.obj ^
    %DST%\src__decoders__NvjpegHardDecoder.obj ^
    %DST%\src__decoders__WicDecoder.obj ^
    %DST%\third_party__sqlite3__sqlite3.obj ^
    %DST%\third_party__miniz__miniz.obj ^
    %DST%\third_party__miniz__miniz_tinfl.obj ^
    %DST%\third_party__miniz__miniz_tdef.obj ^
    %DST%\third_party__miniz__miniz_zip.obj ^
    %TEMP%\arkcomic_app.rc.res ^
    d2d1.lib dwrite.lib dxgi.lib windowscodecs.lib shlwapi.lib comctl32.lib gdi32.lib kernel32.lib user32.lib dwmapi.lib uuid.lib psapi.lib comdlg32.lib shell32.lib ole32.lib oleaut32.lib imm32.lib
if %ERRORLEVEL% NEQ 0 goto error

echo ====== OK: build\ArkComic.exe ======
goto :eof

:error
echo ====== BUILD FAILED ======
exit /b 1
