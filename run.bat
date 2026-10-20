@echo off
SETLOCAL
set path=%path%;D:\programming\Android\Sdk\platform-tools
PUSHD "%~dp0"
REM Uninstall previous version of the app, if any
adb uninstall com.android.support
adb install "D:\programming\Android\LGLTeam_Android-Mod-Menu-5.0\Android-Mod-Menu-5.0\app\build\bin\LGL_Mod_Menu.apk"
pause
POPD
ENDLOCAL
@echo on
