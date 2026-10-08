@echo off
rem Альтернативна збірка без Visual Studio (потрібен MinGW-w64, наприклад з MSYS2)
chcp 65001 >nul
if not exist bin mkdir bin
windres -I src src\app.rc -O coff -o bin\app.res.o || goto :err
g++ -std=c++17 -O2 -municode -mwindows src\main.cpp src\DiskInfo.cpp src\Updater.cpp bin\app.res.o -o bin\DiskMonitor.exe -static ^
    -lwbemuuid -lole32 -loleaut32 -lcomctl32 -lcomdlg32 -luxtheme -lshell32 -ladvapi32 -lgdi32 -luser32 -lwinhttp -lbcrypt || goto :err
del bin\app.res.o
echo Готово: bin\DiskMonitor.exe
pause
exit /b 0
:err
echo Помилка збірки
pause
exit /b 1
