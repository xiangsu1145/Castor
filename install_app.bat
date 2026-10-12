@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ======================================================
echo  Castor 手机端 APK 极速安装助手
echo ======================================================

set "APK_PATH=%~dp0Castor-Mobile\app\build\outputs\apk\debug\app-debug.apk"

if not exist "%APK_PATH%" (
    echo [提示] 尚未检测到已编译好的 APK，正在为您调用 Gradle 极速编译...
    cd /d "%~dp0Castor-Mobile"
    call .\gradlew.bat assembleDebug
    cd /d "%~dp0"
)

if not exist "%APK_PATH%" (
    echo [错误] 未找到 APK 产物: %APK_PATH%
    pause
    exit /b 1
)

echo [1/2] 检测已连接的 Android 设备...
adb devices
echo.

echo [2/2] 正在向手机安装最新的 Castor App...
adb install -r -d -t "%APK_PATH%"

if %ERRORLEVEL% equ 0 (
    echo.
    echo ======================================================
    echo  [成功] Castor App 安装成功！
    echo ======================================================
    echo 正在自动唤醒手机打开 Castor...
    adb shell am start -n io.github.xiangsu1145.castor/.MainActivity
) else (
    echo.
    echo [失败] 安装遇到问题，请检查手机是否开启了【允许USB安装】权限。
)

echo.
pause
