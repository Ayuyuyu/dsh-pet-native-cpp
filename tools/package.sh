#!/usr/bin/env bash
# Linux 打包：编译 -> 暂存目录 -> tar.gz。布局参考 Win 端，但配置放在 assets/，
# 且 Linux 端不使用音效，故不打包 sound/。
# 用法：./tools/package.sh [--no-build]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 脚本位于 tools/，项目根在上一级；自包含布局下 assets/ 与源码同级。
root="$(cd "$here/.." && pwd)"
no_build=0
case "${1:-}" in
  '') ;;
  --no-build) no_build=1 ;;
  *) echo "usage: $0 [--no-build]" >&2; exit 2 ;;
esac

build_dir="$root/build"
if (( ! no_build )); then
  cmake -S "$root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" --parallel "$(nproc)"
fi

bin="$build_dir/pet-linux"
[[ -x "$bin" ]] || { echo "missing: $bin" >&2; exit 1; }

# 资源与 dist 都相对项目根解析。
proj="$root"
if [[ ! -d "$proj/assets/clips" ]]; then
  echo "assets/ not found under $proj" >&2
  exit 1
fi
assets="$proj/assets"

stage="$proj/dist/native-linux-x64"
rm -rf "$stage"
mkdir -p "$stage/assets/clips"

install -m 755 "$bin" "$stage/pet-linux"
install -m 644 "$assets/pet-config.json" "$stage/assets/pet-config.json"
install -m 644 "$assets"/clips/*.webm "$stage/assets/clips/"
# Linux 端不使用音效（代码无 wav 引用），故不打包 sound/；Win 端才需要。

# 运行时依赖都是发行版系统库（Arch 上均位于 /usr/lib）。
# 不随包捆绑 .so：目标机器需安装 gtk3 / ffmpeg / mesa(GL) / libx11 / libxext。
# 若缺库，下面给出告警而不是打一个不可移植的"半截"包。
missing=$(ldd "$stage/pet-linux" 2>/dev/null | awk '/not found/{print $1}' || true)
if [[ -n "$missing" ]]; then
  echo "unresolved shared libraries on this host:" >&2
  echo "$missing" >&2
  echo "Install the corresponding distro packages before distributing." >&2
  exit 1
fi

archive="$proj/dist/native-linux-x64.tar.gz"
rm -f "$archive"
tar -czf "$archive" -C "$(dirname "$stage")" "$(basename "$stage")"

files=$(find "$stage" -type f | wc -l)
bytes=$(du -sb "$stage" | awk '{print $1}')
echo "stage : $stage"
printf 'files : %s  total: %s MB\n' "$files" "$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}')"
echo "tar   : $archive"
