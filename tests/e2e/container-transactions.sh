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
trap 'status=$?; kill "$daemon" 2>/dev/null || true; if ((status != 0)); then tail -n 60 /home/builder/daemon.log >&2 || true; grep -v " info catalog " /home/builder/.local/state/braurser/daemon.log | tail -n 60 >&2 || true; fi' EXIT

rpc() {
    runuser -u builder -- env HOME=/home/builder USER=builder XDG_RUNTIME_DIR=/home/builder/runtime "$client" /home/builder/runtime/braurser/core.sock "$1" "$2" 600
}

ready=0
for _ in $(seq 1 30); do
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

refreshed=0
for _ in $(seq 1 120); do
    if rpc catalog.refresh '{"sources":[]}' | response >/dev/null; then refreshed=1; break; fi
    sleep 2
done
((refreshed))
rpc catalog.search '{"text":"tree","filters":{},"limit":20,"sort":"relevance"}' | response | jq -e '.packages | any(.package.name == "tree")'
transaction jobs.install '{"kind":"install","packageId":"tree"}'
pacman -Q tree
transaction jobs.remove '{"kind":"remove","packageId":"tree"}'
if pacman -Q tree; then exit 1; fi

split_selection='{"kind":"install","packageId":"aur/gtksourceview5-pkgbuild"}'
split_plan=$(rpc jobs.plan "$split_selection" | response)
jq -e '.build_pins | length == 1 and .[0].pkgbase == "gtksourceview-pkgbuild" and
    (.[0].pkgnames | index("gtksourceview5-pkgbuild") != null and index("mime-pkgbuild") != null) and
    (.[0].commit | test("^([0-9a-fA-F]{40}|[0-9a-fA-F]{64})$"))' <<< "$split_plan"
commit=$(jq -er '.build_pins[0].commit' <<< "$split_plan")
rpc catalog.buildFiles "$(jq -cn --arg commit "$commit" '{packageId:"aur/gtksourceview5-pkgbuild",commit:$commit}')" |
    response | jq -e '.packageBase == "gtksourceview-pkgbuild" and
        (.files | any(.name == "PKGBUILD")) and (.files | any(.name == ".SRCINFO"))'
split_run=$(transaction jobs.install "$split_selection")
jq -se --argjson pins "$(jq -c '.build_pins' <<< "$split_plan")" '
    any(.[]; .id == "rpc-client-plan" and .result.build_pins == $pins) and
    any(.[]; .method == "job.state" and .params.state == "succeeded")' <<< "$split_run"
pacman -Q gtksourceview5-pkgbuild mime-pkgbuild gtksourceview5
LC_ALL=C pacman -Qi mime-pkgbuild | awk -F ': *' '$1 ~ /^Install Reason *$/ {found=($2 == "Installed as a dependency for another package")} END {exit !found}'
split_job=$(jq -ser '[.[] | select(.id == "rpc-client-action") | .result.jobId] | select(length == 1) | .[0]' <<< "$split_run")
[[ $split_job =~ ^[a-zA-Z0-9-]{1,128}$ ]]
worktree=/home/builder/.cache/braurser/build/manual/$split_job
bare=/home/builder/.cache/braurser/manual/aur-gtksourceview-pkgbuild/repo.git
test ! -e "$worktree"
pinned=$(runuser -u builder -- git -C "$bare" show "$commit:.SRCINFO" | awk -F ' = ' '$1 ~ /^[[:space:]]*pkgver$/ {ver=$2} $1 ~ /^[[:space:]]*pkgrel$/ {rel=$2} END {print ver "-" rel}')
test "$(pacman -Q gtksourceview5-pkgbuild | awk '{print $2}')" = "$pinned"
test "$(pacman -Q mime-pkgbuild | awk '{print $2}')" = "$pinned"
transaction jobs.remove '{"kind":"remove","packageId":"aur/gtksourceview5-pkgbuild"}'
if pacman -Q gtksourceview5-pkgbuild || pacman -Q mime-pkgbuild; then exit 1; fi

closure_run=$(transaction jobs.install '{"kind":"install","packageId":"aur/concordance-cli"}')
jq -se 'any(.[]; .id == "rpc-client-plan" and
    (.result.build_pins | length == 2 and .[0].pkgbase == "libconcord" and .[1].pkgbase == "concordance-cli")) and
    any(.[]; .method == "job.state" and .params.state == "succeeded")' <<< "$closure_run"
pacman -Q concordance-cli libconcord
LC_ALL=C pacman -Qi libconcord | awk -F ': *' '$1 ~ /^Install Reason *$/ {found=($2 == "Installed as a dependency for another package")} END {exit !found}'
transaction jobs.remove '{"kind":"remove","packageId":"aur/concordance-cli"}'
if pacman -Q concordance-cli || pacman -Q libconcord; then exit 1; fi

check_job=$(rpc updates.check '{"refresh":true}' | response | jq -er '.jobId')
[[ $check_job =~ ^[a-zA-Z0-9-]{1,128}$ ]]
check_db=/home/builder/.cache/braurser/syncdb-check/report-$check_job
expected=$(runuser -u builder -- env LC_ALL=C pacman --sync --sysupgrade --print --print-format '%r/%n %v' --dbpath "$check_db" | sha256sum)
expected=${expected%% *}
[[ $expected =~ ^[a-f0-9]{64}$ ]]
test -f /var/log/braurser/helper.log
before=$(wc -l < /var/log/braurser/helper.log)
update_run=$(transaction jobs.upgradeSystem '{"kind":"update-system"}')
jq -se 'any(.[]; .method == "job.state" and .params.state == "succeeded")' <<< "$update_run"
awk -v before="$before" -v digest="$expected" 'NR > before {if (index($0, "transaction=" digest)) found=1; if (index($0, "stale")) stale=1} END {exit !(found && !stale)}' /var/log/braurser/helper.log

sync_before=$(sha256sum /var/lib/pacman/sync/*.db)
before=$(wc -l < /var/log/braurser/helper.log)
stale_digest=$(printf '%064d' 0)
stale_status=0
runuser -u builder -- env HOME=/home/builder SUDO_ASKPASS=/usr/local/libexec/braurser/braurser-askpass-gtk sudo -A -- /usr/local/libexec/braurser/braurser-pacman-helper --sync --refresh --sysupgrade --noconfirm "--braurser-transaction=$stale_digest" || stale_status=$?
test "$stale_status" = 75
test "$(sha256sum /var/lib/pacman/sync/*.db)" = "$sync_before"
awk -v before="$before" -v digest="$stale_digest" 'NR > before && index($0, "transaction=" digest) && index($0, "stale") && index($0, "status=75") {found=1} END {exit !found}' /var/log/braurser/helper.log

for operation in 'op=A' 'op=C' 'op=B' 'op=D' 'op=E'; do
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
printf 'Official, split-package, AUR closure, update, stale transaction and refresh guard checks passed.\n'
