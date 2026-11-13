#!/usr/bin/env bash
# 校验或应用本地运行时补丁；参数为已有 checkout 和 --check/--apply，不联网、不部署。
set -euo pipefail

component_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="${1:-}"
operation="${2:---check}"
base_commit='eecac1878136d3188370a5f6460b7d0521cf845d'
patches=(
    "$component_dir/patches/head-bmi088.patch"
    "$component_dir/patches/runtime-hardening.patch"
)

if [[ $# -lt 1 || $# -gt 2 || ! -d "$runtime_dir" ]]; then
    echo '用法：prepare-runtime.sh <原生运行时 checkout> [--check|--apply]' >&2
    exit 2
fi
if [[ "$operation" != '--check' && "$operation" != '--apply' ]]; then
    echo '只支持 --check 或 --apply。' >&2
    exit 2
fi
# 部分克隆可能为检查缺失对象而联网；本入口只接受已完整取得的本地源码。
partial_remotes="$(git -C "$runtime_dir" config --get-regexp '^remote\..*\.promisor$' || true)"
if [[ -n "$partial_remotes" ]]; then
    echo '请使用完整 checkout；本入口不为部分克隆自动拉取缺失对象。' >&2
    exit 1
fi
if [[ "$(git -C "$runtime_dir" rev-parse HEAD)" != "$base_commit" ]]; then
    echo '运行时基线不匹配；不要把 control.48 补丁直接套在其他版本上。' >&2
    exit 1
fi
if ! git -C "$runtime_dir" diff --quiet || ! git -C "$runtime_dir" diff --cached --quiet; then
    echo '运行时存在未提交修改；请先保存，不覆盖旧工程。' >&2
    exit 1
fi

# 摘要约束补丁内容；git apply 同时拒绝已经存在的新增目标文件。
(cd "$component_dir/patches" && sha256sum --check SHA256SUMS)
# 后续补丁可能修改前一补丁刚改过的行；在临时索引中按顺序验证，不写入 checkout。
validation_dir="$(mktemp -d)"
trap 'rm -rf -- "$validation_dir"' EXIT
export GIT_INDEX_FILE="$validation_dir/index"
git -C "$runtime_dir" read-tree HEAD
for patch in "${patches[@]}"; do
    # 临时索引不感知未跟踪文件；显式拒绝这些路径上的文件和符号链接。
    while IFS=$'\t' read -r added removed target; do
        if ! git -C "$runtime_dir" ls-files --error-unmatch -- "$target" >/dev/null 2>&1 \
            && [[ -e "$runtime_dir/$target" || -L "$runtime_dir/$target" ]]; then
            echo "新增补丁目标已存在：$target" >&2
            exit 1
        fi
    done < <(git -C "$runtime_dir" apply --numstat "$patch")
    git -C "$runtime_dir" apply --cached --whitespace=error-all "$patch"
done
unset GIT_INDEX_FILE
if [[ "$operation" == '--apply' ]]; then
    for patch in "${patches[@]}"; do
        git -C "$runtime_dir" apply --whitespace=error-all "$patch"
    done
    echo '已应用头部接入与原生修复补丁；尚未构建、部署或启用运动。'
else
    echo '补丁可应用；未修改运行时。'
fi
