@echo off
setlocal enabledelayedexpansion
SET APP_ABI="APP_ABI=armeabi-v7a arm64-v8a"
REM === CONFIGURATION BEGIN =============================================

SET BUILD_TOOLS=D:\programming\Android\Sdk\build-tools\35.0.0
SET ANDROID_JAR=D:\programming\Android\Sdk\platforms\android-34\android.jar
SET NDK=D:\programming\Android\Sdk\ndk\24.0.8215888

set jdk_home=D:\programming\JAVA\openjdk-17\jdk-17
set path=%path%;%jdk_home%\bin

set PROJ_ROOT=D:\programming\Android\Apk\LGL_Mod_Menu
set SRC_DIR=%PROJ_ROOT%\app\src\main
set OUT_DIR=%PROJ_ROOT%\build\bin
set OBJ_DIR=%PROJ_ROOT%\build\obj
set NDK_OUT=%PROJ_ROOT%\build\ndk
set NDK_LIBS=%NDK_OUT%\lib

set ANDROID_MK=%SRC_DIR%\jni\Android.mk

set JMINVER=1.8

REM === CONFIGURATION END ===============================================

TITLE Building dex + native libs
PUSHD "%~dp0"

REM --- Find JDK ---
SET JDK=%jdk_home%
IF EXIST "%JDK%\bin\javac.exe" GOTO JDKFOUND
SET JDK=%JAVA_HOME%
IF EXIST "%JDK%\bin\javac.exe" GOTO JDKFOUND
IF NOT EXIST "%JDK%\bin\javac.exe" GOTO JDKNOTFOUND
:JDKFOUND
echo [OK] JDK found: %JDK%

REM --- Cleanup ---
if exist "%OUT_DIR%" rd /s /q "%OUT_DIR%" >nul 2>nul
if exist "%OBJ_DIR%" rd /s /q "%OBJ_DIR%" >nul 2>nul
if exist "%NDK_OUT%" rd /s /q "%NDK_OUT%" >nul 2>nul
mkdir "%OUT_DIR%" "%OBJ_DIR%" "%NDK_OUT%" 2>nul

REM --- Check NDK ---
if not exist "%NDK%\ndk-build.cmd" (
    echo [ERROR] ndk-build.cmd not found in %NDK%
    goto EXIT
)
echo [OK] NDK found: %NDK%

REM --- Check Android.mk ---
if not exist "%ANDROID_MK%" (
    echo [ERROR] Android.mk not found: %ANDROID_MK%
    goto EXIT
)
echo [OK] Android.mk found

REM --- Java compile ---
echo [INFO] Compiling Java...
set JAVA_SRC_DIR=%SRC_DIR%\java
if not exist "%JAVA_SRC_DIR%" (
    echo [ERROR] Java source dir not found: %JAVA_SRC_DIR%
    goto EXIT
)

set ARGFILE=%OUT_DIR%\java_files.txt
if exist "%ARGFILE%" del "%ARGFILE%"
for /r "%JAVA_SRC_DIR%" %%f in (*.java) do (
    set "F=%%f"
    set "F=!F:\=/!"
    echo !F!>> "%ARGFILE%"
)

"%JDK%\bin\javac" -encoding UTF-8 -source %JMINVER% -target %JMINVER% -Xlint:-options -d "%OBJ_DIR%" -classpath "%JAVA_SRC_DIR%" -bootclasspath "%ANDROID_JAR%" @"%ARGFILE%"
if %errorlevel% neq 0 (
    echo [ERROR] Java compilation failed.
    goto EXIT
)
echo [OK] Java compiled.

REM --- DEX conversion (d8) ---
echo [INFO] Converting to DEX...
set OBJ_JAR=%OUT_DIR%\classes.jar
"%JDK%\bin\jar" cf "%OBJ_JAR%" -C "%OBJ_DIR%" .
if %errorlevel% neq 0 (
    echo [ERROR] Failed to create jar from class files.
    goto EXIT
)

call "%BUILD_TOOLS%\d8.bat" --output "%OUT_DIR%" --lib "%ANDROID_JAR%" "%OBJ_JAR%"
if %errorlevel% neq 0 (
    echo [ERROR] DEX conversion failed.
    goto EXIT
)
echo [OK] DEX created: %OUT_DIR%\classes.dex

REM --- NDK build ---
echo [INFO] Building native libraries with NDK...
call "%NDK%\ndk-build.cmd" NDK_PROJECT_PATH=%NDK_OUT% APP_BUILD_SCRIPT="%ANDROID_MK%" APP_STL=c++_static %APP_ABI% APP_OPTIM=release APP_THIN_ARCHIVE=true APP_PIE=true NDK_OUT=%NDK_OUT% NDK_LIBS_OUT=%NDK_LIBS%
if %errorlevel% neq 0 (
    echo [ERROR] NDK build failed.
    goto EXIT
)
echo [OK] Native libraries built: %NDK_LIBS%

echo.
echo ========================================
echo  BUILD COMPLETE!
echo  DEX:    %OUT_DIR%\classes.dex
echo  LIBS:   %NDK_LIBS%\
echo ========================================
echo.
echo [INFO] Structure of native libs:
dir /s /b "%NDK_LIBS%\*.so"

goto EXIT

:JDKNOTFOUND
echo JDK not found. Set JDK_HOME to point to JDK location.

:EXIT
cd "%PROJ_ROOT%"
pause
POPD
ENDLOCAL
@echo on