PYTHON ?= python3
UVICORN ?= uvicorn
BUTTON_HOST ?= 0.0.0.0
BUTTON_PORT ?= 8001
IDF_EXPORT ?= ~/esp/esp-idf/export.sh
IDF_PY ?= idf.py
IDF_DIR ?= firmware
CLI_PYTHONPATH ?= pcwcc/src
APP_ARGS ?=

.PHONY: test test-integration test-unit install-app build-exe run-app run-buttons fw fw-test fw-tests

test:
	PYTHONPATH=$(CLI_PYTHONPATH) $(PYTHON) -m pytest -c pcwcc/pytest.ini -q

test-integration:
	PYTHONPATH=$(CLI_PYTHONPATH) $(PYTHON) -m pytest -c pcwcc/pytest.ini -q -m integration

test-unit:
	PYTHONPATH=$(CLI_PYTHONPATH) $(PYTHON) -m pytest -c pcwcc/pytest.ini -q -m "not integration"

install-app:
	$(PYTHON) -m pip install -e "./pcwcc[dev]"

build-exe: install-app
	$(PYTHON) -m PyInstaller --noconfirm --clean --name PCWCC --windowed --onedir --paths pcwcc/src pcwcc/src/pcwcc/main.py

run-app: install-app
	PYTHONPATH=$(CLI_PYTHONPATH) $(PYTHON) -m pcwcc.main $(APP_ARGS)

run-buttons:
	$(UVICORN) tests.raspberry.app:app --host $(BUTTON_HOST) --port $(BUTTON_PORT)

fw:
	bash -lc '. $(IDF_EXPORT) && $(IDF_PY) -C $(IDF_DIR) fullclean build flash'

fw-test:
	bash -lc '. $(IDF_EXPORT) && $(IDF_PY) -C $(IDF_DIR) -DPAIR_RUN_TESTS=1 fullclean build flash'
