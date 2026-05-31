---
description: Запустить одну итерацию работы агентов над текущей консолью
---

Запусти `orchestrator`. Сделай одну итерацию работы над текущей "in_progress" консолью (сейчас SNES). Если возникнут вопросы — остановись и выведи их.

Перед началом проверь:
1. База `agent_data\issues.db` существует:
   ```powershell
   Test-Path agent_data\issues.db
   ```
2. Бинарник `build\debug\emudor.exe` собран и свежий:
   ```powershell
   Test-Path build\debug\emudor.exe
   ```
3. Есть тестовые ROM-ы в `test_roms\snes\`:
   ```powershell
   Get-ChildItem -Path "test_roms\snes" -Recurse -Filter "*.sfc" | Select-Object -First 1
   ```

Если что-то отсутствует — скажи об этом и остановись.
