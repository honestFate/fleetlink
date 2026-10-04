#!/usr/bin/env bash
# Приложение 11.3. Запускать из корня проекта после реализации ядра и тестов.
set -euo pipefail

# обычная сборка + Python-модуль
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
PYTHONPATH=build python3 demo.py

# ThreadSanitizer — на нативном бинаре, Python-модуль выключен
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DFLEETLINK_SANITIZER=thread -DFLEETLINK_BUILD_PYTHON=OFF
cmake --build build-tsan -j
./build-tsan/fleetlink_demo --scenario S2 2>&1 | tee tsan-S2.log

# AddressSanitizer — на тестах
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DFLEETLINK_SANITIZER=address -DFLEETLINK_BUILD_PYTHON=OFF
cmake --build build-asan -j
./build-asan/fleetlink_tests 2>&1 | tee asan-tests.log
