@echo off
chcp 65001 >nul
setlocal

set "BASE=http://localhost:8080"

echo === 1. 设备上报 POST /report ===
curl -s -X POST %BASE%/report -H "Content-Type: application/json" -d "{\"device\":\"esp32s3-test\",\"version\":\"1.0.0\",\"temperature\":25.4,\"humidity\":61.2,\"rssi\":-58,\"uptime\":1234,\"lamp\":\"OFF\"}"
echo.

echo === 2. 灯命令轮询 GET /lampcmd ^(期望 204，无命令^) ===
curl -s -o nul -w "HTTP %%{http_code}\n" %BASE%/lampcmd

echo === 3. 控制台排队一条 ON ===
curl -s -X POST %BASE%/api/lamp -H "Content-Type: application/json" -d "{\"state\":\"ON\"}"
echo.

echo === 4. 再轮询 GET /lampcmd ^(期望 200 + {"lamp":"ON"}^) ===
curl -s -w "  <- HTTP %%{http_code}\n" %BASE%/lampcmd

echo === 5. 第三次轮询 ^(期望 204，命令已消费^) ===
curl -s -o nul -w "HTTP %%{http_code}\n" %BASE%/lampcmd

echo === 6. 查最新上报 GET /api/latest ===
curl -s %BASE%/api/latest
echo.

echo === 7. 查统计 GET /api/stats ===
curl -s %BASE%/api/stats
echo.

echo === 8. 查历史 GET /api/history ===
curl -s %BASE%/api/history
echo.

endlocal
