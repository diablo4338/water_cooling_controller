PYTEST ?= pytest -c cli/pytest.ini
PYTHON ?= python3
UVICORN ?= uvicorn
BUTTON_HOST ?= 0.0.0.0
BUTTON_PORT ?= 8001
IDF_EXPORT ?= ~/esp/esp-idf/export.sh
IDF_PY ?= idf.py
IDF_DIR ?= firmware

.PHONY: test test-integration test-unit run-app run-buttons fw fw-test fw-tests

test:
	$(PYTEST) -q

test-integration:
	$(PYTEST) -q -m integration

test-unit:
	$(PYTEST) -q -m "not integration"

run-app:
	$(PYTHON) -m cli.main

run-buttons:
	$(UVICORN) tests.raspberry.app:app --host $(BUTTON_HOST) --port $(BUTTON_PORT)

fw:
	bash -lc '. $(IDF_EXPORT) && $(IDF_PY) -C $(IDF_DIR) fullclean build flash'

fw-test:
	bash -lc '. $(IDF_EXPORT) && $(IDF_PY) -C $(IDF_DIR) -DPAIR_RUN_TESTS=1 fullclean build flash'

fw-tests: fw-test
