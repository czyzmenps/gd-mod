@echo off
title Live Collab server
cd /d "%~dp0"
where node >NUL 2>NUL
if errorlevel 1 (
  echo Node.js is not installed. Install the LTS version from https://nodejs.org and run this file again.
  pause
  exit /b 1
)
echo Live Collab server is starting on port 5588. Keep this window open while you play.
node server.js
pause
