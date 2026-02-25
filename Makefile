PYTEST ?= pytest -c ble_app/pytest.ini
PYTHON ?= python3
UVICORN ?= uvicorn
BUTTON_HOST ?= 0.0.0.0
BUTTON_PORT ?= 8001
IDF_EXPORT ?= ~/esp-idf-v5.4.1/export.sh
IDF_PY ?= idf.py
IDF_DIR ?= mk

.PHONY: test test-integration test-unit run-app run-buttons fw fw-tests

test:
	$(PYTEST) -q

test-integration:
	$(PYTEST) -q -m integration

test-unit:
	$(PYTEST) -q -m "not integration"

run-app:
	$(PYTHON) -m ble_app.main

run-buttons:
	$(UVICORN) raspberry.app:app --host $(BUTTON_HOST) --port $(BUTTON_PORT)

fw:
	bash -lc '. $(IDF_EXPORT) && $(IDF_PY) -C $(IDF_DIR) build flash'

fw-tests:
	bash -lc '. $(IDF_EXPORT) && $(IDF_PY) -C $(IDF_DIR) -DPAIR_RUN_TESTS=1 build flash'
