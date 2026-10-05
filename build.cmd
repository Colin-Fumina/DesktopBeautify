@echo off
setlocal
rem ============================================================
rem  DesktopBeautify 构建脚本
rem  依赖：zig（单文件工具链）https://ziglang.org/download/
rem        解压后把 zig.exe 所在目录加入 PATH，或修改下面的 ZIG 变量
rem ============================================================

set "ZIG=zig"
set "OUT=%~dp0dist"
set "SRC=%~dp0src"

where %ZIG% >nul 2>nul
if errorlevel 1 (
  echo [错误] 找不到 zig。请安装后加入 PATH，或修改本脚本里的 ZIG 变量。
  echo        下载：https://ziglang.org/download/
  pause
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

echo ============================================
echo   编译 DesktopBeautify
echo ============================================
echo.

cd /d "%SRC%"

echo [1/3] DesktopBeautify.exe ...
"%ZIG%" cc -target x86_64-windows-gnu -O2 -s -Wall "-Wl,--subsystem,windows" ^
    -o "%OUT%\DesktopBeautify.exe" DesktopBeautify.c ^
    -luser32 -lshell32 -lgdi32 -ldwmapi -lcomctl32
if errorlevel 1 ( echo. & echo [失败] DesktopBeautify.c & pause & exit /b 1 )

echo [2/3] 恢复.exe ...
"%ZIG%" cc -target x86_64-windows-gnu -O2 -s -Wall "-Wl,--subsystem,windows" ^
    -o "%OUT%\Restore.exe" RestoreTool.c -luser32 -lshell32
if errorlevel 1 ( echo. & echo [失败] RestoreTool.c & pause & exit /b 1 )

echo [3/3] envcheck.exe ...
rem envcheck.c 用 wmain()，需要 -municode；不要加 --subsystem
"%ZIG%" cc -target x86_64-windows-gnu -municode -O2 -s -Wall ^
    -o "%OUT%\envcheck.exe" envcheck.c
if errorlevel 1 ( echo. & echo [失败] envcheck.c & pause & exit /b 1 )

echo.
echo [完成] 产物在 %OUT%
for %%F in ("%OUT%\DesktopBeautify.exe" "%OUT%\Restore.exe" "%OUT%\envcheck.exe") do echo   %%~nxF = %%~zF bytes

echo.
echo 注意：编译输出目录不要选带"低完整性"标签的文件夹，
echo       否则生成的 exe 会被 Windows 降权，运行时无法控制桌面。
echo.
pause
