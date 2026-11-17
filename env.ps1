# Load ESP-IDF v5.4 into the current PowerShell session.
# Usage (from the project folder):   . .\env.ps1
# Then: idf.py build | idf.py -p COMx flash monitor
#
# Puts Espressif's bundled Python 3.11 ahead of any system Python so export.ps1 finds its venv.
$py = (Get-ChildItem "$env:USERPROFILE\.espressif\tools\idf-python" -Directory | Select-Object -First 1).FullName
$env:PATH = "$py;$env:PATH"
$env:IDF_PYTHON_ENV_PATH = "$env:USERPROFILE\.espressif\python_env\idf5.4_py3.11_env"
. "$env:USERPROFILE\esp\v5.4\esp-idf\export.ps1"
