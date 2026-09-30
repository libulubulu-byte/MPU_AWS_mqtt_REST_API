@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

cd /d "%~dp0"

set "PORT=8080"
set "PIDFILE=%~dp0server.pid"

echo ============================================
echo   ESP32 REST 测试后台 - 状态
echo ============================================
echo.

rem ---------- 进程状态 ----------
if exist "%PIDFILE%" (
    set /p PID=<"%PIDFILE%"
    tasklist /FI "PID eq !PID!" 2>nul | find "!PID!" >nul
    if not errorlevel 1 (
        echo [运行中] PID=!PID!
    ) else (
        echo [未运行] PID 文件存在但进程 !PID! 已消失，建议运行一次 stop.bat 清理。
    )
) else (
    echo [未运行] 没有 PID 文件。
)

rem ---------- 端口状态 ----------
echo.
echo 端口 %PORT% 监听情况:
netstat -ano | findstr /R /C:":%PORT% .*LISTENING"
if errorlevel 1 echo   ^(没有进程监听 %PORT%^)

rem ---------- HTTP 探活 ----------
echo.
echo HTTP 探活 http://localhost:%PORT%/api/stats :
curl -s -m 3 http://localhost:%PORT%/api/stats
if errorlevel 1 (
    echo   请求失败，服务可能没起来。
) else (
    echo.
)

echo.
echo 日志文件: %~dp0server.log
echo.
pause
endlocal
