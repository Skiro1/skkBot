@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ============================================================
rem  release.bat — отправить релиз skkBot2 из этой папки
rem
rem  Использование:
rem    release.bat                       — коммит всех изменений + push + GitHub Release
rem    release.bat "комментарий коммита" — свой текст коммита
rem    release.bat ... path\to\skiro1.skk-bot.geode — свой путь к пакету
rem
rem  Пакет .geode ищется по порядку:
rem    1) путь, заданный вторым аргументом
rem    2) <repo>\build\skiro1.skk-bot.geode
rem    3) D:\SKKVPN\GEODE_MODS\skkBot2\build\skiro1.skk-bot.geode (dev-сборка)
rem  Тег/версия берутся из mod.json ("version").
rem ============================================================

set "REPO=%~dp0"
pushd "%REPO%" || (echo [!] Не найдена папка репозитория & exit /b 1)

rem ---- 1. Все изменения в папке: add -A + коммит + push ----
git add -A
if errorlevel 1 (echo [!] git add -A неудачен & popd & exit /b 1)

for /f "usebackq delims=" %%V in (`powershell -NoProfile -Command "(Get-Content mod.json -Raw | ConvertFrom-Json).version"`) do set "VERSION=%%V"
if "%VERSION%"=="" (echo [!] Не удалось прочитать version из mod.json & popd & exit /b 1)

git diff --cached --quiet
if errorlevel 1 (
    set "MSG=%~1"
    if "!MSG!"=="" set "MSG=release: !VERSION!"
    git commit -m "!MSG!"
    if errorlevel 1 (echo [!] Коммит неудачен & popd & exit /b 1)
    echo [OK] Коммит создан.
) else (
    echo [i] Нет изменений для коммита.
)

git push origin main
if errorlevel 1 (echo [!] git push неудачен & popd & exit /b 1)
echo [OK] Push в origin main выполнен.

rem ---- 2. Найти пакет .geode ----
set "PKG=%~2"
if "%PKG%"=="" if exist "%REPO%build\skiro1.skk-bot.geode" set "PKG=%REPO%build\skiro1.skk-bot.geode"
if "%PKG%"=="" if exist "D:\SKKVPN\GEODE_MODS\skkBot2\build\skiro1.skk-bot.geode" set "PKG=D:\SKKVPN\GEODE_MODS\skkBot2\build\skiro1.skk-bot.geode"
if "%PKG%"=="" (echo [!] Пакет .geode не найден — Release без артефакта не создаётся. & popd & exit /b 1)
echo [i] Пакет: %PKG%

rem ---- 3. Заметки релиза = верхняя секция changelog.md (до первого ---) ----
powershell -NoProfile -Command "$c = Get-Content changelog.md -Encoding UTF8; $i = ($c | Select-String -SimpleMatch '---' | Select-Object -First 1).LineNumber; $n = if ($i) { $c[0..($i-2)] } else { $c }; $n | Set-Content -Encoding UTF8 $env:TEMP\skkbot2_release_notes.md"
if errorlevel 1 (echo [!] Не удалось собрать заметки из changelog.md & popd & exit /b 1)

rem ---- 4. GitHub Release (нужен gh и валидный логин: gh auth login) ----
gh auth status >nul 2>&1
if errorlevel 1 (
    echo [!] gh не авторизован или токен недействителен. Выполни один раз: gh auth login -h github.com
    popd & exit /b 1
)

gh release view "%VERSION%" --repo Skiro1/skkBot >nul 2>&1
if errorlevel 1 (
    gh release create "%VERSION%" "%PKG%" --repo Skiro1/skkBot --title "%VERSION%" --notes-file "%TEMP%\skkbot2_release_notes.md"
) else (
    gh release upload "%VERSION%" "%PKG%" --repo Skiro1/skkBot --clobber
    if errorlevel 1 (echo [!] Загрузка артефакта неудачна & popd & exit /b 1)
    echo [i] Release %VERSION% уже существует — артефакт перезалит.
)
if errorlevel 1 (echo [!] Создание Release неудачно & popd & exit /b 1)

echo [DONE] Релиз %VERSION% опубликован: пакет %PKG%.
popd
endlocal
