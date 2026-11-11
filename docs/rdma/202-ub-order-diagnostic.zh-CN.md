# 202 UB 排序能力诊断

更新日期：2026-11-11。本轮先诊断 SDK/provider 对排序配置的接受情况，不更改
Valkey 原生 URMA 的能力检查或收发语义，不把资源创建成功当作功能/性能通过。

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

## 软件检查边界

无硬件测试使用真实 SDK 头文件编译，但只链接 `order_probe_mock.c`，不链接
provider、不初始化设备：

```bash
make -C tests/urma test-order-probe
```

自定义头文件路径时传 `URMA_CFLAGS='-isystem /实际/SDK/include'`。
本地 8 项测试覆盖 DEFAULT/OT 诊断、零能力报告、创建拒绝、失败阶段、依赖清理、
参数校验及非原生 UB 设备拒绝。它们不能证明现场 API 接受配置或实际收发保序。
