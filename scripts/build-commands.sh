#!/usr/bin/env bash
# Сборка, запуск нативного демо и тестов из корня проекта
set -euo pipefail

# Нативная сборка, Python-модуль подключается отдельной опцией
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@"
cmake --build build -j
./build/fleetlink_demo
ctest --test-dir build --output-on-failure
