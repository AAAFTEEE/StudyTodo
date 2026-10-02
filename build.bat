@echo off
rem ================================================
rem  StudyTodo 一键构建脚本
rem  依赖：MinGW-w64 (g++ / windres) 在 PATH 中
rem  产物：studytodo_v4.exe（输出到项目根目录）
rem ================================================
setlocal
cd /d "%~dp0"

if not exist build mkdir build

echo [1/2] 编译资源(视觉样式 manifest)...
windres res\app.rc -O coff -o build\app_res.o
if errorlevel 1 ( echo [失败] 资源编译出错 & pause & exit /b 1 )

echo [2/2] 编译主程序...
g++ -std=c++17 -municode -O2 -mwindows src\studytodo_v4.cpp build\app_res.o -o studytodo_v4.exe -lcomctl32 -lcomdlg32 -lgdi32 -luser32
if errorlevel 1 ( echo [失败] 主程序编译出错 & pause & exit /b 1 )

echo.
echo [完成] 构建成功：studytodo_v4.exe
echo 数据文件位于 homework\ 目录，运行前请保持 exe 与 homework 在同一目录。
pause
