# 202 UB 排序能力诊断

更新日期：2026-11-11。本轮先诊断 SDK/provider 对排序配置的接受情况，不更改
Valkey 原生 URMA 的能力检查或收发语义，不把资源创建成功当作功能/性能通过。

最新 root 日志已定位共享 TP 的兼容接口拒绝，以及 UDMA 控制队列查询 TP ID 超时，
见 [SDK/平台排查摘要](202-ub-platform-blockers.zh-CN.md)。后续 root 操作使用该文档的
绝对证据路径；本页保留前面的诊断过程，不能将早期猜测当作最终根因。

## 现场证据

- 工具提交：`a4cf86525f5458ec0df2432468489ca009ade1b8`，工作区查询未显示修改。
- 202 内核：`6.6.0-gdr_f00955680_diag`，aarch64，页大小 4096。
- `umdk-urma-devel` 与 `umdk-urma-lib` 均为 `25.12.0-B138.oe2403sp3.aarch64`。
  provider 目录有 `liburma-udma.so` 和 `liburma_ubagg.so.0.0.3`；仅目录列表不证明
  provider/诊断内核完全配套，也不能排除发行包内的能力报告兼容问题。
- SDK 编译、动态链接、初始化和设备枚举成功，枚举 7 个 `transport_type=0` 设备及 EID。
  现场头文件确认 0 对应 `URMA_TRANSPORT_UB`。
- 7 个设备均报告 `modes=7 rc_order=0 rc_tp=3 max_message=65536`，队列上限足够大。
  现场头文件确认 `ot` 是 `rc_order_cap` 位图的一位；位图全零表示没有报告 OT 支持。

Valkey 目前在 `urmaRuntimeOpen` 检查 `rc_order_cap.bs.ot`，并在 JFR、发送 Jetty 和
远端导入配置中显式选择 `URMA_OT`。据现场报告，现有适配器会在创建上下文前拒绝设备。
这说明当前适配器与报告能力不匹配，尚不能据此认定硬件不具备可用的可靠收发能力。

现场头文件另提供 `URMA_DEF_ORDER`，注释为驱动自动配置。
[openEuler API 指南](https://docs.openeuler.org/zh/docs/24.03_LTS_SP4/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)
将 RC 的默认排序描述为 OL，而非 OT。该文档不能替代现场 B138/诊断内核组合的
实际契约；不将 DEFAULT 与 OT 当作同义词，也不直接删去应用的能力检查。

## 独立探针

[`tests/urma/order_probe.c`](../../tests/urma/order_probe.c) 对同一设备/EID 分别尝试
`URMA_DEF_ORDER` 和 `URMA_OT` 的 RC JFR/Jetty 创建，打印失败阶段、errno 和清理状态。
它短暂创建本进程的 context/JFCE/JFC/JFR/Jetty；不注册数据缓冲、不投递工作请求、
不导入远端端点、不建链、不收发数据，也不修改设备配置或现有 Valkey 实例。
删除失败时不再显式删除依赖资源，以非零状态退出并保留诊断输出。

在 202 更新工具后执行以下 Bash，先以已枚举的 `udmac0d1e2`、EID index 0 为候选。
该选择只针对资源创建诊断，不表示已确定双机链路或同机回环路径：

```bash
cd "$HOME/valkey-ub"
git fetch origin work/rdma-perf-baseline
git switch work/rdma-perf-baseline
git pull --ff-only origin work/rdma-perf-baseline
```

```bash
(
set -euo pipefail
mkdir -p "$HOME/valkey-validation"
diag_root=$(mktemp -d "$HOME/valkey-validation/ub-order-diagnostic.XXXXXX")
git rev-parse HEAD > "$diag_root/tool-sha.txt"
make -C tests/urma OUT="$diag_root" "$diag_root/order-probe" \
  2>&1 | tee "$diag_root/build.log"
ldd "$diag_root/order-probe" | tee "$diag_root/libraries.log"
set +e
timeout 30s "$diag_root/order-probe" udmac0d1e2 0 \
  2>&1 | tee "$diag_root/order.log"
status=${PIPESTATUS[0]}
set -e
printf '%s\n' "$status" > "$diag_root/order.exit"
printf 'order-probe exit=%s\nEVIDENCE=%s\n' "$status" "$diag_root"
)
```

退出 0 仅表示两种排序请求的资源创建及清理均成功；退出 1 可表示其中一种配置
被拒绝或初始化/清理失败，必须查看阶段；退出 2 为参数错误，124 为 timeout 超时。
错误输出中的 errno 若为 0，表示该失败接口没有提供有效 errno，不能解释为调用成功。

| 结果 | 能说明什么 | 下一步 |
| --- | --- | --- |
| DEFAULT 成功、OT 被拒绝 | 该组合至少接受默认配置；显式 OT 不可用或被拦截 | 核对 provider/驱动契约，评估是否支持 OL 方案 |
| DEFAULT、OT 均成功，但报告位图为 0 | 资源创建接受 OT 请求，与能力报告存在差异 | 核对报告契约并验证实际收发/顺序，不直接放宽应用检查 |
| 两者均在公共阶段失败 | 上下文、权限、EID、队列或 provider 等条件尚不满足 | 根据首个失败阶段定位，不能直接归因于排序 |
| 清理失败或超时 | 生命周期未验证通过 | 保留日志，先处理资源/驱动问题，不循环重复压测 |

无论创建结果如何，后续仍需验证接收顺序、跨帧大值、credit、CLOSE、异常及资源
清理，再进入 Valkey 功能探针。若评估 DEFAULT/OL 方案，须审查本地创建、远端导入、
工作请求顺序标志和协议假设的一致性，不只改一处开关。

## 202 资源诊断实际结果

用户补充的现场日志显示，`udmac0d1e2` / EID index 0 的诊断于 2026-11-11 完成，
证据目录为 `~/valkey-validation/ub-order-diagnostic.HhUfOi`：

| 项目 | 结果 |
| --- | --- |
| 报告的 RC 排序能力 | `rc_order=0 ot=0 ol=0` |
| SGE 上限 | send 13，receive 4 |
| 默认排序 | JFCE/JFC/JFR/Jetty 创建成功，清理状态均为 0 |
| 显式 OT | 同样创建成功，清理状态均为 0 |
| Context 删除 / SDK uninit | 均为 0 |
| 探针退出码 | 0 |

因此，当前设备组合接受显式 OT 的本地资源配置，不能再把位图为零直接解释为
“OT 资源不可创建”。能力报告与接受配置之间存在差异；是否真正提供预期排序保证、
是否可以建链及收发，仍待验证。现有 Valkey OT 门禁尚未更改。

## SDK 收发现场失败及参数复核

此前的收发命令尚未跑通，不能作为已验证运行命令。完整帮助现已确认其默认管理
通道为 TCP；“缺少 UB 管理 EID”并不是解释此前 TCP 模式失败的已验证根因。
实际日志记录：

| 已执行配置 | 可确认的结果 |
| --- | --- |
| RC、OT、设备 udmac0d1e2、EID index 0，控制端口 21116 | 两端 `Failed to import jetty: 0!`，退出码均为 255 |
| 加 `--share_jfr true` 的 OT 客户端重试 | share_jfr 警告消失，但仍 import 失败；该段未包含配对服务端的完整命令和退出码 |
| RC、默认排序、share_jfr true，控制端口 21117 | 两端 `Failed to bind jetty: 0!`，该段未记录退出码 |

因此尚未进入成功收发。share_jfr 警告消失不能证明它是根因；默认排序与 OT 的失败
阶段不同，需要在相同且已核对的管理通道配置下重新配对比较。
`Failed ... jetty: 0!` 中 0 的含义需要对应工具源码确认，不把它当作明确的 errno。

现场完整帮助确认：

| 参数 | 准确语义 |
| --- | --- |
| `--mgmt_proto` | `tcp` 或 `ub`，默认 `tcp`；管理通道与 UB 数据通道分开 |
| `-S` | TCP 管理模式为服务端 IP；UB 管理模式为服务端管理 EID |
| `-P` | TCP 模式为端口；UB 模式为双方一致的服务端管理 Jetty ID，且小于 1024 |
| `--mgmt_addr` | 仅在 UB 管理模式指定本地管理 EID，且该模式必填 |
| `--eid_idx` | 数据设备的 EID 索引；后面的 server mgmt eid 说明实际属于 `-S` |
| `-p` | 0 为 RM（默认），1 为 RC，2 为 UM |

因此原命令默认使用 TCP 管理通道，`-S 127.0.0.1` 和 `-P 21116` 在该模式下有效，
不要求 `--mgmt_addr`。不能仅因数据设备是 UB 就认定工具的管理通道也选了 UB。
`-P 21116` 不适用于 UB 管理模式，不能仅加 `--mgmt_proto ub` 后照用原端口值。

实际执行文件是 `/usr/bin/urma_perftest`，归属
`umdk-urma-tools-25.12.0-B138.oe2403sp3.aarch64`，SHA256 为
`e03bf302341a92b266c384a898c4f2251c58f9a314729b32f47b0509898a49b4`。
现场 `send_lat --help` 报 unrecognized option 后打印帮助，帮助本身的退出状态
不能当作硬件测试结果；后续可尝试帮助中列出的短参数 `-h`。

用户输出中夹带的分析段落和未执行命令不作为测试证据。尤其是最后一组 mgmt_addr
建议省略了 `-p 1`，按通用手册会回到 RM 默认值，不能当作原 RC/OT 用例的修正结果。
EID 的选择依据实际拓扑和 SDK 契约，不能根据十六进制字符串中出现的某个字节猜测。
目前也没有证据确认 204 已有可用的原生 UB 路径。

## 下一步：只读检查控制面与失败日志

以下为取得详细日志前的采集过程；目前已获得具体错误，转入上述平台排查摘要。
取得日志前不能根据 import/bind 的通用报错指定某个修复参数。
[官方接口说明](https://docs.openeuler.org/zh/docs/24.03_LTS_SP4/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)
列出了控制面 UVS 无响应导致 import 失败的情形；它是需要排查的方向，不是已确认根因。
先读取现有状态和最近失败日志，不启停服务、不重配设备：

```bash
(
set +e
state_root=$(mktemp -d "$HOME/valkey-validation/ub-control-state.XXXXXX") || exit 1
{
  date -Iseconds
  ps -eo pid,comm | grep -Ei 'uvs|tpsa'
  systemctl list-units --all --type=service --no-pager --full | grep -Ei 'uvs|tpsa'
  journalctl --since '-30 min' --no-pager -o short-precise \
    _COMM=urma_perftest _UID="$(id -u)"
  journalctl -k --since '-30 min' --no-pager | grep -Ei 'ubcore|uburma|udma|uvs|tpsa'
} > "$state_root/state.log" 2>&1
cat "$state_root/state.log"
printf 'STATE_EVIDENCE=%s\n' "$state_root"
)
```

服务名可能不同，因此没有匹配结果不等于证明服务不存在；日志不可读或被其他
syslog 服务转存时，也不能将空输出视为没有错误。保留权限提示，必要时由现场管理员
协助导出对应时段日志。近 30 分钟未覆盖本轮失败时，按实际时间扩大查询窗口。

若需重跑来抓日志，使用进程级 `URMA_LOG_LEVEL=debug`，明确保留
`--mgmt_proto tcp -p 1 --order_type 1 --share_jfr true -O 0`，设备、EID 和端口保持一致。
SDK 参考实现默认将日志送往 syslog，单独设置日志级别不保证错误会打印到终端。
当前未验证 `URMA_LOG_PATH` 为现场 SDK 支持的接口，不提供该变量作为日志重定向方案。
不要批量更换设备/EID、混加 CTP/tp_aware/simplex，或把用户态 TP 建链选项直接当作
默认控制面的修复。每种路径应单独记录并核对与 Valkey 适配器的契约是否一致。

即使 SDK 收发完成，仍需严格的接收顺序、Payload 完整性、分段/credit/CLOSE 验证，
才进入 Valkey 功能验收。perftest 延迟不直接与 Valkey benchmark 的 QPS 比较。

## 软件检查边界

无硬件测试使用真实 SDK 头文件编译，但只链接 `order_probe_mock.c`，不链接
provider、不初始化设备：

```bash
make -C tests/urma test-order-probe
```

自定义头文件路径时传 `URMA_CFLAGS='-isystem /实际/SDK/include'`。
本地 8 项测试覆盖 DEFAULT/OT 诊断、零能力报告、创建拒绝、失败阶段、依赖清理、
参数校验及非原生 UB 设备拒绝。它们不能证明现场 API 接受配置或实际收发保序。
