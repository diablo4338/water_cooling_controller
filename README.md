# BLE Pairing GUI

## Конечная задача
Нужно написать GUI приложение, которое:
- умеет париться с BLE устройством;
- по кнопке показывает список всех спаренных устройств;
- отображает данные в реальном времени в приложении, которые приходят с выбранного устройства.

## Реализация
Приложение написано на `PySide6`, BLE слой — `bleak`. Спаривание и авторизация реализованы на уровне приложения, через GATT‑протокол ниже. Это не системное OS‑bonding, а прикладное спаривание по собственному протоколу.
Список спаренных устройств и ключей хранится локально в `paired_devices.json`. Постоянный ключ хоста хранится в `host_key.pem`.

## Структура репозитория
- `ble_app/` — GUI приложение + общий BLE слой, используемый и GUI, и тестами.
- `mk/` — прошивка ESP‑IDF (NimBLE) с реализацией протокола и метрик.
- `raspberry/` — HTTP‑сервис для кнопки (GPIO), используется интеграционными тестами.
- `paired_devices.json` — база доверенных устройств (address, name, k_hex, last_connected).
- `host_key.pem` — приватный ключ хоста (P‑256), используется при pairing.

## Архитектура приложения
- `ble_app/config.py` — параметры таймаутов и ретраев, читаются из переменных окружения.
- `ble_app/core.py` — публичный BLE API и бизнес‑логика протокола (pair/auth/metrics).
- `ble_app/presentation.py` — слой UI‑состояния (Actions, AppState, derive_ui). Чистая логика без Qt.
- `ble_app/gui.py` — UI и потоковый worker, связаны с `AppModel` через Actions.
- `ble_app/main.py` — entrypoint для запуска GUI.

Сканирование показывает только устройства, которые рекламируют сервис `PAIR_SVC` — это считается признаком готовности к сопряжению.

Контракт UI:
- Все интерактивные элементы имеют `actionId = Action.name`.
- GUI не содержит бизнес‑логики, только биндинг `AppModel` ↔ виджеты.
- Поведение UI фиксируется unit‑тестами `ble_app/tests/test_presentation.py`.

UI поведение (derive_ui):
- Включено автоподключение или активен `AUTO_CONNECT` — доступна только эта кнопка/галка.
- Идет асинхронное действие (`SCAN/PAIR/CONNECT/DISCONNECT`) — кнопки недоступны.
- Устройство подключено — доступна только кнопка Отключить.
- Выбрано сохраненное устройство — доступны Сканировать, Подключить, Автоподключение.
- Выбрано найденное устройство — доступны Сканировать, Спарить, Автоподключение.
- По умолчанию — доступны Сканировать и Автоподключение.

## GATT протокол (актуальная прошивка)
### Advertising
- В pairing‑mode устройство рекламирует `PAIR_SVC` и имя `sensor-pair`.
- В обычном режиме — `MAIN_SVC` и имя `sensor`.

### Pairing service `PAIR_SVC`
Характеристики:
- `PAIR_DEV_NONCE` (read): `nonce_d` (16 байт)
- `PAIR_DEV_PUB` (read): `d_pub` (65 байт, P‑256 uncompressed)
- `PAIR_HOST_PUB` (write): `h_pub` (65 байт, P‑256 uncompressed)
- `PAIR_CONFIRM` (write): `HMAC(K, b"confirm"+nonce_d)`
- `PAIR_FINISH` (write): `0x01` — завершить pairing и сохранить доверенного

Алгоритм:
1. Хост подключается к устройству в pairing‑mode.
2. Читает `PAIR_DEV_NONCE` и `PAIR_DEV_PUB`.
3. Вычисляет ECDH `shared`, затем `K = HKDF(shared, salt=nonce_d, info="PAIRv1", len=32)`.
4. Пишет `PAIR_HOST_PUB`, затем `PAIR_CONFIRM`, затем `PAIR_FINISH`.
5. Устройство сохраняет identity хоста (hash `h_pub`) и выходит из pairing‑mode.

### Main service `MAIN_SVC`
Характеристики:
- `AUTH_NONCE` (read): 16 байт
- `AUTH_PROOF` (write): `HMAC(K, b"auth"+nonce)`

Алгоритм AUTH:
1. Хост подключается к устройству (обычный режим).
2. Читает `AUTH_NONCE`.
3. Пишет `AUTH_PROOF`.
4. После успешного AUTH устройство разрешает чтение/notify метрик.

### Config service `CONFIG_SVC`
Характеристики:
- `CONFIG_PARAMS` (read/write): пакет параметров (версия, маска, 3 float32 LE)
- `CONFIG_STATUS` (read/notify/write): статус применения; запись `0x01` — Apply
- `CONFIG_FAN_STATUS` (read/notify): статус вентилятора (`IDLE/STARTING/RUNNING/STALL/IN_SERVICE`)

Payload `CONFIG_PARAMS`:
- `version` (uint8) = `1`
- `mask` (uint8): бит0=target_temp_c, бит1=fan_min_rpm, бит2=alarm_delta_c
- `target_temp_c` (float32 LE)
- `fan_min_rpm` (float32 LE)
- `alarm_delta_c` (float32 LE)

Payload `CONFIG_STATUS`:
- `version` (uint8) = `1`
- `status` (uint8): `0=OK`, `1=INVALID`, `2=BUSY`
- `field` (uint8): `0..2` или `0xFF`, если без ошибки

Payload `CONFIG_FAN_STATUS`:
- `version` (uint8) = `1`
- `state` (uint8): `0=IDLE`, `1=STARTING`, `2=RUNNING`, `3=STALL`, `4=IN_SERVICE`
- `op_type` (uint8): `0=NONE`, `1=FAN_CALIBRATION` (заполняется когда `state=IN_SERVICE`)

Параметры кешируются в RAM и сохраняются в NVS; при старте прошивки загружаются из NVS.

### Operations service `OPERATIONS_SVC`
Характеристики:
- `OP_CONTROL` (write): запуск операции (`version/op_type/action`)
- `OP_STATUS` (read/notify): состояние операции

Payload `OP_CONTROL`:
- `version` (uint8) = `1`
- `op_type` (uint8): `1=FAN_CALIBRATION`
- `action` (uint8): `1=start`

Payload `OP_STATUS` (фикс. длина 24 байта):
- `version` (uint8) = `1`
- `op_type` (uint8): `1=FAN_CALIBRATION`
- `state` (uint8): `0=IDLE`, `1=IN_SERVICE`, `2=DONE`, `3=ERROR`
- `err_len` (uint8): `0..20`
- `err_text` (char[20], ASCII/UTF-8), заполнен только при `state=ERROR`
При старте при активной операции устройство отвечает `state=ERROR` и `err_text="busy"`.

### Metrics service `METRICS_SVC`
Характеристики:
- `TEMP0_VALUE` (read/notify): float32 LE
- `TEMP1_VALUE` (read/notify): float32 LE
- `TEMP2_VALUE` (read/notify): float32 LE
- `TEMP3_VALUE` (read/notify): float32 LE

## UUID (должны совпасть с прошивкой)
PAIR_SVC: `8fdd08d6-2a9e-4d5a-9f44-9f58b3a9d3c1`
MAIN_SVC: `3d1a4b35-9707-43e6-bf3e-2e2f7b561d82`
METRICS_SVC: `f3a0c1d2-5b6a-4d2e-9b43-1c2d3e4f5061`
CONFIG_SVC: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e10`
OPERATIONS_SVC: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e20`

PAIR_DEV_NONCE: `0b46b3cf-7e3b-44a3-8f39-4af2a8c9a1ee`
PAIR_DEV_PUB: `91c66f66-5c92-4c4d-86bf-6d2c58b6f0d7`
PAIR_HOST_PUB: `c9c8f69a-1f49-4ea0-a0a2-3c0d0a69e9d4`
PAIR_CONFIRM: `f5ee9c0b-96ae-4dc0-9b46-5f6f7f2ad2bf`
PAIR_FINISH: `a4c8e2c1-1c7b-4b06-a59f-4b5f8a2a8b3c`

AUTH_NONCE: `f1d1f9b6-8c92-47f6-a2f5-5b0a77d2e3a9`
AUTH_PROOF: `74cde77a-7f14-4e6e-b7f5-92ef0c3ad7e4`

CONFIG_PARAMS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e11`
CONFIG_STATUS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e12`
CONFIG_FAN_STATUS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e13`
OP_CONTROL: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e21`
OP_STATUS: `6d4f8a52-1f5c-4b02-9b7c-cc7f2a1d9e22`

TEMP0_VALUE: `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718291`
TEMP1_VALUE: `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718292`
TEMP2_VALUE: `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718293`
TEMP3_VALUE: `a1b2c3d4-0b1c-4a2b-9c3d-4e5f60718294`

## Запуск
1. Установить зависимости GUI: `ble_app/requirements.txt`.
2. Запустить `python -m ble_app.main` или `make run-app`.

Кнопки (для интеграционных тестов):
- `make run-buttons` или `uvicorn raspberry.app:app --host 0.0.0.0 --port 8001`.

Прошивка (ESP‑IDF):
- `make fw` — build/flash.
- `make fw-tests` — build/flash с `PAIR_RUN_TESTS=1`.

## Тесты
Интеграционные тесты используют только публичный API из пакета `ble_app`.
Юнит‑тесты для UI‑логики живут в `ble_app/tests/test_presentation.py` и не требуют PySide6.

Зависимости для тестов: `ble_app/tests/requirements.txt`.

Запуск:
```bash
pytest -c ble_app/pytest.ini -q -m integration
```

## Переменные окружения
Приложение (GUI/ble_app):
- `BLE_SCAN_TIMEOUT_S` — таймаут сканирования.
- `BLE_RESOLVE_TIMEOUT_S` — таймаут поиска по адресу.
- `BLE_CONNECT_TIMEOUT_S` — таймаут подключения.
- `BLE_PAIR_TIMEOUT_S` — таймаут pairing.
- `BLE_AUTH_TIMEOUT_S` — таймаут AUTH.
- `BLE_METRICS_TIMEOUT_S` — таймаут чтения метрик.
- `BLE_METRICS_RETRIES` — количество ретраев чтения метрик.
- `BLE_METRICS_RECONNECT_DELAY_S` — пауза перед переподключением при чтении метрик.
- `GUI_ACTION_DEFAULT_TIMEOUT_S` — дефолтный таймаут UI действий.
- `GUI_ACTION_SCAN_TIMEOUT_S` — таймаут UI сканирования.
- `GUI_ACTION_PAIR_TIMEOUT_S` — таймаут UI pairing.
- `GUI_ACTION_CONNECT_TIMEOUT_S` — таймаут UI подключения.
- `GUI_ACTION_DISCONNECT_TIMEOUT_S` — таймаут UI отключения.

Интеграционные тесты:
- `BLE_ADDRESS` — адрес устройства, чтобы не делать скан.
- `BLE_ADAPTER` — BLE адаптер (например, `hci0`).
- `SCAN_TIMEOUT_S` — таймаут сканирования (для тестов).
- `CONNECT_TIMEOUT_S` — таймаут подключения (для тестов).
- `PRESS_BASE_URL` — базовый URL для кнопок.
- `PRESS_TIMEOUT_S` — таймаут HTTP нажатия.
- `PRESS_RETRIES` — количество ретраев.
- `PRESS_NO_RESPONSE` — не ждать ответ от HTTP сервиса (1/0).
