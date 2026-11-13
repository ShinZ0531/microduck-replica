#!/usr/bin/env bash
# 在开发机构建诊断库并测试网页数据链路；不安装、不连接机器人、不写标定。
set -euo pipefail

component_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
studio_dir="$(cd -- "$component_dir/../studio" && pwd)"
build_dir="${HEAD_IMU_BUILD_DIR:-$component_dir/build}"

cmake -S "$component_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" -j2
ctest --test-dir "$build_dir" --output-on-failure
(cd "$component_dir/patches" && sha256sum --check SHA256SUMS)

# 只使用 Studio 已有依赖；Unix socket 和 HTTP 都指向测试创建的本地模拟服务。
cd "$studio_dir"
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=.:deps:dev python3 -m unittest \
    tests.test_head_imu tests.test_head_calibration tests.test_commissioning_view tests.test_sensors_http
node tests/head_imu_unit.mjs
