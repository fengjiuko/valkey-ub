# 204 原生 UB / URMA 验证步骤

日期：2026-11-11。状态：待执行；此前 204 已有 RoCE/RDMA 功能记录，尚未取得本页
原生 UB 验证结果。采用 root 会话、绝对源码/证据路径，分阶段保留退出码和日志。
204 的设备名、EID、内核/固件及工具选项均以本机查询为准。

## 阶段与通过条件

| 阶段 | 动作 | 能证明什么 |
| --- | --- | --- |
| 1 | 构建 SDK 探针、枚举本机设备 | SDK/库可用，是否存在原生 UB 设备及 EID |
| 2 | DEFAULT/OT 本地资源创建及销毁 | 选定设备/EID 的资源配置是否被接受 |
| 3 | 204 两进程 RC/OT 同机建链及 SDK 收发 | 当前路径能否完成 SDK 收发，不等同 Valkey 正确性 |
| 4 | 202→204 SDK 双机收发 | 两机 UB 路径可用性，同机结果不能替代此项 |
| 5 | Valkey 原生 URMA 功能 | AUTH/PING、二进制 SET/GET/DEL、分段和资源清理 |
| 6 | 等价发生器性能采集 | 功能通过且统一客户端具备后才开展 |

当前优先取得阶段 1 输出，再固定后续设备/EID。不要仅因安装了 liburma，就将
RoCE 设备名 `rocep40s0f0` 填入原生 UB 用例。

## 准备工具仓库

默认仓库路径为 `/home/h30086230/valkey-ub`。若该机目录不同，替换绝对路径。
Git 更新由仓库所有者执行，下面可从 root 会话调用，避免 root 改写普通用户的仓库：

```bash
runuser -u h30086230 -- git -C /home/h30086230/valkey-ub status --short
runuser -u h30086230 -- git -C /home/h30086230/valkey-ub fetch origin work/rdma-perf-baseline
runuser -u h30086230 -- git -C /home/h30086230/valkey-ub switch work/rdma-perf-baseline
runuser -u h30086230 -- git -C /home/h30086230/valkey-ub pull --ff-only origin work/rdma-perf-baseline
```

如有本地改动或切换/更新失败，保留输出，先处理仓库状态，不强制覆盖。
没有仓库时可以由该用户在新目录克隆本分支，再按实际路径执行。

## 阶段 1：204 SDK 与设备枚举

以下独立构建探针，不重新构建或清理现有 Valkey：

```bash
(
set -euo pipefail
ub204_repo=/home/h30086230/valkey-ub
mkdir -p /home/h30086230/valkey-validation
ub204_root=$(mktemp -d /home/h30086230/valkey-validation/ub204-preflight.XXXXXX)
printf 'EVIDENCE=%s\n' "$ub204_root"
{
  hostname
  id
  date -Iseconds
  uname -a
  ulimit -l
  rpm -qa | grep -Ei 'umdk|urma|ubcore|udma' || true
  ls -l /dev/uburma /dev/uburma/* /usr/lib64/urma/ 2>/dev/null || true
} > "$ub204_root/environment.log" 2>&1
cat "$ub204_root/environment.log"
make -C "$ub204_repo/tests/urma" OUT="$ub204_root" "$ub204_root/sdk-probe" \
  2>&1 | tee "$ub204_root/build.log"
ldd "$ub204_root/sdk-probe" | tee "$ub204_root/libraries.log"
"$ub204_root/sdk-probe" --link-only 2>&1 | tee "$ub204_root/link.log"
set +e
timeout 30s "$ub204_root/sdk-probe" --inventory \
  2>&1 | tee "$ub204_root/inventory.log"
status=${PIPESTATUS[0]}
set -e
printf '%s\n' "$status" > "$ub204_root/inventory.exit"
printf 'inventory exit=%s\nEVIDENCE=%s\n' "$status" "$ub204_root"
)
```

检查 init/query/uninit 的状态、device_count、transport_type、模式/排序能力和 EID。
本项目 SDK 定义中 `transport_type=0` 为原生 UB；枚举到其他类型不算原生 UB 通过。
device_count 为 0、构建/库加载失败或没有可用 EID 时，先处理该阶段的问题。
库存查询退出 0 不证明实际建链、收发或同机回环成功。

## 阶段 2：本地资源创建/销毁

按阶段 1 输出填写下面两个变量。示例占位值不是 204 的已知设备：

```bash
UB204_DEVICE='填写204实际原生UB设备名'
UB204_EID_INDEX='填写该设备实际EID索引'
```

保持这两个变量在当前 root 会话中，然后执行：

```bash
(
set -euo pipefail
: "${UB204_DEVICE:?先填写204设备名}"
: "${UB204_EID_INDEX:?先填写204 EID索引}"
ub204_order_root=$(mktemp -d /home/h30086230/valkey-validation/ub204-order.XXXXXX)
printf 'EVIDENCE=%s\n' "$ub204_order_root"
make -C /home/h30086230/valkey-ub/tests/urma OUT="$ub204_order_root" \
  "$ub204_order_root/order-probe" 2>&1 | tee "$ub204_order_root/build.log"
set +e
timeout 30s "$ub204_order_root/order-probe" "$UB204_DEVICE" "$UB204_EID_INDEX" \
  2>&1 | tee "$ub204_order_root/order.log"
status=${PIPESTATUS[0]}
set -e
printf '%s\n' "$status" > "$ub204_order_root/order.exit"
printf 'order-probe exit=%s\nEVIDENCE=%s\n' "$status" "$ub204_order_root"
)
```

退出 0 只表示两种请求配置均创建并清理成功。若报告的 rc_order 仍为 0，保留原值；
不能据此自动放宽 Valkey 的能力门禁。资源清理失败、超时或上下文失败时先定位，
不要循环批量尝试设备/EID。

## 阶段 3：204 同机 SDK 收发模板

先保存 204 本机工具身份和完整帮助，不能假定它与 202 的 B138 二进制相同：

```bash
ub204_perf=$(command -v urma_perftest)
printf 'PERF=%s\n' "$ub204_perf"
test -n "$ub204_perf" && sha256sum "$ub204_perf"
test -n "$ub204_perf" && rpm -qf "$ub204_perf"
test -n "$ub204_perf" && "$ub204_perf" send_lat -h
```

确认该版本支持下面的参数组合后执行；若选项不同，先保留帮助输出再调整。
此处显式使用 TCP 管理通道，`-S` 为 IP，`-P` 为 TCP 端口，不需要 UB 管理 EID。
数据仍走选定的原生 UB 设备。两终端均填写相同的实际设备/EID。

root 终端 A（服务端）：

```bash
(
set +e
: "${UB204_DEVICE:?先填写204设备名}"
: "${UB204_EID_INDEX:?先填写204 EID索引}"
ub204_perf=$(command -v urma_perftest) || exit 1
ub204_log=$(mktemp -d /home/h30086230/valkey-validation/ub204-sdk-server.XXXXXX) || exit 1
date -Iseconds > "$ub204_log/start.txt"
URMA_LOG_LEVEL=debug timeout 120s "$ub204_perf" send_lat \
  --mgmt_proto tcp -d "$UB204_DEVICE" --eid_idx "$UB204_EID_INDEX" \
  -p 1 --order_type 1 --share_jfr true -O 0 \
  -P 21204 -s 256 -n 1000 2>&1 | tee "$ub204_log/send.log"
status=${PIPESTATUS[0]}
printf '%s\n' "$status" > "$ub204_log/send.exit"
printf 'server exit=%s\nEVIDENCE=%s\n' "$status" "$ub204_log"
)
```

root 终端 B（客户端），服务端等待连接后尽快执行：

```bash
(
set +e
: "${UB204_DEVICE:?本终端也要填写204设备名}"
: "${UB204_EID_INDEX:?本终端也要填写204 EID索引}"
ub204_perf=$(command -v urma_perftest) || exit 1
ub204_log=$(mktemp -d /home/h30086230/valkey-validation/ub204-sdk-client.XXXXXX) || exit 1
date -Iseconds > "$ub204_log/start.txt"
URMA_LOG_LEVEL=debug timeout 120s "$ub204_perf" send_lat \
  --mgmt_proto tcp -d "$UB204_DEVICE" --eid_idx "$UB204_EID_INDEX" \
  -p 1 --order_type 1 --share_jfr true -O 0 \
  -P 21204 -S 127.0.0.1 -s 256 -n 1000 2>&1 | tee "$ub204_log/send.log"
status=${PIPESTATUS[0]}
printf '%s\n' "$status" > "$ub204_log/send.exit"
printf 'client exit=%s\nEVIDENCE=%s\n' "$status" "$ub204_log"
)
```

通过条件为两端正常完成、退出 0、输出配置与预期一致、没有完成错误或超时。
它仍不是逐值完整性和严格顺序证明。失败时按对应 start.txt 查询 journal，root 用例
筛选 `_UID=0`，不要继续用 202 普通用户的 `_UID=1217`。例如在各终端中：
先将 `ub204_log` 设为该终端打印的实际 EVIDENCE 绝对路径；括号子 shell 中的变量
不会在该段命令结束后保留到外层 shell。

```bash
journalctl --since "$(cat "$ub204_log/start.txt")" --no-pager \
  _COMM=urma_perftest _UID=0
journalctl -k --since "$(cat "$ub204_log/start.txt")" --no-pager | \
  grep -Ei 'ubcore|uburma|udma|uvs|tpsa'
```

主机日志时钟以本机 date 为准，关联自己的 perftest PID；kernel 查询可能含其他任务，
不能将其他进程的 GPU 映射等信息归到本次用例。

若重复出现 `Share TP is not supported` 或 TP 查询超时，按
[平台阻塞摘要](202-ub-platform-blockers.zh-CN.md)关联 SDK/驱动日志，不通过混改
EID、CTP、user_tp、simplex 或重载驱动来判定测试通过。

## 阶段 4～6：双机、Valkey 功能和性能

204 同机 SDK 通过后，再安排 202 与 204 两端 SDK 双机验证。TCP 管理模式的客户端
`-S` 改成选定的 204 可达管理 IP；两端数据设备/EID 各按本机实际值配置，验证 UB
Fabric 连通性。当前 202 的 SDK 路径仍未通过，204 通过不能代替另一端通过。

SDK 双机和实际顺序/关闭契约确认后，按 [URMA 功能说明](../../tests/urma/README.md)
构建独立原生 URMA Valkey 实例，验收认证、PING、65537 字节二进制 SET/GET/DEL、
分段、credit/CLOSE 和清理。若设备没有报告 OT，先解决兼容契约，不能删门禁就宣布通过。

最后按 [统一性能评估方案](rdma-urma-performance-comparison-design.zh-CN.md)补齐等价
客户端再比较。SDK send_lat、204 同机结果和 Valkey 双机结果分别报告。
本页发布不表示 204 已完成任何新的 UB 测试。
