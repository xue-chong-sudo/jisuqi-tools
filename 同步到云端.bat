@echo off
chcp 65001 >nul
:: 双击入口：调用同目录的 sync.ps1（绕过默认执行策略限制）
:: 此文件不含任何令牌，认证由 Windows 凭据管理器自动完成。
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sync.ps1"
