#!/usr/bin/env bash
set -euo pipefail
umask 022

prefix=/usr/local
uninstall=0
yes=0
skip_tests=0
verbose=0
manifest=/var/lib/braurser/install-manifest.tsv
policy=/usr/share/polkit-1/actions/org.braurser.pacman.policy
work=

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

usage() {
    printf '%s\n' 'Usage: ./install.sh [--prefix /path] [--yes] [--skip-tests] [--verbose] [--uninstall]' 'Run as your normal user. Installation uses sudo; --uninstall removes only unchanged installed files.'
}

while (($#)); do
    case $1 in
        --prefix)
            (($# >= 2)) || die 'A path is needed after --prefix.'
            prefix=$2
            shift 2 ;;
        --prefix=*) prefix=${1#--prefix=}; shift ;;
        --yes) yes=1; shift ;;
        --skip-tests) skip_tests=1; shift ;;
        --verbose) verbose=1; shift ;;
        --uninstall) uninstall=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) die "Unknown option: $1" ;;
    esac
done

safe_path() {
    [[ $1 == /* && $1 =~ ^/[A-Za-z0-9_./-]+$ && $1 != *'//'* && $1 != */./* && $1 != */../* && $1 != */. && $1 != */.. ]]
}

safe_path "$prefix" || die 'Choose a simple absolute install prefix.'
[[ $prefix != / && $prefix != */ ]] || die 'Choose a prefix other than /.'
((${#prefix} <= 200)) || die 'The install prefix is too long.'
((EUID != 0)) || die 'Run this as your normal user, not as root.'
command -v sudo >/dev/null || die 'sudo is needed to install and remove files. Please install sudo first.'

show_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

privileged() {
    show_command sudo "$@" >&2
    sudo "$@"
}

run() {
    if ((verbose)); then show_command "$@" >&2; fi
    "$@"
}

confirm() {
    local answer
    if ((yes)); then return 0; fi
    [[ -t 0 ]] || die 'Confirmation needs a terminal; use --yes to continue.'
    read -r -p "$1 [y/N] " answer || return 1
    [[ $answer == [Yy] || $answer == [Yy][Ee][Ss] ]]
}

cleanup() {
    if [[ -n $work ]]; then rm -rf -- "$work"; fi
}
trap cleanup EXIT
work=$(mktemp -d)

check_parents() {
    local path=$1 parent
    parent=${path%/*}
    while [[ -n $parent && $parent != / ]]; do
        [[ ! -L $parent ]] || die "A folder in this path is a link: $parent"
        parent=${parent%/*}
    done
}

allowed_path() {
    safe_path "$1" || return 1
    [[ $1 == "$prefix/"* || $1 == "$policy" ]]
}

read_manifest() {
    local path hash mode backup extra line=0
    declare -gA old_hash=() old_mode=() old_backup=()
    if privileged test -L "$manifest"; then die 'The install record cannot be a link.'; fi
    if ! privileged test -e "$manifest"; then return 0; fi
    [[ $(privileged stat -c '%u:%g:%a:%F' "$manifest") == '0:0:600:regular file' ]] || die 'The install record is not a private root-owned file.'
    install -m 0600 /dev/null "$work/previous.tsv"
    privileged cat -- "$manifest" > "$work/previous.tsv"
    while IFS=$'\t' read -r path hash mode backup extra || [[ -n $path ]]; do
        ((line += 1))
        [[ -z ${extra:-} && -n $path && -n $hash && -n $mode && -n $backup ]] || die "Invalid install record on line $line."
        allowed_path "$path" || die "Unexpected path in the install record: $path"
        [[ $hash =~ ^[a-f0-9]{64}$ && $mode =~ ^0?[0-7]{3}$ ]] || die "Invalid checksum or mode on line $line."
        if [[ $backup != - ]]; then
            safe_path "$backup" || die "Invalid backup on line $line."
            [[ $backup =~ ^/var/lib/braurser/backups/[0-9]{8}T[0-9]{6}Z-[0-9]+/ ]] || die "Invalid backup on line $line."
            [[ $backup == *"/${path#/}" ]] || die "Backup does not match its file on line $line."
        fi
        [[ ! -v old_hash["$path"] ]] || die "Repeated path in the install record: $path"
        old_hash["$path"]=$hash
        old_mode["$path"]=$mode
        old_backup["$path"]=$backup
    done < "$work/previous.tsv"
}

checksum() {
    local result
    result=$(privileged sha256sum -- "$1")
    printf '%s' "${result%% *}"
}

exists() {
    privileged test -e "$1" || privileged test -L "$1"
}

remove_autostart() {
    local marker="$HOME/.local/state/braurser/autostart-installed"
    local entry="$HOME/.config/autostart/braurser-notifier.desktop"
    local saved_prefix saved_hash
    [[ -f $marker && ! -L $marker ]] || return 0
    IFS=$'\t' read -r saved_prefix saved_hash < "$marker" || true
    [[ $saved_prefix == "$prefix" && $saved_hash =~ ^[a-f0-9]{64}$ ]] || return 0
    if [[ -f $entry && ! -L $entry ]]; then
        current=$(sha256sum -- "$entry")
        if [[ ${current%% *} == "$saved_hash" ]]; then
            rm -- "$entry"
            printf 'Removed sign-in update checks.\n'
        else
            printf 'Kept your changed sign-in setting: %s\n' "$entry"
            return 0
        fi
    fi
    rm -- "$marker"
}

if ((uninstall)); then
    read_manifest
    ((${#old_hash[@]})) || die 'No installation record was found for this prefix.'
    printf 'Files to remove: %s. Your personal files and installed packages stay.\n' "${#old_hash[@]}"
    confirm 'Remove braurser?' || die 'Nothing was removed.'
    for path in "${!old_hash[@]}"; do
        check_parents "$path"
        if exists "$path"; then
            if [[ -L $path || $(checksum "$path") != "${old_hash[$path]}" ]]; then
                printf 'Kept a changed file: %s\n' "$path"
                continue
            fi
            privileged rm -- "$path"
        fi
        backup=${old_backup[$path]}
        if [[ $backup != - ]]; then
            check_parents "$backup"
            if exists "$backup"; then
                privileged mkdir -p -m 0755 -- "${path%/*}"
                privileged mv -- "$backup" "$path"
                parent=${backup%/*}
                while [[ $parent == /var/lib/braurser/backups/* ]]; do
                    privileged rmdir --ignore-fail-on-non-empty -- "$parent"
                    parent=${parent%/*}
                done
            fi
        fi
    done
    remove_autostart
    privileged rm -- "$manifest"
    for size in 16 24 32 48 64 128 256; do
        for dir in "$prefix/share/icons/hicolor/${size}x${size}/apps" "$prefix/share/icons/hicolor/${size}x${size}"; do
            if [[ -d $dir ]]; then privileged rmdir --ignore-fail-on-non-empty -- "$dir"; fi
        done
    done
    for dir in "$prefix/share/icons/hicolor/scalable/apps" "$prefix/share/icons/hicolor/scalable" "$prefix/share/icons/hicolor/symbolic/apps" "$prefix/share/icons/hicolor/symbolic" "$prefix/share/icons/hicolor" "$prefix/libexec/braurser" "$prefix/share/braurser/copy" "$prefix/share/braurser" "$prefix/share/applications" "$prefix/bin" /var/lib/braurser/backups /var/lib/braurser /var/log/braurser; do
        if privileged test -d "$dir"; then privileged rmdir --ignore-fail-on-non-empty -- "$dir"; fi
    done
    printf 'Uninstall finished. Personal files and packages were kept.\n'
    exit 0
fi

command -v pacman >/dev/null || die 'This installer needs pacman.'
if [[ -f /etc/os-release ]]; then
    . /etc/os-release
    if [[ " ${ID:-} ${ID_LIKE:-} " != *arch* ]]; then
        printf 'This system does not identify as Arch-based; continuing because pacman is available.\n'
    fi
fi

deps=(base-devel cmake ninja gcc gtkmm-4.0 glibmm-2.68 qt6-base qt6-svg nlohmann-json curl libarchive sqlite libxml2 gumbo-parser libgit2 icu gtest fakeroot bubblewrap git libjxl polkit desktop-file-utils shared-mime-info librsvg xdg-utils archlinux-appstream-data)
missing_text=$(run pacman -T "${deps[@]}") || {
    status=$?
    ((status == 127)) || die 'Could not check the required packages.'
}
missing=()
if [[ -n $missing_text ]]; then mapfile -t missing <<< "$missing_text"; fi
((${#missing[@]} <= ${#deps[@]})) || die 'The package check returned unexpected results.'
for package in "${missing[@]}"; do
    matched=0
    for dependency in "${deps[@]}"; do
        if [[ $package == "$dependency" ]]; then matched=1; break; fi
    done
    ((matched)) || die 'The package check returned an unexpected name.'
done
if ((${#missing[@]})); then
    printf 'Build tools needed: %s\n' "${missing[*]}"
    confirm 'Install these packages with pacman?' || die 'The missing packages are needed to continue.'
    if ((yes)); then
        privileged pacman -S --needed --noconfirm -- "${missing[@]}" || die 'Package installation failed. Update your system first if the package database is out of date.'
    else
        privileged pacman -S --needed -- "${missing[@]}" || die 'Package installation failed. Update your system first if the package database is out of date.'
    fi
fi

if ! command -v paru >/dev/null && ! command -v yay >/dev/null; then
    choice=
    if ((yes)); then
        choice=yay
    elif [[ -t 0 ]]; then
        read -r -p 'Community apps: yay (ready-made, quick) [default], paru (built from source, slower), or skip? [yay/paru/skip] ' choice || true
        case $choice in ''|yay|paru|skip) ;; *) die 'Choose yay, paru, or skip.' ;; esac
        [[ -n $choice ]] || choice=yay
    else
        choice=skip
    fi
    if [[ $choice != skip ]]; then
        helper_dir=$(mktemp -d "$work/aur.XXXXXXXX")
        if [[ $choice == yay ]]; then
            helper_url=https://aur.archlinux.org/yay-bin.git
        else
            helper_url=https://aur.archlinux.org/paru.git
        fi
        run git clone -- "$helper_url" "$helper_dir/source"
        printf 'Please review the community build recipe:\n'
        while IFS= read -r line || [[ -n $line ]]; do printf '%s\n' "$line"; done < "$helper_dir/source/PKGBUILD"
        if confirm "Build and install $choice?"; then
            (cd "$helper_dir/source" && run makepkg -si --noconfirm) || die 'The community helper could not be installed.'
        else
            printf 'Community apps stay disabled until a helper is installed.\n'
        fi
    else
        printf 'Community apps stay disabled until a helper is installed.\n'
    fi
fi

printf 'Building braurser.\n'
run cmake -S . -B build/install -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBRAURSER_HELPER_PATH="$prefix/libexec/braurser/braurser-pacman-helper" -DBRAURSER_LIBEXEC_DIR="$prefix/libexec/braurser"
run cmake --build build/install
if ((skip_tests)); then
    printf 'Tests were skipped.\n'
else
    run ctest --test-dir build/install --output-on-failure
fi
stage="$work/stage"
run env DESTDIR="$stage" cmake --install build/install
read_manifest

backup_dir="/var/lib/braurser/backups/$(date -u +%Y%m%dT%H%M%SZ)-$$"
for dir in /var/lib/braurser /var/log/braurser /var/lib/braurser/backups; do
    check_parents "$dir"
    if privileged test -L "$dir"; then die "A private folder is a link: $dir"; fi
done
privileged install -d -m 0700 -o root -g root -- /var/lib/braurser /var/log/braurser
install -m 0600 /dev/null "$work/new.tsv"

backup_conflict() {
    local path=$1 current=$2 backup=${old_backup[$1]:--}
    if [[ $current != "${old_hash[$path]:-}" ]]; then
        backup="$backup_dir/${path#/}"
        privileged install -d -m 0700 -o root -g root -- "${backup%/*}"
        privileged cp -a -- "$path" "$backup"
    fi
    printf '%s' "$backup"
}

changed=0
unchanged=0
declare -A seen=()
while IFS= read -r -d '' file; do
    path=${file#"$stage"}
    [[ ! -v seen["$path"] ]] || die "Duplicate staged file: $path"
    seen["$path"]=1
    allowed_path "$path" || die "Unexpected staged file: $path"
    [[ -f $file && ! -L $file ]] || die "Unexpected staged link: $path"
    check_parents "$path"
    hash=$(sha256sum -- "$file")
    hash=${hash%% *}
    mode=$(stat -c %a -- "$file")
    backup=${old_backup[$path]:--}
    if exists "$path"; then
        [[ -f $path && ! -L $path ]] || die "An unexpected file is at $path"
        current=$(checksum "$path")
        backup=$(backup_conflict "$path" "$current")
        if [[ $current == "$hash" && $(privileged stat -c '%a:%u:%g' "$path") == "$mode:0:0" ]]; then
            ((unchanged += 1))
            printf '%s\t%s\t%s\t%s\n' "$path" "$hash" "$mode" "$backup" >> "$work/new.tsv"
            continue
        fi
    fi
    privileged install -D -m "$mode" -o root -g root -- "$file" "$path"
    ((changed += 1))
    printf '%s\t%s\t%s\t%s\n' "$path" "$hash" "$mode" "$backup" >> "$work/new.tsv"
done < <(find "$stage" -type f -print0 | sort -z)
((changed + unchanged > 0)) || die 'The build did not produce installable files.'
declare -A cache_backups=() cache_hashes=()
for path in "$prefix/share/applications/mimeinfo.cache" "$prefix/share/icons/hicolor/icon-theme.cache"; do
    check_parents "$path"
    if exists "$path"; then
        [[ -f $path && ! -L $path ]] || die "An unexpected cache file is at $path"
        cache_hashes["$path"]=$(checksum "$path")
        cache_backups["$path"]=$(backup_conflict "$path" "${cache_hashes[$path]}")
    fi
done
if command -v update-desktop-database >/dev/null; then
    privileged update-desktop-database "$prefix/share/applications" || printf 'Desktop cache could not be refreshed.\n'
fi
if command -v gtk-update-icon-cache >/dev/null && [[ -f $prefix/share/icons/hicolor/index.theme ]]; then
    privileged gtk-update-icon-cache -q -t -f "$prefix/share/icons/hicolor" || printf 'Icon cache could not be refreshed.\n'
fi
for path in "$prefix/share/applications/mimeinfo.cache" "$prefix/share/icons/hicolor/icon-theme.cache"; do
    if exists "$path"; then
        [[ -f $path && ! -L $path ]] || die "An unexpected cache file is at $path"
        hash=$(checksum "$path")
        mode=$(privileged stat -c '%a' "$path")
        backup=${cache_backups[$path]:-${old_backup[$path]:--}}
        printf '%s\t%s\t%s\t%s\n' "$path" "$hash" "$mode" "$backup" >> "$work/new.tsv"
        seen["$path"]=1
        if [[ $hash != "${cache_hashes[$path]:-}" ]]; then ((changed += 1)); fi
    fi
done
for path in "${!old_hash[@]}"; do
    if [[ ! -v seen["$path"] ]]; then
        printf '%s\t%s\t%s\t%s\n' "$path" "${old_hash[$path]}" "${old_mode[$path]}" "${old_backup[$path]}" >> "$work/new.tsv"
    fi
done
privileged install -D -m 0600 -o root -g root -- "$work/new.tsv" "$manifest.tmp.$$"
privileged mv -f -- "$manifest.tmp.$$" "$manifest"
printf 'Installed %s files; %s already up to date.\n' "$changed" "$unchanged"

if confirm 'Check for updates when I sign in?'; then
    entry="$HOME/.config/autostart/braurser-notifier.desktop"
    marker="$HOME/.local/state/braurser/autostart-installed"
    source="$prefix/share/braurser/braurser-notifier.desktop"
    if [[ -e $entry || -L $entry || -e $marker || -L $marker ]]; then
        printf 'Kept your existing sign-in setting.\n'
    else
        install -d -m 0700 -- "$HOME/.config/autostart" "$HOME/.local/state/braurser"
        install -m 0600 -- "$source" "$entry"
        install -m 0600 /dev/null "$marker"
        source_hash=$(sha256sum -- "$entry")
        printf '%s\t%s\n' "$prefix" "${source_hash%% *}" > "$marker"
        printf 'Sign-in update checks enabled.\n'
    fi
fi
