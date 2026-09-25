#!/usr/bin/env bash
set -euo pipefail

if [[ ! -d /src ]]; then
    image=${1:-archlinux:base-devel}
    docker run --rm -v "$PWD":/src:ro "$image" bash /src/tests/e2e/container-transactions.sh
    exit
fi

if [[ ! -x /usr/local/bin/braurserd ]]; then
    pacman -Syu --noconfirm sudo jq
    useradd -m -s /bin/bash builder
    printf 'builder ALL=(ALL) NOPASSWD: ALL\n' | install -m 0440 /dev/stdin /etc/sudoers.d/builder
    mkdir -p /home/builder/work /home/builder/runtime
    tar -C /src --exclude=build --exclude=.git -cf - . | tar -C /home/builder/work -xf -
    chown -R builder:builder /home/builder/work /home/builder/runtime
    chmod 0700 /home/builder/runtime
    cd /home/builder/work
    runuser -u builder -- ./install.sh --yes --skip-tests
    printf 'builder:builder-test-password\n' | chpasswd
    printf 'builder ALL=(ALL) ALL\n' | install -m 0440 /dev/stdin /etc/sudoers.d/builder
    printf '#!/usr/bin/env bash\nprintf '\''builder-test-password\\n'\''\n' | install -m 0755 /dev/stdin /usr/local/libexec/braurser/braurser-askpass-gtk
fi
cd /home/builder/work
client=/home/builder/work/build/install/bin/braurser-rpc-client
test -x "$client"
runuser -u builder -- env HOME=/home/builder USER=builder XDG_RUNTIME_DIR=/home/builder/runtime BRAURSER_ELEVATION=sudo-askpass /usr/local/bin/braurserd > /home/builder/daemon.log 2>&1 &
daemon=$!
trap 'status=$?; kill "$daemon" 2>/dev/null || true; if ((status != 0)); then tail -n 60 /home/builder/daemon.log >&2 || true; tail -n 60 /home/builder/.local/state/braurser/daemon.log >&2 || true; fi' EXIT

rpc() {
    runuser -u builder -- env HOME=/home/builder USER=builder XDG_RUNTIME_DIR=/home/builder/runtime "$client" /home/builder/runtime/braurser/core.sock "$1" "$2" 600
}

ready=0
for attempt in $(seq 1 30); do
    if rpc system.status '{}' >/dev/null 2>&1; then ready=1; break; fi
    sleep 1
done
((ready))

response() {
    jq -c 'select(has("result") or has("error"))' | jq -s -e '.[0] | select(.error == null) | .result'
}

transaction() {
    runuser -u builder -- env HOME=/home/builder USER=builder XDG_RUNTIME_DIR=/home/builder/runtime BRAURSER_E2E_ACK_REVIEWS=1 "$client" /home/builder/runtime/braurser/core.sock --plan "$2" "$1" 3600
}

rpc catalog.refresh '{"sources":[]}' | response >/dev/null
rpc catalog.search '{"text":"tree","filters":{},"limit":20,"sort":"relevance"}' | response | jq -e '.packages | any(.package.name == "tree")'
transaction jobs.install '{"kind":"install","packageId":"tree"}'
pacman -Q tree
transaction jobs.remove '{"kind":"remove","packageId":"tree"}'
if pacman -Q tree; then exit 1; fi

for helper in yay paru; do
    if [[ $helper == paru ]]; then
        printf 'builder ALL=(ALL) NOPASSWD: ALL\n' | install -m 0440 /dev/stdin /etc/sudoers.d/builder
        runuser -u builder -- git clone https://aur.archlinux.org/paru.git /home/builder/paru
        (cd /home/builder/paru && runuser -u builder -- makepkg -si --noconfirm)
        printf 'builder ALL=(ALL) ALL\n' | install -m 0440 /dev/stdin /etc/sudoers.d/builder
    fi
    rpc settings.set "$(jq -cn --arg helper "$helper" '{changedFields:{helper:$helper}}')" | response >/dev/null
    transaction jobs.install '{"kind":"install","packageId":"aur/pfetch"}'
    pacman -Q pfetch
    transaction jobs.remove '{"kind":"remove","packageId":"aur/pfetch"}'
    if pacman -Q pfetch; then exit 1; fi
done

rpc updates.check '{"refresh":true}' | response >/dev/null
transaction jobs.upgradeSystem '{"kind":"update-system"}'
test -f /var/log/braurser/helper.log
for operation in 'op=A' 'op=C' 'op=B' 'op=D'; do
    test "$(awk -v needle="$operation" 'index($0, needle) {found=1} END {print found+0}' /var/log/braurser/helper.log)" = 1
done
before=$(wc -l < /var/log/braurser/helper.log)
if runuser -u builder -- env HOME=/home/builder SUDO_ASKPASS=/usr/local/libexec/braurser/braurser-askpass-gtk sudo -A -- /usr/local/libexec/braurser/braurser-pacman-helper --sync --refresh; then
    printf 'Standalone refresh was accepted.\n' >&2
    exit 1
fi
after=$(wc -l < /var/log/braurser/helper.log)
((after > before))
test "$(awk 'END {print (index($0, "status=64") > 0)}' /var/log/braurser/helper.log)" = 1
printf 'Official, community, update and refresh guard checks passed.\n'
