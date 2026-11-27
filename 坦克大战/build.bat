@echo off
rem 一键编译（在 Visual Studio 开发者命令提示符里也可直接运行）
setlocal

set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /utf-8 /W3 /O2 /MD main.c /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib ws2_32.lib /OUT:TankBattle.exe
if errorlevel 1 (
  echo.
  echo [build failed]
  exit /b 1
)
echo.
echo [build ok] TankBattle.exe
endlocal
