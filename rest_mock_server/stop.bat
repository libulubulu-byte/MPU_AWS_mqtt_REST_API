@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
title REST Mock Server - 停止

cd /d "%~dp0"

set "PORT=8080"
set "PIDFILE=%~dp0server.pid"

echo ============================================
echo   ESP32 REST 测试后台 - 停止
echo ============================================
echo.

set "KILLED=0"

rem ---------- 1. 按 PID 文件杀 ----------
if exist "%PIDFILE%" (
    set /p OLDPID=<"%PIDFILE%"
    tasklist /FI "PID eq !OLDPID!" 2>nul | find "!OLDPID!" >nul
    if not errorlevel 1 (
        taskkill /F /T /PID !OLDPID! >nul 2>nul
        if not errorlevel 1 (
            echo [成功] 已停止 PID=!OLDPID!
            set "KILLED=1"
        ) else (
            echo [警告] 杀 PID !OLDPID! 失败，可能权限不足。
        )
    ) else (
        echo [提示] PID 文件里的进程 !OLDPID! 已经不在了。
    )
    del "%PIDFILE%" >nul 2>nul
)

rem ---------- 2. 兜底：按端口占用再扫一遍 ----------
for /f "tokens=5" %%p in ('netstat -ano ^| findstr /R /C:":%PORT% .*LISTENING"') do (
    if not "%%p"=="0" (
        taskkill /F /T /PID %%p >nul 2>nul
        if not errorlevel 1 (
            echo [成功] 端口 %PORT% 被 PID=%%p 占用，已强制结束。
            set "KILLED=1"
        )
    )
)

echo.
if "!KILLED!"=="1" (
    echo 服务已停止。历史数据保留在 telemetry.db，下次启动继续累积。
) else (
    echo 没有发现正在运行的服务。
)
echo.
pause
endlocal
