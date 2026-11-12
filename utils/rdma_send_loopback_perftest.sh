#!/usr/bin/env bash
# Verbs SEND component measurements on one host, for comparison with URMA SEND.
set -euo pipefail
umask 077
if [[ ${1:-} == --help ]]; then
    echo 'Usage: RDMA_GID=<local RoCE v2 GID index> bash rdma_send_loopback_perftest.sh'
    echo 'Environment: RDMA_DEVICE=rocep40s0f0 RDMA_IB_PORT=1 RDMA_SERVER=192.168.17.202'
    echo '             RDMA_ROUNDS=5 RDMA_PORT=21420 RDMA_OUTPUT_PARENT=$HOME'
    echo 'Fixed: 2/64/256/1024/4096 B; latency n=100000; bandwidth D=29, margin=7.'
    echo 'Place send_perftest_report.py beside this script for automatic terminal summaries.'
    exit 0
fi
[[ $# == 0 ]] || { echo 'Unexpected argument; use --help' >&2; exit 2; }
: "${RDMA_GID:?Set RDMA_GID to the local IPv4 RoCE v2 GID index first}"
device=${RDMA_DEVICE:-rocep40s0f0}
ib_port=${RDMA_IB_PORT:-1}
server=${RDMA_SERVER:-192.168.17.202}
gid=$RDMA_GID
rounds=${RDMA_ROUNDS:-5}
port=${RDMA_PORT:-21420}
[[ $device =~ ^[a-zA-Z0-9_.-]+$ ]] || exit 2
for number in "$ib_port" "$gid" "$rounds" "$port"; do
    [[ $number =~ ^(0|[1-9][0-9]{0,4})$ ]] || { echo 'Invalid integer setting' >&2; exit 2; }
done
(( ib_port >= 1 && rounds >= 1 && rounds <= 20 && port >= 1024 && port <= 65000 )) || exit 2
[[ $server =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || { echo 'RDMA_SERVER must be a local IPv4 address' >&2; exit 2; }
IFS=. read -r ip_a ip_b ip_c ip_d <<< "$server"
for octet in "$ip_a" "$ip_b" "$ip_c" "$ip_d"; do (( 10#$octet <= 255 )) || exit 2; done
printf -v expected_gid '0000:0000:0000:0000:0000:ffff:%02x%02x:%02x%02x' \
    "$((10#$ip_a))" "$((10#$ip_b))" "$((10#$ip_c))" "$((10#$ip_d))"
reporter="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/send_perftest_report.py"
[[ -r $reporter ]] || { echo 'Place send_perftest_report.py beside this script' >&2; exit 2; }
for tool in timeout stdbuf sha256sum tar ip python3; do command -v "$tool" >/dev/null; done
lat=$(command -v ib_send_lat)
bw=$(command -v ib_send_bw)
out=$(mktemp -d "${RDMA_OUTPUT_PARENT:-$HOME}/rdma-send-loopback-XXXXXXXX")
echo "RUN=$out"
server_pid=
client_pid=
finish() {
    local rc=$?
    trap - EXIT INT TERM
    for child in "$client_pid" "$server_pid"; do
        if [[ -n $child ]]; then
            kill -TERM "$child" 2>/dev/null || true
            wait "$child" 2>/dev/null || true
        fi
    done
    if (( rc != 0 )); then printf 'runner_exit=%s\n' "$rc" > "$out/FAILED.txt"; fi
    if ! python3 "$reporter" --write "$out"; then echo 'Terminal summary failed; original logs are preserved' >&2; fi
    if tar -czf "$out.tar.gz" -C "$(dirname "$out")" "$(basename "$out")"; then
        echo "ARCHIVE=$out.tar.gz"
    else
        echo "Archive failed; evidence remains at $out" >&2
        rc=1
    fi
    exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# Override is used by local synthetic checks only; always archive the actual root.
sysfs_root=${RDMA_SYSFS_ROOT:-/sys/class/infiniband}
dev_port="$sysfs_root/$device/ports/$ib_port"
{
    date -Iseconds; hostname; id; uname -a; ulimit -l || true
    printf 'device=%s ib_port=%s gid=%s server=%s rounds=%s sysfs_root=%s\n' \
        "$device" "$ib_port" "$gid" "$server" "$rounds" "$sysfs_root"
    command -v lscpu >/dev/null && lscpu || true
    ip -br address
    command -v ibv_devinfo >/dev/null && ibv_devinfo -d "$device" -v || true
    command -v rpm >/dev/null && rpm -qf "$lat" "$bw" || true
    for field in state link_layer rate active_mtu; do
        if [[ -r $dev_port/$field ]]; then printf '%s=' "$field"; cat "$dev_port/$field"; fi
    done
} > "$out/environment.log" 2>&1
sha256sum "$lat" "$bw" "$0" > "$out/tool.sha256"
sha256sum "$reporter" >> "$out/tool.sha256"
cp "$reporter" "$out/send_perftest_report.py"
printf 'backend\tRDMA\nvariant\tverbs/RC\nprofile\tbaseline\ntopology\tloopback\ncompletion\tpoll\nchannels_bw\t1\ntx_depth_bw\t128\nrx_depth\t512\ncq_mod_bw\t100\ninline\t0\nrounds\t%s\n' \
    "$rounds" > "$out/settings.tsv"
cat "$out/settings.tsv"
printf 'case\tphase\top\tbytes\tround\n' > "$out/plan.tsv"
for (( round=1; round<=rounds; round++ )); do
    for size in 2 64 256 1024 4096; do
        for op in send_lat send_bw; do
            printf 'measured-%s-%s-r%s\tmeasured\t%s\t%s\t%s\n' "$op" "$size" "$round" "$op" "$size" "$round" >> "$out/plan.tsv"
        done
    done
done
grep -q ACTIVE "$dev_port/state"
grep -qx Ethernet "$dev_port/link_layer"
actual_gid=$(cat "$dev_port/gids/$gid")
gid_type=$(cat "$dev_port/gid_attrs/types/$gid")
netdev=$(cat "$dev_port/gid_attrs/ndevs/$gid")
printf 'gid=%s\ntype=%s\nnetdev=%s\n' "$actual_gid" "$gid_type" "$netdev" > "$out/selected-gid.txt"
[[ ${actual_gid,,} == "$expected_gid" && $gid_type == 'RoCE v2' ]] || {
    echo "GID does not match local IPv4 RoCE v2; inspect $out/selected-gid.txt" >&2; exit 1;
}
ip -4 -o address show dev "$netdev" > "$out/netdev-ipv4.txt"
awk -v ip="$server" '{split($4,a,"/"); if(a[1]==ip) found=1} END {exit !found}' "$out/netdev-ipv4.txt" || {
    echo 'RDMA_SERVER is not assigned to the selected GID interface' >&2; exit 1;
}
for tool in "$lat" "$bw"; do
    name=$(basename "$tool")
    timeout 10s "$tool" --help > "$out/$name-help.log" 2>&1 || true
    for option in --ib-dev --ib-port --gid-index --connection --inline_size --rx-depth --tx-depth --size --iters --port; do
        grep -q -- "$option" "$out/$name-help.log" || { echo "Missing $option in $name help" >&2; exit 1; }
    done
done
for option in --duration --margin --cq-mod --qp --noPeak; do
    grep -q -- "$option" "$out/ib_send_bw-help.log" || { echo "Missing $option in bandwidth help" >&2; exit 1; }
done
printf 'case\tphase\top\tbytes\tround\tserver_exit\tclient_exit\n' > "$out/results.tsv"
pair() {
    local phase=$1 op=$2 size=$3 round=$4 sr=0 cr=0 ready=0 tool
    local tag="${phase}-${op}-${size}-r${round}"
    local args=(-d "$device" -i "$ib_port" -x "$gid" -c RC -I 0 -r 512 -s "$size" -p "$port")
    if [[ $op == send_lat ]]; then
        tool=$lat
        if [[ $phase == warmup ]]; then args+=(-t 1 -n 10000); else args+=(-t 1 -n 100000); fi
    else
        tool=$bw
        args+=(-q 1 -t 128 -Q 100 -N)
        if [[ $phase == warmup ]]; then args+=(-n 10000); else args+=(-D 29 -f 7); fi
    fi
    {
        date -Iseconds
        printf 'server: '; printf '%q ' "$tool" "${args[@]}"; printf '\n'
        printf 'client: '; printf '%q ' "$tool" "${args[@]}" "$server"; printf '\n'
    } > "$out/$tag-command.txt"
    echo "RUN $tag"
    timeout --signal=TERM --kill-after=5s 120s stdbuf -oL -eL "$tool" "${args[@]}" > "$out/$tag-server.log" 2>&1 &
    server_pid=$!
    for (( poll=0; poll<200; poll++ )); do
        if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
        if grep -q 'Waiting for client to connect' "$out/$tag-server.log"; then ready=1; break; fi
        sleep 0.1
    done
    if (( ready == 0 )); then echo "Server not ready; inspect $out/$tag-server.log" >&2; return 1; fi
    timeout --signal=TERM --kill-after=5s 120s stdbuf -oL -eL "$tool" "${args[@]}" "$server" > "$out/$tag-client.log" 2>&1 &
    client_pid=$!
    wait "$client_pid" || cr=$?
    client_pid=
    wait "$server_pid" || sr=$?
    server_pid=
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tag" "$phase" "$op" "$size" "$round" "$sr" "$cr" >> "$out/results.tsv"
    printf '%s\n' "$sr" > "$out/$tag-server.exit"
    printf '%s\n' "$cr" > "$out/$tag-client.exit"
    date -Iseconds > "$out/$tag-end.txt"
    echo "RESULT $tag server_exit=$sr client_exit=$cr"
    if [[ $phase == measured ]]; then
        grep -E 'iterations.*(t_min|BW)|^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]' "$out/$tag-client.log" || true
    fi
    if (( sr != 0 || cr != 0 )); then
        tail -n 12 "$out/$tag-server.log" "$out/$tag-client.log" >&2
        echo "Failed: $tag server=$sr client=$cr" >&2; return 1
    fi
    grep -Eq "^[[:space:]]*$size[[:space:]]+[0-9]+[[:space:]]+[0-9]" "$out/$tag-client.log" || {
        echo "No result row in $out/$tag-client.log" >&2; return 1;
    }
    port=$((port + 1))
}
for size in 2 64 256 1024 4096; do
    pair warmup send_lat "$size" 0
    pair warmup send_bw "$size" 0
done
for (( round=1; round<=rounds; round++ )); do
    if (( round % 2 )); then ops=(send_lat send_bw); else ops=(send_bw send_lat); fi
    for size in 2 64 256 1024 4096; do
        for op in "${ops[@]}"; do pair measured "$op" "$size" "$round"; done
    done
done
printf 'All %s measured pairs exited 0 and produced a client result row.\n' "$((rounds * 10))" > "$out/COMPLETE"
echo "COMPLETE: $(cat "$out/COMPLETE")"
