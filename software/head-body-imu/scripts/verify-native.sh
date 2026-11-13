#!/usr/bin/env bash
# 验证准备好的原生源码；仅访问本地模拟设备，不安装系统文件、不连接鸭子。
# 参数：运行时仓库根目录、独立构建目录（可选）；ORT_DYLIB_PATH 必须指向主机版运行库。
set -euo pipefail

component_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="${1:-}"
build_dir="${2:-$component_dir/build/native}"
if [[ $# -lt 1 || $# -gt 2 || ! -f "$runtime_dir/microduck-hd1910/firmware/Cargo.toml" ]]; then
    echo '用法：verify-native.sh <已应用补丁的运行时仓库> [独立构建目录]' >&2
    exit 2
fi
for command_name in cargo rustc cmake node jq bwrap; do
    command -v "$command_name" >/dev/null || { echo "缺少工具：$command_name" >&2; exit 1; }
done
if [[ ! -f "${ORT_DYLIB_PATH:-}" ]]; then
    echo '请将 ORT_DYLIB_PATH 指向已准备的主机版 libonnxruntime；脚本不自动下载或安装。' >&2
    exit 1
fi
firmware_dir="$(cd -- "$runtime_dir/microduck-hd1910/firmware" && pwd)"
mkdir -p -- "$build_dir"
build_dir="$(cd -- "$build_dir" && pwd)"
# 基线与修改版必须使用不同目录，防止同名 crate 的旧产物混用。
export CARGO_TARGET_DIR="$build_dir/cargo"
export MICRODUCK_HEAD_CORE_TEST_LIB="$build_dir/core/libmicroduck_head_body.so"

# 缺失原始模型是环境未备齐，不跳过断言，也不用另一张真实网络冒充原模型。
node --input-type=module - "$firmware_dir" <<'JS'
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
const firmware = process.argv[2];
for (const group of ['models', 'roller']) {
    const root = join(firmware, 'packaging', group);
    for (const item of readdirSync(root, { withFileTypes: true })) {
        if (!item.isDirectory()) {
            continue;
        }
        const directory = join(root, item.name);
        const manifestPath = join(directory, 'manifest.json');
        if (!existsSync(manifestPath)) {
            continue;
        }
        const manifest = JSON.parse(readFileSync(manifestPath));
        const modelPath = join(directory, 'policy.onnx');
        if (!existsSync(modelPath)) {
            throw new Error(`缺少原始模型夹具：${modelPath}`);
        }
        const digest = createHash('sha256').update(readFileSync(modelPath)).digest('hex');
        if (digest !== manifest.onnx_sha256) {
            throw new Error(`模型夹具摘要不匹配：${modelPath}`);
        }
    }
}
JS

cmake -S "$component_dir" -B "$build_dir/core" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir/core" -j2
ctest --test-dir "$build_dir/core" --output-on-failure
cd -- "$firmware_dir"
cargo test --locked --no-fail-fast -p duck-control -p robotd -p robotd-params
# 明确执行常规入口标为 ignored 的动态 ABI、循环网络和控制器运行库测试。
cargo test --locked -p duck-control -- --ignored
cargo test --locked -p robotd --bin robotd -- --ignored \
    --skip tests::a_mode_switch_that_arrives_on_a_blind_tick_is_not_lost \
    --skip tests::a_mode_switch_on_a_limp_robot_swaps_the_policies_and_leaves_it_limp \
    --skip tests::a_mode_switch_is_not_undone_by_a_policy_load_that_lands_after_it

# 仅模式切换状态机使用显式常量网络替身；真实动作推理用例仍使用上方核验的原模型。
layout_dir="$build_dir/layout-policy-fixture"
mkdir -p -- "$layout_dir"
for name in velstand.onnx alpha_sitstand.onnx alpha_ground_pick.onnx ball_kick_left.onnx \
    ball_kick_right.onnx roulade.onnx roller.onnx roller_crouch.onnx; do
    ln -sfn -- "$firmware_dir/duck-control/tests/fixtures/feedforward.onnx" "$layout_dir/$name"
done
cargo test --locked -p robotd --bin robotd --no-run --message-format=json > "$build_dir/test-build.jsonl"
test_binary="$(jq -r '
    select(.reason == "compiler-artifact" and .target.name == "robotd"
        and .profile.test == true and .executable != null) | .executable
' "$build_dir/test-build.jsonl")"
[[ -x "$test_binary" ]] || { echo '未取得唯一的 robotd 测试程序' >&2; exit 1; }
# /opt 仅在本次子进程的挂载空间中出现；主机目录只读，也没有外部网络。
bwrap --die-with-parent --ro-bind / / --tmpfs /opt --dir /opt/robot \
    --ro-bind "$firmware_dir/packaging" /opt/robot/feetech-ft5-r5 \
    --dir /opt/robot/policies --ro-bind "$layout_dir" /opt/robot/policies/current \
    --bind /tmp /tmp --dev-bind /dev /dev --proc /proc --unshare-net \
    -- "$test_binary" --ignored tests::a_mode_switch_
