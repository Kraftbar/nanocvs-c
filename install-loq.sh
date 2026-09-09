#!/bin/sh
# Build from this checkout; keep the executable beside its existing database.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
service_dir=${1:-"$HOME/services/nanocvs-c"}
if [ ! -d "$service_dir" ] || [ ! -f "$repo/config.h" ]; then
    echo "Usage: $0 [existing service directory]" >&2
    echo "First copy the installation's config.h into this checkout." >&2
    exit 1
fi

# The original Makefile does not track all header dependencies.
make -C "$repo" clean
make -C "$repo"
"$repo/nanocvs-c" --version

if [ -f "$service_dir/nanocvs-c" ]; then
    state_dir=${XDG_STATE_HOME:-"$HOME/.local/state"}/nanocvs-c
    mkdir -p "$state_dir"
    backup_dir=$(mktemp -d "$state_dir/before-install.XXXXXXXX")
    cp -p "$service_dir/nanocvs-c" "$backup_dir/nanocvs-c"
    echo "Previous binary: $backup_dir/nanocvs-c"
fi

staged=$(mktemp "$service_dir/.nanocvs-c-install.XXXXXXXX")
trap 'rm -f "$staged"' EXIT HUP INT TERM
install -m 755 "$repo/nanocvs-c" "$staged"
mv -f "$staged" "$service_dir/nanocvs-c"
trap - EXIT HUP INT TERM
echo "Installed: $service_dir/nanocvs-c"
