# Architecture

## App (ble_app)
- `ble_app/config.py` — загрузка конфигурации из env, единые таймауты/ретраи для BLE и GUI.
- `ble_app/core.py` — публичный BLE API и протокол (pair/auth/metrics). Этим API пользуются GUI и тесты.
- `ble_app/presentation.py` — чистая логика UI‑состояния (Actions, AppState, derive_ui).
- `ble_app/gui.py` — PySide6 GUI и `BleWorker`, который вызывает `BleAppCore`.
- `ble_app/main.py` — минимальный entrypoint для GUI.
- `ble_app/tests/` — unit‑ и integration‑тесты, работают через `ble_app/core.py`.

## Firmware (mk/)
- `mk/ble_protocol/main.c` — `app_main`, инициализация NVS, NimBLE, таймеров, metrics.
- `mk/ble_protocol/gap.c` — advertising, connect/disconnect, выбор PAIR/MAIN режима.
- `mk/ble_protocol/gatt.c` — GATT база (PAIR/MAIN/METRICS), callbacks чтения/записи.
- `mk/ble_protocol/uuid.c` — строковые UUID, источник истины для прошивки.
- `mk/ble_protocol/state.c` — FSM состояний (pairing, paired, authed) и хранение session‑state.
- `mk/ble_protocol/pair_state.c` — шаги pairing‑процесса.
- `mk/ble_protocol/conn_guard.c` — ограничения по connection handle.
- `mk/ble_protocol/crypto.c`, `ecdh.c`, `host_verify.c` — криптография, проверка host identity.
- `mk/ble_protocol/storage.c` — хранение доверенного хоста и ключа в NVS.
- `mk/ble_protocol/metrics_ble.c` — notify/handle метрик по BLE.
- `mk/metrics/` — источник метрик (ADS1115 и агрегация).
- `mk/build/` — артефакты сборки ESP‑IDF.

## Support service (raspberry/)
- `raspberry/app.py` — HTTP‑endpoint для GPIO кнопки, используется интеграционными тестами.

## Public API Contract
Любые изменения в `ble_app/core.py` должны сопровождаться корректировкой тестов и GUI. Этот файл является источником истины для BLE‑поведения приложения.
UUID‑константы в `ble_app/core.py` должны совпадать с `mk/ble_protocol/uuid.c`.
Actions из `ble_app/presentation.py` — публичный контракт между GUI и UI‑логикой. Переименование Actions требует обновления биндинга UI и тестов.
Все интерактивные элементы GUI помечаются `actionId = Action.name` и используются для smoke‑проверок.
