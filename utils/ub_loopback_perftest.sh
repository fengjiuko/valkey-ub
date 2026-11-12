#!/usr/bin/env bash
# SDK SEND microbenchmarks on one host; no Valkey or system configuration changes.
set -euo pipefail
umask 077

if [[ ${1:-} == --help ]]; then
    echo 'Usage: bash ub_loopback_perftest.sh'
    echo 'Environment: UB_DEVICE=udmac0d1e2 UB_EID=1 UB_SERVER=141.61.17.202'
    echo '             UB_ROUNDS=5 UB_LAT_ITERS=100000 UB_BW_SECONDS=30 UB_PORT=21220'
    echo '             UB_PERF=urma_perftest UB_OUTPUT_PARENT=$HOME'
    echo 'Shared: SEND_PROFILE=baseline|demo SEND_SIZES="8 64 256 1024 4096"'
    echo '        SEND_OPS="send_lat send_bw" SEND_CHANNELS=1 SEND_TX_DEPTH=128 SEND_RX_DEPTH=512 SEND_CQ_MOD=100'
    echo '        SEND_ROUNDS=5 SEND_LAT_ITERS=100000 SEND_MODE=poll'
    exit 0
fi
[[ $# == 0 ]] || { echo 'Unexpected argument; use --help' >&2; exit 2; }
device=${UB_DEVICE:-udmac0d1e2}
eid=${UB_EID:-1}
server=${UB_SERVER:-141.61.17.202}
profile=${SEND_PROFILE:-baseline}
case $profile in
    baseline) default_rounds=5; default_iters=100000; default_seconds=30; default_sizes='8 64 256 1024 4096' ;;
    demo) default_rounds=1; default_iters=10000; default_seconds=10; default_sizes='8 256 4096' ;;
    *) echo 'SEND_PROFILE must be baseline or demo' >&2; exit 2 ;;
esac
rounds=${SEND_ROUNDS:-${UB_ROUNDS:-$default_rounds}}
iters=${SEND_LAT_ITERS:-${UB_LAT_ITERS:-$default_iters}}
seconds=${UB_BW_SECONDS:-$default_seconds}
channels=${SEND_CHANNELS:-1}
tx_depth=${SEND_TX_DEPTH:-128}
rx_depth=${SEND_RX_DEPTH:-512}
cq_mod=${SEND_CQ_MOD:-100}
[[ ${SEND_MODE:-poll} == poll ]] || { echo 'Only polling is supported by this duration collector; event tests need a separate iterations plan' >&2; exit 2; }
read -r -a sizes <<< "${SEND_SIZES:-$default_sizes}"
read -r -a selected_ops <<< "${SEND_OPS:-send_lat send_bw}"
for op in "${selected_ops[@]}"; do [[ $op == send_lat || $op == send_bw ]] || exit 2; done
for size in "${sizes[@]}"; do
    [[ $size =~ ^[1-9][0-9]{0,4}$ ]] && (( size <= 4096 )) || {
        echo 'CTP SEND sizes must be 1..4096 bytes; 64 KiB needs separately validated TP mode' >&2; exit 2;
    }
done
(( ${#sizes[@]} > 0 && ${#selected_ops[@]} > 0 )) || exit 2
port=${UB_PORT:-21220}
for number in "$eid" "$rounds" "$iters" "$seconds" "$port" "$channels" "$tx_depth" "$rx_depth" "$cq_mod"; do
    [[ $number =~ ^(0|[1-9][0-9]{0,8})$ ]] || { echo 'Numeric settings must be decimal integers' >&2; exit 2; }
done
(( rounds >= 1 && rounds <= 20 && iters >= 10000 && seconds >= 5 && seconds <= 60 && port >= 1024 && port <= 65000 )) || {
    echo 'Invalid range: rounds 1..20, iterations >=10000, bandwidth seconds 5..60, port 1024..65000' >&2
    exit 2
}
(( channels >= 1 && channels <= 64 && tx_depth >= 1 && tx_depth <= 4096 &&
   rx_depth >= 2 && rx_depth <= 4096 && rx_depth % 2 == 0 && cq_mod >= 1 &&
   cq_mod <= tx_depth && cq_mod <= rx_depth && cq_mod <= 1024 )) || {
    echo 'Invalid channels/depth/CQ moderation (CQ moderation must fit both depths; RX depth must be even)' >&2; exit 2;
}
# Duplicate cases would overwrite logs; reject duplicate dimensions before starting tools.
[[ $(printf '%s\n' "${sizes[@]}" | sort -u | wc -l) == "${#sizes[@]}" &&
   $(printf '%s\n' "${selected_ops[@]}" | sort -u | wc -l) == "${#selected_ops[@]}" ]] || exit 2
reporter="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/send_perftest_report.py"
[[ -r $reporter ]] || { echo 'Place send_perftest_report.py beside this script' >&2; exit 2; }
for tool in timeout stdbuf sha256sum tar python3; do command -v "$tool" >/dev/null; done
perf=$(command -v "${UB_PERF:-urma_perftest}")
out=$(mktemp -d "${UB_OUTPUT_PARENT:-$HOME}/ub-loopback-XXXXXXXX")
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
{
    date -Iseconds; hostname; id; uname -a
    ulimit -l || true
    printf 'device=%s eid=%s server=%s rounds=%s lat_iters=%s bw_seconds=%s port=%s\n' \
        "$device" "$eid" "$server" "$rounds" "$iters" "$seconds" "$port"
    command -v lscpu >/dev/null && lscpu || true
    command -v ip >/dev/null && ip -br address || true
    command -v rpm >/dev/null && rpm -qf "$perf" /usr/lib64/liburma.so.0 /usr/lib64/liburma_common.so.0 || true
} > "$out/environment.log" 2>&1
sha256sum "$perf" "$0" > "$out/tool.sha256"
sha256sum "$reporter" >> "$out/tool.sha256"
cp "$reporter" "$out/send_perftest_report.py"
printf 'backend\tUB\nvariant\tRM/CTP\nprofile\t%s\ntopology\tloopback\ncompletion\tpoll\nchannels_bw\t%s\ntx_depth_bw\t%s\nrx_depth\t%s\ncq_mod_bw\t%s\ninline\t0\nrounds\t%s\n' \
    "$profile" "$channels" "$tx_depth" "$rx_depth" "$cq_mod" "$rounds" > "$out/settings.tsv"
cat "$out/settings.tsv"
printf 'case\tphase\top\tbytes\tround\n' > "$out/plan.tsv"
for (( round=1; round<=rounds; round++ )); do
    for size in "${sizes[@]}"; do
        for op in "${selected_ops[@]}"; do
            printf 'measured-%s-%s-r%s\tmeasured\t%s\t%s\t%s\n' "$op" "$size" "$round" "$op" "$size" "$round" >> "$out/plan.tsv"
        done
    done
done
for op in send_lat send_bw; do
    timeout 10s "$perf" "$op" -h > "$out/$op-help.log" 2>&1 || true
    for option in --ctp --eid_idx --priority --share_jfr --mgmt_proto --trans_mode --port --size --iters; do
        grep -q -- "$option" "$out/$op-help.log" || {
            echo "Missing help option $option for $op; inspect $out/$op-help.log" >&2; exit 1;
        }
    done
done
grep -q -- --duration "$out/send_bw-help.log"
for option in --jettys --jfs_depth --jfr_depth --jfc_depth --inline_size --cq_mod; do
    grep -q -- "$option" "$out/send_bw-help.log" || { echo "Missing $option in bandwidth help" >&2; exit 1; }
done
printf 'case\tphase\top\tbytes\tround\tserver_exit\tclient_exit\n' > "$out/results.tsv"

pair() {
    local phase=$1 op=$2 size=$3 round=$4 value=$5
    local tag="${phase}-${op}-${size}-r${round}" sr=0 cr=0 ready=0
    local args=("$op" -d "$device" --eid_idx "$eid" --ctp -O 1 --share_jfr true
                --mgmt_proto tcp -p 0 -P "$port" -s "$size" -I 0 -R "$rx_depth")
    if [[ $op == send_lat ]]; then
        args+=(-n "$value" -J 1 -T 1 -C 512)
    else
        args+=(-D "$value" -J "$channels" -T "$tx_depth" -C 4096 -Q "$cq_mod")
    fi
    {
        date -Iseconds
        printf 'server: '; printf '%q ' "$perf" "${args[@]}"; printf '\n'
        printf 'client: '; printf '%q ' "$perf" "${args[@]}" -S "$server"; printf '\n'
    } > "$out/$tag-command.txt"
    echo "RUN $tag"
    timeout --signal=TERM --kill-after=5s 120s stdbuf -oL -eL "$perf" "${args[@]}" \
        > "$out/$tag-server.log" 2>&1 &
    server_pid=$!
    # Read the tool's readiness message; never probe its control socket with a connection.
    for (( poll=0; poll<200; poll++ )); do
        if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
        if grep -q 'Waiting for client to connect' "$out/$tag-server.log"; then ready=1; break; fi
        sleep 0.1
    done
    if (( ready == 0 )); then
        echo "Server did not become ready: $out/$tag-server.log" >&2
        return 1
    fi
    timeout --signal=TERM --kill-after=5s 120s stdbuf -oL -eL "$perf" "${args[@]}" -S "$server" \
        > "$out/$tag-client.log" 2>&1 &
    client_pid=$!
    wait "$client_pid" || cr=$?
    client_pid=
    wait "$server_pid" || sr=$?
    server_pid=
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tag" "$phase" "$op" "$size" "$round" "$sr" "$cr" >> "$out/results.tsv"
    printf '%s\n' "$sr" > "$out/$tag-server.exit"
    printf '%s\n' "$cr" > "$out/$tag-client.exit"
    echo "RESULT $tag server_exit=$sr client_exit=$cr"
    if [[ $phase == measured ]]; then
        grep -E 'bytes.*iterations|^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]' "$out/$tag-client.log" || true
    fi
    if (( sr != 0 || cr != 0 )); then
        tail -n 12 "$out/$tag-server.log" "$out/$tag-client.log" >&2
        echo "Failed: $tag server=$sr client=$cr; inspect both logs" >&2; return 1
    fi
    grep -Eq "^[[:space:]]*$size[[:space:]]+[0-9]+[[:space:]]+[0-9]" "$out/$tag-client.log" || {
        echo "No result row in $out/$tag-client.log" >&2; return 1;
    }
    # Each pair gets its own control port so a failed/stale peer cannot consume a later case.
    port=$((port + 1))
}

# Discarded setup runs are separate processes, not an in-process steady-state warmup.
for size in "${sizes[@]}"; do
    for op in "${selected_ops[@]}"; do
        if [[ $op == send_lat ]]; then pair warmup "$op" "$size" 0 10000; else pair warmup "$op" "$size" 0 5; fi
    done
done
for (( round=1; round<=rounds; round++ )); do
    ops=("${selected_ops[@]}")
    if (( round % 2 == 0 && ${#ops[@]} == 2 )); then ops=("${selected_ops[1]}" "${selected_ops[0]}"); fi
    for size in "${sizes[@]}"; do
        for op in "${ops[@]}"; do
            if [[ $op == send_lat ]]; then value=$iters; else value=$seconds; fi
            pair measured "$op" "$size" "$round" "$value"
        done
    done
done
printf 'All %s measured pairs exited 0 and produced a client result row.\n' "$((rounds * ${#sizes[@]} * ${#selected_ops[@]}))" > "$out/COMPLETE"
echo "COMPLETE: $(cat "$out/COMPLETE")"
