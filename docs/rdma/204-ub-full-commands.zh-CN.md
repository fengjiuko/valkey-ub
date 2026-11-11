# 204 原生 UB：从盘点到 Valkey 功能的完整命令

日期：2026-11-11。下面是待现场执行的条件流程，不表示这些阶段已经通过。
使用 204 的两个 root 终端，终端 A 保留变量，终端 B 通过本轮 `run.env` 加载同一配置。
只需按盘点结果填写本机设备名和 EID index，之后成功就继续、失败就到第 8 节采日志，
无需每个成功阶段都等待新的命令。所有源码和证据目录使用绝对路径。

## 1. 终端 A：更新工具、建立本轮目录、盘点

先由仓库所有者更新工具（可从 root 调用），不强制覆盖本地修改：

```bash
runuser -u h30086230 -- git -C /home/h30086230/valkey-ub status --short
runuser -u h30086230 -- git -C /home/h30086230/valkey-ub fetch origin work/rdma-perf-baseline
runuser -u h30086230 -- git -C /home/h30086230/valkey-ub switch work/rdma-perf-baseline
runuser -u h30086230 -- git -C /home/h30086230/valkey-ub pull --ff-only origin work/rdma-perf-baseline
```

下面开始仅操作本轮新目录；构建/检查失败时先处理该阶段，不继续后面的命令：

```bash
set -euo pipefail
set +x
umask 077
UB_REPO=/home/h30086230/valkey-ub
mkdir -p /home/h30086230/valkey-validation
UB_RUN=$(mktemp -d /home/h30086230/valkey-validation/ub204-full.XXXXXX)
printf 'UB_RUN=%s\n' "$UB_RUN"
date -Iseconds > "$UB_RUN/run-start.txt"
runuser -u h30086230 -- git -C "$UB_REPO" rev-parse HEAD > "$UB_RUN/tool-sha.txt"
{
  hostname; id; date -Iseconds; uname -a; ulimit -l
  rpm -qa | grep -Ei 'umdk|urma|ubcore|udma' || true
} > "$UB_RUN/environment.log" 2>&1
cat "$UB_RUN/environment.log"
make -C "$UB_REPO/tests/urma" OUT="$UB_RUN" \
  "$UB_RUN/sdk-probe" "$UB_RUN/order-probe" 2>&1 | tee "$UB_RUN/probe-build.log"
ldd "$UB_RUN/sdk-probe" | tee "$UB_RUN/probe-libraries.log"
set +e
timeout 30s "$UB_RUN/sdk-probe" --inventory 2>&1 | tee "$UB_RUN/inventory.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/inventory.exit"
test "$rc" -eq 0
```

**继续条件：** init/query/uninit 成功，有 `transport_type=0` 的原生 UB 设备及可用 EID。
此前 202 的设备名称不能代替 204 本机盘点。

## 2. 终端 A：选设备、创建/清理本地资源

将下面两个占位值改为本机盘点值。`UB_EID` 填数字索引，不填 EID 的十六进制内容：

```bash
UB_DEVICE='填写204实际原生UB设备名'
UB_EID='填写该设备实际EID索引'
UB_PERF=$(command -v urma_perftest)
test -n "$UB_PERF"
{
  printf 'UB_REPO=%q\n' "$UB_REPO"
  printf 'UB_RUN=%q\n' "$UB_RUN"
  printf 'UB_DEVICE=%q\n' "$UB_DEVICE"
  printf 'UB_EID=%q\n' "$UB_EID"
  printf 'UB_PERF=%q\n' "$UB_PERF"
} > "$UB_RUN/run.env"
set +e
timeout 30s "$UB_RUN/order-probe" "$UB_DEVICE" "$UB_EID" 2>&1 | tee "$UB_RUN/order.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/order.exit"
test "$rc" -eq 0
```

**继续条件：** DEFAULT、OT 都显示 resources=created，删除资源/context/uninit 均为 0。
这仍未证明建链和收发。`rc_order=0` 原样记录，不能把它改成支持 OT。

检查 204 本机 perftest 的选项；`-h` 的退出状态不作硬件验收依据：

```bash
set +e
"$UB_PERF" send_lat -h > "$UB_RUN/perftest-help.log" 2>&1
set -e
cat "$UB_RUN/perftest-help.log"
for option in --mgmt_proto --order_type --share_jfr --eid_idx; do
  grep -q -- "$option" "$UB_RUN/perftest-help.log"
done
sha256sum "$UB_PERF" > "$UB_RUN/perftest.sha256"
rpm -qf "$UB_PERF" > "$UB_RUN/perftest-package.txt"
```

下面只适用于本机帮助支持这些参数、且与记录的 B138 语义一致的工具：TCP 管理模式
使用 IP 和 TCP 端口；`-p 1` 是 RC，`--order_type 1` 是 OT。

## 3. 两终端：SDK 同机 RC/OT 收发

终端 A 先执行服务端，保持运行。端口 21204 应为空闲测试端口：

```bash
date -Iseconds > "$UB_RUN/sdk-lat-start.txt"
set +e
URMA_LOG_LEVEL=debug timeout 120s "$UB_PERF" send_lat \
  --mgmt_proto tcp -d "$UB_DEVICE" --eid_idx "$UB_EID" \
  -p 1 --order_type 1 --share_jfr true -O 0 \
  -P 21204 -s 256 -n 1000 2>&1 | tee "$UB_RUN/sdk-lat-server.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/sdk-lat-server.exit"
printf 'SDK server exit=%s\n' "$rc"
```

终端 B 将路径替换为第 1 节实际打印的 UB_RUN，再加载本轮配置。等待终端 A 显示等待
连接后尽快执行：

```bash
set -euo pipefail
UB_RUN=/home/h30086230/valkey-validation/ub204-full.填写实际后缀
source "$UB_RUN/run.env"
set +x
set +e
URMA_LOG_LEVEL=debug timeout 120s "$UB_PERF" send_lat \
  --mgmt_proto tcp -d "$UB_DEVICE" --eid_idx "$UB_EID" \
  -p 1 --order_type 1 --share_jfr true -O 0 \
  -P 21204 -S 127.0.0.1 -s 256 -n 1000 2>&1 | tee "$UB_RUN/sdk-lat-client.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/sdk-lat-client.exit"
printf 'SDK client exit=%s\n' "$rc"
```

终端 A、B 都完成后，在终端 A 检查：

```bash
test "$(cat "$UB_RUN/sdk-lat-server.exit")" = 0
test "$(cat "$UB_RUN/sdk-lat-client.exit")" = 0
```

**继续条件：** 两端正常完成、配置一致、退出码均为 0，无 import/bind/CQE 错误或超时。
若出现 202 已见的 Share TP 拒绝或 TP 查询超时，执行第 8 节采日志，本轮不进入 Valkey。

## 4. 可选：SDK 多请求收发检查

可在 SDK send_lat 通过后增加 send_bw，作为更多在途请求的 SDK 检查；它不等于
Valkey 性能结果。终端 A：

```bash
set +e
timeout 120s "$UB_PERF" send_bw \
  --mgmt_proto tcp -d "$UB_DEVICE" --eid_idx "$UB_EID" \
  -p 1 --order_type 1 --share_jfr true -O 0 \
  -P 21205 -s 256 -n 10000 2>&1 | tee "$UB_RUN/sdk-bw-server.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/sdk-bw-server.exit"
```

终端 B：

```bash
set +e
timeout 120s "$UB_PERF" send_bw \
  --mgmt_proto tcp -d "$UB_DEVICE" --eid_idx "$UB_EID" \
  -p 1 --order_type 1 --share_jfr true -O 0 \
  -P 21205 -S 127.0.0.1 -s 256 -n 10000 2>&1 | tee "$UB_RUN/sdk-bw-client.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/sdk-bw-client.exit"
```

两端检查退出码，并保留实际配置输出。若该可选检查失败，也先定位失败而不将 SDK
准入标成完整通过。严格的顺序和内容正确性仍需后续功能验证。

## 5. 终端 A：Valkey 能力门禁和独立构建

进入本阶段前，确认 SDK 收发已通过，平台已确认当前显式 OT 的实际顺序/关闭契约。
当前适配器额外要求设备报告 OT；下面检查所选设备的 order-probe 输出：

```bash
grep -qE '(^| )ot=1( |$)' "$UB_RUN/order.log"
```

**若该检查失败，当前 Valkey 适配器不能继续。** 此时能报告的是 SDK 诊断/收发结果；
需要核对版本能力报告兼容方案，没有可直接绕过此条件的已实现测试命令。

门禁满足后，固定原生代码基线，在本轮目录构建，不改变原有 RDMA 二进制：

```bash
UB_SHA=aff2d3350e57e1c222d28743b486a940b0ac992c
mkdir "$UB_RUN/source"
printf '%s\n' "$UB_SHA" > "$UB_RUN/source-sha.txt"
runuser -u h30086230 -- git -C "$UB_REPO" archive "$UB_SHA" | tar -x -C "$UB_RUN/source"
make -C "$UB_RUN/source" -j4 BUILD_RDMA=no BUILD_URMA=yes MALLOC=libc \
  2>&1 | tee "$UB_RUN/native-build.log"
make -C "$UB_RUN/source/tests/urma" OUT="$UB_RUN/native-probes" \
  "$UB_RUN/native-probes/client-probe" 2>&1 | tee "$UB_RUN/client-build.log"
ldd "$UB_RUN/source/src/valkey-server" | tee "$UB_RUN/server-libraries.log"
ldd "$UB_RUN/native-probes/client-probe" | tee "$UB_RUN/client-libraries.log"
sha256sum "$UB_RUN/source/src/valkey-server" "$UB_RUN/native-probes/client-probe" \
  > "$UB_RUN/native-binaries.sha256"
```

## 6. 两终端：带认证的 Valkey 同机功能

终端 A 创建本轮专用测试账户。密码只写入私有文件，ACL 使用哈希；不把密码打印到
终端、命令参数或运行日志。该实例仅绑定 loopback，使用独立目录和端口：

```bash
umask 077
mkdir "$UB_RUN/data"
python3 - "$UB_RUN" <<'PY'
import hashlib, secrets, sys
from pathlib import Path
p = Path(sys.argv[1])
password = secrets.token_hex(24)
(p / 'probe.password').write_text(password)
digest = hashlib.sha256(password.encode()).hexdigest()
(p / 'users.acl').write_text(
    'user default reset off\n'
    'user ubtest reset on #' + digest +
    ' ~__urma_probe:* +ping +set +get +del +info +shutdown\n')
PY
date -Iseconds > "$UB_RUN/valkey-start.txt"
set +e
"$UB_RUN/source/src/valkey-server" \
  --bind 127.0.0.1 --port 16379 --rdma-port 0 --urma-port 16382 \
  --urma-device "$UB_DEVICE" --urma-eid-index "$UB_EID" \
  --urma-allow-insecure-bootstrap yes --urma-buffer-limit 64mb \
  --io-threads 1 --cluster-enabled no --appendonly no --save "" \
  --protected-mode yes --aclfile "$UB_RUN/users.acl" \
  --dir "$UB_RUN/data" --daemonize no --logfile "" --loglevel notice \
  2>&1 | tee "$UB_RUN/valkey-server.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/valkey-server.exit"
```

该命令保持运行，出现 `Ready to accept connections urma` 后，终端 B（已加载同一
run.env）执行。此处的明文 bootstrap 显式限定在本机测试，业务命令通过 URMA：

```bash
set +e
VALKEY_URMA_USER=ubtest VALKEY_URMA_PASSWORD="$(cat "$UB_RUN/probe.password")" \
  timeout 60s "$UB_RUN/native-probes/client-probe" \
  127.0.0.1 16382 "$UB_DEVICE" "$UB_EID" --allow-insecure-bootstrap \
  2>&1 | tee "$UB_RUN/valkey-client.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/valkey-client.exit"
test "$rc" -eq 0
```

**通过条件：** 出现 `Native URMA PING and binary 65537-byte SET/GET/DEL: PASS`，
退出 0。探针会认证、校验 PING、逐字节比较跨帧二进制值并删除测试 Key；它不是性能发生器。

终端 B 从独立 TCP 管理监听读取统计，然后正常停止本轮实例：

```bash
VALKEYCLI_AUTH="$(cat "$UB_RUN/probe.password")" \
  "$UB_RUN/source/src/valkey-cli" -h 127.0.0.1 -p 16379 --user ubtest INFO urma \
  > "$UB_RUN/info-urma.log"
cat "$UB_RUN/info-urma.log"
VALKEYCLI_AUTH="$(cat "$UB_RUN/probe.password")" \
  "$UB_RUN/source/src/valkey-cli" -h 127.0.0.1 -p 16379 --user ubtest SHUTDOWN NOSAVE
```

终端 A 返回后，检查服务器退出码和日志；资源删除/保留告警需单独核对，不仅凭
客户端 PASS 就认定所有故障/关闭路径均已验收。

## 7. 追加 202→204 双机 SDK 检查

只有 202 也完成 SDK 盘点、资源检查且具备选定设备/EID 后才安排。下面使用已确认
可达的 TCP 管理地址 `192.168.17.204`；若该机 IP 已变化，先核对并替换。
两端设备/EID 各用本机值，端口双方均为 21206；TCP 地址不能替代 UB Fabric 连通性。

204 服务端（变量沿用本机配置）：

```bash
set +e
timeout 120s "$UB_PERF" send_lat --mgmt_proto tcp \
  -d "$UB_DEVICE" --eid_idx "$UB_EID" -p 1 --order_type 1 --share_jfr true -O 0 \
  -P 21206 -s 256 -n 1000 2>&1 | tee "$UB_RUN/sdk-cross-server.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$UB_RUN/sdk-cross-server.exit"
printf 'cross-server exit=%s\n' "$rc"
```

202 客户端（填入该机实际设备/EID，使用本机工具绝对路径）：

```bash
UB202_DEVICE='填写202实际设备名'
UB202_EID='填写202实际EID索引'
UB202_PERF=$(command -v urma_perftest)
mkdir -p /home/h30086230/valkey-validation
UB202_LOG=$(mktemp -d /home/h30086230/valkey-validation/ub202-cross.XXXXXX)
set +e
timeout 120s "$UB202_PERF" send_lat --mgmt_proto tcp \
  -d "$UB202_DEVICE" --eid_idx "$UB202_EID" -p 1 --order_type 1 --share_jfr true -O 0 \
  -P 21206 -S 192.168.17.204 -s 256 -n 1000 2>&1 | tee "$UB202_LOG/send.log"
rc=${PIPESTATUS[0]}
printf '%s\n' "$rc" > "$UB202_LOG/send.exit"
printf 'cross-client exit=%s\nEVIDENCE=%s\n' "$rc" "$UB202_LOG"
```

两端同样保存日志和退出码。202 的现有 Share TP/TP 查询问题没有因 204 通过而自动
解决。Valkey 双机功能还需两端原生 client-probe、匹配的认证配置及 204 的非 loopback
专用监听；按 [URMA 说明](../../tests/urma/README.md)固定这些参数后执行。

## 8. 任一阶段失败：采集本轮日志

从任一 root 终端加载对应 run.env 后执行，筛选这轮 root 运行的 UID=0：

```bash
for comm in urma_perftest client-probe valkey-server; do
  journalctl --since "$(cat "$UB_RUN/run-start.txt")" \
    --no-pager -o short-precise _COMM="$comm" _UID=0 > "$UB_RUN/$comm-journal.log"
done
journalctl -k --since "$(cat "$UB_RUN/run-start.txt")" --no-pager | \
  grep -Ei 'ubcore|uburma|udma|uvs|tpsa' > "$UB_RUN/kernel-journal.log" || true
cat "$UB_RUN/urma_perftest-journal.log"
cat "$UB_RUN/kernel-journal.log"
```

上面的时间窗口覆盖本轮记录的起始时间。SDK 日志中需关联自己的
perftest PID，内核日志可能同时含其他任务。若仍为共享 TP 拒绝或控制队列 TP ID
查询超时，按 [平台摘要](202-ub-platform-blockers.zh-CN.md)继续定位，不混改传输模式。

## 9. 归档与性能阶段的边界

成功或失败均可归档本轮材料；排除密码、ACL 和庞大的源码/二进制目录。下列命令
只改变新生成归档的所有者，便于原用户 scp 回收：

```bash
tar -C "$UB_RUN" --exclude='./probe.password' --exclude='./users.acl' \
  --exclude='./source' --exclude='./native-probes' -czf "$UB_RUN.tar.gz" .
chown h30086230 "$UB_RUN.tar.gz"
sha256sum "$UB_RUN.tar.gz"
printf 'ARCHIVE=%s.tar.gz\n' "$UB_RUN"
```

SDK send_lat/send_bw 结果、本机 Valkey 功能、双机功能分别报告。当前没有已实现的
等价原生 URMA Valkey 性能客户端，不提供不存在的 `valkey-benchmark --urma` 命令。
正式 QPS/p50/p95/p99/CPU 比较按 [改进方案](rdma-urma-performance-comparison-design.zh-CN.md)
补齐发生器后统一测量；本流程不将 SDK 微基准冒充 Valkey 性能。
