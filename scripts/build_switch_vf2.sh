#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "== Building Virtua Fighter 2 for Nintendo Switch =="

if [ -z "$DEVKITPRO" ]; then
    echo "DEVKITPRO not set, running via Docker (devkitpro-mesa-rust:latest)..."
    docker run --rm \
        -v "${ROOT_DIR}/..":/work \
        -w "/work/$(basename "${ROOT_DIR}")" \
        devkitpro-mesa-rust:latest \
        bash -c "export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:\$PATH && \
                 cmake -S platform/switch -B build-switch-vf2 -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release -DM2_ROMSET=vf2 && \
                 cmake --build build-switch-vf2 -j\$(nproc)"
else
    export PATH="${DEVKITPRO}/devkitA64/bin:${DEVKITPRO}/tools/bin:${PATH}"
    cmake -S "${ROOT_DIR}/platform/switch" -B "${ROOT_DIR}/build-switch-vf2" \
        -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/Switch.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DM2_ROMSET=vf2
    cmake --build "${ROOT_DIR}/build-switch-vf2" -j$(nproc)
fi

cp "${ROOT_DIR}/build-switch-vf2/vf2_switch.nro" "${ROOT_DIR}/../vf2.nro"
echo "== Successfully built: vf2.nro =="
