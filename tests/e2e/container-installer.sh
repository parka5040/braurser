#!/usr/bin/env bash
set -euo pipefail

if [[ ! -d /src ]]; then
    image=${1:-archlinux:base-devel}
    docker run --rm -v "$PWD":/src:ro "$image" bash /src/tests/e2e/container-installer.sh
    exit
fi

pacman -Syu --noconfirm sudo
useradd -m -s /bin/bash builder
printf 'builder ALL=(ALL) NOPASSWD: ALL\n' | install -m 0440 /dev/stdin /etc/sudoers.d/builder
mkdir -p /home/builder/work /usr/local/share/applications
tar -C /src --exclude=build --exclude=.git -cf - . | tar -C /home/builder/work -xf -
chown -R builder:builder /home/builder/work
printf 'original desktop file\n' > /usr/local/share/applications/braurser.desktop
original=$(sha256sum /usr/local/share/applications/braurser.desktop)
cd /home/builder/work
runuser -u builder -- ./install.sh --yes --skip-tests
for name in braurser braurserd braurser-gtk braurser-qt braurser-notifier; do
    test "$(stat -c '%U:%G:%a' "/usr/local/bin/$name")" = root:root:755
done
for name in braurser-elevate braurser-pacman-helper braurser-askpass-gtk braurser-askpass-qt; do
    test "$(stat -c '%U:%G:%a' "/usr/local/libexec/braurser/$name")" = root:root:755
done
for path in /usr/local/share/braurser/restart-rules.json /usr/local/share/braurser/copy/en.json /usr/local/share/braurser/braurser-notifier.desktop /usr/local/share/applications/braurser.desktop /usr/local/share/icons/hicolor/scalable/apps/braurser.svg /usr/local/share/icons/hicolor/symbolic/apps/braurser-symbolic.svg /usr/share/polkit-1/actions/org.braurser.pacman.policy; do
    test -f "$path"
    test "$(stat -c '%U:%G:%a' "$path")" = root:root:644
done
for size in 16 24 32 48 64 128 256; do
    test -f "/usr/local/share/icons/hicolor/${size}x${size}/apps/braurser.png"
done
test "$(stat -c '%U:%G:%a' /var/lib/braurser/install-manifest.tsv)" = root:root:600
test "$(stat -c '%U:%G:%a' /var/lib/braurser)" = root:root:700
test "$(stat -c '%U:%G:%a' /var/log/braurser)" = root:root:700
test "$(stat -c '%U:%G:%a' /home/builder/.config/autostart/braurser-notifier.desktop)" = builder:builder:600
second=$(runuser -u builder -- ./install.sh --yes --skip-tests)
[[ $second == *'Installed 0 files;'* ]]
runuser -u builder -- ./install.sh --uninstall --yes
test ! -e /var/lib/braurser/install-manifest.tsv
test ! -e /usr/local/bin/braurser
test ! -e /usr/local/libexec/braurser/braurser-pacman-helper
test ! -e /usr/share/polkit-1/actions/org.braurser.pacman.policy
test ! -e /usr/local/share/braurser/restart-rules.json
test ! -e /usr/local/share/icons/hicolor/scalable/apps/braurser.svg
test ! -e /home/builder/.config/autostart/braurser-notifier.desktop
test "$(sha256sum /usr/local/share/applications/braurser.desktop)" = "$original"
printf 'Installer, repeat install and uninstall passed.\n'
