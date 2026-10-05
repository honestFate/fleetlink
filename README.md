# fleetlink

Нужны Linux, компилятор C++17, CMake ≥ 3.20, Make (либо Ninja),
Python ≥ 3.8 и заголовки разработки Python (пакет python3-dev в Debian/Ubuntu)
Внешние зависимости включены в `third_party`, загрузок при сборке нет:

- doctest 2.4.12 — тесты
- nlohmann/json 3.12.0 — JSONL
- pybind11 3.0.1 — Python-привязка
- Системная библиотека потоков — через CMake Threads

Версии, лицензии и контрольные суммы: `third_party/VERSIONS.txt`

Сборка и запуск:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLEETLINK_BUILD_PYTHON=ON && cmake --build build -j2
PYTHONPATH=build python3 demo.py
```

По умолчанию demo.py выполняет S1–S4, seed=42 и k=1
Отдельные сценарии п. 5 и журнал:

```sh
PYTHONPATH=build python3 demo.py --scenario S1 S2 S3 S4
PYTHONPATH=build python3 demo.py --scenario S5 --log-dir build/s5-logs
PYTHONPATH=build python3 demo.py --scenario S2 --k 3 --log-dir build/logs --log-level TRACE
```

S1 — чистый канал, S2 — потери/задержки/дубли, S3 — отказ M3 в начале тика 1000,
S4 — отсутствие АРМ на тиках 800–1000. S5 — 10 машин, 100 000 тиков, loss=0.1,
delay=0, dup=0; выполняется дважды со сравнением отчётов и SHA-256 журналов
Код 0 означает прохождение проверок сценария. Максимум ожидания относится к этому прогону
S3 проверяет ожидание M3 перед отказом; другой seed или k может не удовлетворять условию

`--k` задаёт вместимость обоих буферов; обслуживание в каждой точке всегда по одной машине
Журнал по умолчанию INFO; без `--log-dir` временные файлы удаляются после прогона
Нативное демо сохраняет S1–S4: `./build/fleetlink_demo --scenario S2 --trace > trace.jsonl`
Сборка только C++: `-DFLEETLINK_BUILD_PYTHON=OFF`

Тесты:

```sh
ctest --test-dir build --output-on-failure
```

Санитайзеры (GCC/Clang, без Python):

```sh
cmake -S . -B build-asan -DFLEETLINK_SANITIZER=address -DFLEETLINK_BUILD_PYTHON=OFF && cmake --build build-asan -j2
./build-asan/fleetlink_tests
cmake -S . -B build-tsan -DFLEETLINK_SANITIZER=thread -DFLEETLINK_BUILD_PYTHON=OFF && cmake --build build-tsan -j2
ctest --test-dir build-tsan -R 'fleetlink_demo(_trace|_s2|_s2_trace)?$' --output-on-failure
```

Если TSan в WSL завершается до старта с `unexpected memory mapping`, запускать ту же
команду через `setarch "$(uname -m)" -R ctest ...` для отключения ASLR у процесса
