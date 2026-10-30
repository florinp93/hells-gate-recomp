@echo off
setlocal
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\out\texture_studio"
python -m pip install --quiet -r "%~dp0requirements.txt" pyinstaller || exit /b 1
python -m PyInstaller --noconfirm --onefile --windowed --name TextureStudio ^
  --paths "%ROOT%\tools" --icon "%ROOT%\launcher\icon.ico" ^
  --distpath "%OUT%\dist" --workpath "%OUT%\build" --specpath "%OUT%" ^
  --exclude-module tkinter --exclude-module PySide6.QtQml --exclude-module PySide6.QtQuick ^
  --exclude-module PySide6.QtMultimedia --exclude-module PySide6.QtWebEngineCore ^
  "%~dp0__main__.py" || exit /b 1
echo Built %OUT%\dist\TextureStudio.exe
