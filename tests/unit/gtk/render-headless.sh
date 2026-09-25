#!/usr/bin/env bash
set -euo pipefail
umask 077
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
output=${1:?output directory required}
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
pid=
display=
runtime_parent=$output
if [[ ${TMPDIR:-} == /* ]]; then
    runtime_parent=$TMPDIR
fi
runtime=$(mktemp -d "$runtime_parent/braurser-broadway.XXXXXX")
export XDG_RUNTIME_DIR="$runtime"
cleanup() {
    if [[ -n $pid ]]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rmdir -- "$runtime" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
for number in {17..40}; do
    /usr/bin/gtk4-broadwayd ":$number" > "$output/broadway.log" 2>&1 &
    pid=$!
    sleep 0.2
    if kill -0 "$pid" 2>/dev/null; then
        display=":$number"
        break
    fi
    wait "$pid" 2>/dev/null || true
    pid=
done
if [[ -z $display ]]; then
    printf '%s\n' 'Broadway did not start on displays 17–40' >&2
    while IFS= read -r line; do printf '%s\n' "$line" >&2; done < "$output/broadway.log"
    exit 1
fi
failures=()
run() {
    local name=$1
    shift
    if env -u WAYLAND_DISPLAY -u DISPLAY DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/no-session-bus" GDK_BACKEND=broadway BROADWAY_DISPLAY="$display" GSK_RENDERER=cairo "$@"; then
        return 0
    else
        local code=$?
        failures+=("$name (exit $code)")
        printf 'FAILED: %s (exit %s)\n' "$name" "$code" >&2
    fi
}
for width in 360 600 1024 1920; do
    for theme in light dark; do
        for page in cards badges controls banners progress document empty; do
            run "gtk-gallery-$page-$theme-$width.png" timeout 20 "$root/build/release/bin/braurser-gtk-gallery" --render-png "$output/gtk-gallery-$page-$theme-$width.png" --page "$page" --theme "$theme" --width "$width"
        done
        for page in discover search details updates installed settings activity git; do
            run "gtk-app-$page-$theme-$width.png" timeout 20 "$root/build/release/bin/braurser-gtk" --render-png "$output/gtk-app-$page-$theme-$width.png" --page "$page" --theme "$theme" --width "$width"
        done
    done
done
for suite in braurser-gtk-kit-tests braurser-gtk-app-tests; do
    if [[ -x $root/build/release/bin/$suite ]]; then
        run "$suite" timeout 120 "$root/build/release/bin/$suite"
    else
        failures+=("$suite (executable missing)")
    fi
done
if ((${#failures[@]})); then
    printf '\n%s GTK render/test failures:\n' "${#failures[@]}" >&2
    printf '  %s\n' "${failures[@]}" >&2
    exit 1
fi
printf '%s\n' 'All GTK renders and tests completed successfully.'
