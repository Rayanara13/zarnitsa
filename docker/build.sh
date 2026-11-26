#!/bin/bash
# Сборка внутри образа zarnitsa-build:astra17. Каталог сборки — том /build, исходники — /src.
set -euo pipefail
NAME="${1:-release}"
shift || true
BUILD="/build/${NAME}"
cmake -S /src -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
cmake --build "$BUILD" -j"$(nproc)"
