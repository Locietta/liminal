#!/usr/bin/env bash
set -euo pipefail

source_root=${1:?usage: test-wsl.sh <source-root>}
mode=${2:-normal}
test -d "$source_root/.git"
test -d "$source_root/xmake"

audit_root=$(mktemp -d /tmp/liminal-wsl.XXXXXX)
cleanup() {
    case "$audit_root" in
        /tmp/liminal-wsl.*) rm -rf -- "$audit_root" ;;
        *) echo "refusing to remove unexpected path: $audit_root" >&2 ;;
    esac
}
trap cleanup EXIT

copy_worktree() {
    local source=$1
    local destination=$2
    shift 2
    mkdir -p "$destination"
    (
        cd "$source"
        git ls-files --cached --others --exclude-standard -z -- . "$@" |
            while IFS= read -r -d '' path; do
                if [[ -e "$path" || -L "$path" ]]; then
                    printf '%s\0' "$path"
                fi
            done |
            tar --null --no-recursion --files-from=- -cf -
    ) | tar -C "$destination" -xf -
}

# Copy the exact working files, including uncommitted and untracked source,
# without importing Windows build products or relying on network access.
copy_worktree "$source_root" "$audit_root" ':(exclude)xmake'
copy_worktree "$source_root/xmake" "$audit_root/xmake"

echo "Testing worktree snapshot from $(git -C "$source_root" rev-parse --short HEAD) in $audit_root"
cd "$audit_root"
case "$mode" in
    normal)
        pixi run configure
        pixi run build
        pixi run test-all
        ;;
    release)
        pixi run configure-release
        pixi run build
        pixi run test-all
        ;;
    sanitize)
        pixi run configure-sanitize
        # Sanitizer compiles use substantially more memory than normal builds.
        # Keep the disposable WSL validation lane within typical laptop and CI
        # runner memory limits.
        pixi run xmake build -y
        pixi run xmake test -vvvD
        ;;
    *)
        echo "unknown test mode: $mode" >&2
        exit 2
        ;;
esac
