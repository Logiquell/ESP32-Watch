@echo off
rem Load ESP-IDF v5.4 into the current cmd session (works even when PowerShell scripts are blocked).
rem Usage:  env.bat   then: idf.py build / idf.py -p COM5 flash monitor
rem Puts Espressif's bundled Python 3.11 ahead of any system Python so export.bat finds its venv.
for /d %%D in ("%USERPROFILE%\.espressif\tools\idf-python\*") do set "IDF_PY_DIR=%%D"
set "PATH=%IDF_PY_DIR%;%PATH%"
set "IDF_PYTHON_ENV_PATH=%USERPROFILE%\.espressif\python_env\idf5.4_py3.11_env"
call "%USERPROFILE%\esp\v5.4\esp-idf\export.bat" >nul
