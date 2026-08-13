@echo off
chcp 65001 >nul

cd /d "%~dp0"

rd /s /q "config\data" >nul 2>&1

start "PaperBreakEdgeService" cmd /k "chcp 65001 >nul & %~dp0out\build\local-windows-vs2026-debug\src\service\Debug\PaperBreakEdgeService.exe --console --config %~dp0config\default-config.json"