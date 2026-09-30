@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
title REST Mock Server - 启动

cd /d "%~dp0"

set "PORT=8080"
set "PIDFILE=%~dp0server.pid"
set "LOGFILE=%~dp0server.log"

echo ============================================
echo   ESP32 REST 测试后台 - 启动
echo ============================================
echo.

rem ---------- 1. 找 Python ----------
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    where python >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo [错误] 没找到 Python。
    echo        请先安装 Python 3 ^(https://www.python.org/downloads/^)，
    echo        安装时记得勾选 "Add python.exe to PATH"。
    echo.
    pause
    exit /b 1
)

rem ---------- 2. 已经在跑就直接退出 ----------
if exist "%PIDFILE%" (
    set /p OLDPID=<"%PIDFILE%"
    tasklist /FI "PID eq !OLDPID!" 2>nul | find "!OLDPID!" >nul
    if not errorlevel 1 (
        echo [提示] 服务已经在运行 ^(PID=!OLDPID!^)。
        echo        浏览器打开 http://localhost:%PORT%/
        echo        要重启请先运行 stop.bat。
        echo.
        pause
        exit /b 0
    )
    del "%PIDFILE%" >nul 2>nul
)

rem ---------- 3. 检查端口占用 ----------
netstat -ano | findstr /R /C:":%PORT% .*LISTENING" >nul
if not errorlevel 1 (
    echo [错误] 端口 %PORT% 已被占用，可能是别的程序或上一次没退干净。
    echo        查看占用进程:  netstat -ano ^| findstr :%PORT%
    echo        杀掉占用进程:  taskkill /F /PID ^<上面查到的PID^>
    echo        或者换端口:    修改本文件里的 set "PORT=8080"
    echo.
    pause
    exit /b 1
)

rem ---------- 4. 检查 Flask ----------
%PY% -c "import flask" >nul 2>nul
if errorlevel 1 (
    echo [信息] 缺少 Flask，正在自动安装...
    %PY% -m pip install --quiet --disable-pip-version-check flask
    if errorlevel 1 (
        echo [错误] Flask 安装失败。手动装一次试试:
        echo        %PY% -m pip install flask
        echo.
        pause
        exit /b 1
    )
    echo [信息] Flask 安装完成。
)

rem ---------- 5. 查本机局域网 IP，方便填进设备配网页 ----------
echo --------------------------------------------
echo   设备配网页要填的 REST 参数:
echo --------------------------------------------
set "HASIP=0"
for /f "delims=" %%a in ('%PY% "%~dp0lan_ip.py" 2^>nul') do (
    echo   rest_host  = %%a
    set "HASIP=1"
)
if "!HASIP!"=="0" (
    echo   rest_host  = ^<没找到局域网IP，用 ipconfig 自己看一眼^>
)
echo   rest_port  = %PORT%
echo   rest_path  = /report
echo   rest_cmd   = /lampcmd
echo   rest_tls   = 关 ^(off^)
echo.
echo   浏览器控制台:  http://localhost:%PORT%/
echo --------------------------------------------
echo.

rem ---------- 6. 后台启动 ----------
echo [信息] 正在启动服务...
%PY% "%~dp0launch.py" >nul 2>nul

if not exist "%PIDFILE%" (
    echo [错误] 启动失败，请看日志: %LOGFILE%
    echo.
    pause
    exit /b 1
)

rem 等它把端口监听起来
set /p NEWPID=<"%PIDFILE%"
set /a WAITCNT=0
:waitloop
rem 用 ping 做 1 秒延时：timeout 命令在输入被重定向时会直接报错并空转
ping -n 2 127.0.0.1 >nul
netstat -ano | findstr /R /C:":%PORT% .*LISTENING" >nul
if not errorlevel 1 goto running
set /a WAITCNT+=1
if !WAITCNT! lss 15 goto waitloop

echo [警告] 等了 15 秒端口还没起来，进程可能在报错。
echo        日志内容如下:
echo --------------------------------------------
type "%LOGFILE%"
echo --------------------------------------------
echo.
pause
exit /b 1

:running
echo [成功] 服务已启动 ^(PID=!NEWPID!^)，监听 0.0.0.0:%PORT%
echo.
echo   停止服务:  双击 stop.bat
echo   查看日志:  %LOGFILE%
echo.

rem ---------- 7. 打开浏览器 ----------
start "" "http://localhost:%PORT%/"

echo 按任意键关闭本窗口（服务继续在后台运行）。
pause >nul
endlocal
