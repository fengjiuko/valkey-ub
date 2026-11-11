# 202 UB 建链失败：SDK/平台排查摘要

客户端记录日期：2026-11-11。以下现场时间按原始日志引用：主机 `date` 输出为
`2026-11-12T03:46:48+08:00`，perftest 错误位于该主机日志的 03:30～03:33。
后续归档应同时保留两端时钟记录，不据客户端日期直接截取主机日志。

## 当前结论

SDK 初始化、设备枚举、本地 DEFAULT/OT 资源创建及删除均已成功；RC 收发用例尚未
成功建链，更未通过 Valkey 原生 URMA 功能或性能测试。
已定位两个直接失败点，分别位于 liburma 兼容接口和 UDMA/ubcore TP 查询路径。
驱动控制队列为何超时、工具为何请求共享 TP，仍需现场 SDK/平台进一步确认。

环境：202/master，aarch64，`6.6.0-gdr_f00955680_diag`，UMDK SDK/tools
`25.12.0-B138.oe2403sp3.aarch64`，设备 `udmac0d1e2`，EID index 0。
执行文件 `/usr/bin/urma_perftest`，SHA256：
`e03bf302341a92b266c384a898c4f2251c58f9a314729b32f47b0509898a49b4`。

## 失败链 A：共享 TP 兼容接口拒绝

日志在多组成对进程中明确记录：

```text
urma_cp_api.c:urma_import_jetty_compat:2032
Share TP is not supported.
```

例如 PID 994127/994446、1011701/1011929，位于主机日志 03:30:01 和 03:30:41。
缓存的 SDK 参考源码在同名函数、同一行号附近为：

```c
if (rjetty->flag.bs.share_tp != 0) {
    URMA_LOG_ERR("Share TP is not supported.\n");
    return NULL;
}
```

因此可将直接拒绝点定位到 import 的 `share_tp` 配置。参考源码并非现场已验证的
完整源码包，最终由 B138 对应源码/调试符号复核。
这不是 `share_jfr`：共享传输通道 TP 与共享接收队列 JFR 是不同字段和能力。
开启 `--share_jfr true` 不能据此证明已经解决共享 TP 的兼容问题。
当前工具帮助没有列出可直接关闭 share_tp 的选项，不能编造一个开关作为修复。
当前 Valkey 参考实现对远端描述零初始化后设置 RC/RTP/OT，没有显式启用 share_tp；
因此也不能将 perftest 的共享 TP 拒绝直接认定为 Valkey 同样的缺陷。应用的 OT
能力位门禁仍然存在，后续须按它实际使用的非共享 TP 接口组合独立验证。

## 失败链 B：UDMA 控制队列查询 TP ID 超时

另一组 PID 1023228/1023429 在 03:31:20～03:31:32 的主要内核链为：

```text
UDMA: ctrl queue send message failed, ret = -110.
UDMA: udma control queue get TP id list failed, ret = -110.
ubcore_get_tp_list_helper: Get tp list from ops failed, ret = -110.
ubcore_create_tpid_priv: Failed to query/select tpid, ret = -28.
ubcore_bind_jetty_reuse_compat: Failed to create tpid for reuse, ret: -1.
uburma_cmd_bind_jetty_ex: bind jetty failed, ret: -1.
```

对应用户态还有 `ioctl failed, ret=-1, errno=1, cmd=49` 和
`udma_u_bind_jetty_ex` 失败。
PID 1061853/1061979 在 03:32:50～03:33:03 的 import 路径也有相同 TP 列表查询
超时，随后 `ubcore_import_jetty failed`，对应 ioctl cmd=47。

按 Linux errno 定义，`-110` 为超时。这里的 `-28` 是后续 TP 查询/选择阶段的返回，
不能单独解释为 TP 资源确实耗尽；用户态 `errno=1` 也不能覆盖内核更具体的超时证据，
不能只凭它认定普通账户缺少权限。

本次 root 只读取旧失败日志；查询指定 `_UID=1217`，不是一次新的 root 打流结果。
因此尚未比较普通用户与 root 下同一用例的表现。
ps/systemctl 输出没有匹配到 UVS/TPSA 项；它不足以证明服务不存在或一定需要启动。
目前不将 UVS 状态作为上述控制队列超时的唯一根因。

## 平台侧需要确认的事项

1. B138 liburma、UDMA provider、当前诊断内核中的 udma/ubcore/uburma/ubase 与固件
   是否为同一已验证组合；控制队列 TP 列表查询是否被支持、是否需要先完成平台初始化。
2. UDMA 为什么在 get TP ID list 的控制消息处返回超时；检查对应设备/固件响应和
   控制队列状态，确认同机回环及当前 EID 是否支持该 TP 建链路径。
3. perftest 在哪些配置下设置 `rjetty.flag.share_tp`；B138 的 compat 导入接口与
   provider `_ex` 接口的配套契约是什么。不要将 share_jfr 当作 share_tp。
4. 推荐的最小 RC/SEND 同机用例究竟使用哪种控制面/TP 模式。若推荐 user_tp、CTP
   或其他路径，应明确它与当前 Valkey 所用 RC/RTP/OT 导入/绑定接口的差异。

平台链路恢复后，先用固定参数的 SDK 成对用例复验，再验收显式 OT 的顺序、分段、
credit/CLOSE 和资源清理，随后进入 Valkey 原生 URMA 功能。当前不需要重编译 Valkey
来处理这些 perftest 建链错误，也不依据现有证据放宽 Valkey 的 OT 能力检查。

## root 会话：补取驱动及包身份

以下只查询版本信息，输出固定放在原测试用户的绝对路径下。shell 通过 `command -v`
解析管理工具的绝对路径，避免假定它们位于 `/usr/bin` 或 `/usr/sbin`。

```bash
(
set +e
mkdir -p /home/h30086230/valkey-validation
platform_root=$(mktemp -d /home/h30086230/valkey-validation/ub-platform-identity.XXXXXX) || exit 1
modinfo_path=$(command -v modinfo)
rpm_path=$(command -v rpm)
{
  date -Iseconds
  uname -a
  if test -n "$modinfo_path"; then
    printf 'modinfo=%s\n' "$modinfo_path"
    for module in udma ubcore uburma ubase; do
      printf '\nMODULE=%s\n' "$module"
      "$modinfo_path" "$module" | grep -E '^(filename|version|srcversion|vermagic|depends):'
    done
  fi
  if test -n "$rpm_path"; then
    "$rpm_path" -qf /usr/bin/urma_perftest /usr/lib64/liburma.so.0 /usr/lib64/urma/liburma-udma.so
    "$rpm_path" -V umdk-urma-lib umdk-urma-devel umdk-urma-tools
  fi
} > "$platform_root/identity.log" 2>&1
cat "$platform_root/identity.log"
printf 'PLATFORM_EVIDENCE=%s\n' "$platform_root"
)
```

`rpm -V` 有输出时保留原样；无输出只说明该命令未报告文件校验差异，不证明硬件
功能通过。版本记录应与以下现场证据目录一并保留：

- `/home/h30086230/valkey-validation/ub-preflight.jZjPxI`
- `/home/h30086230/valkey-validation/ub-order-diagnostic.HhUfOi`
- `/home/h30086230/valkey-validation/ub-control-state.a7oJw2`

系统日志含本次 perftest 之外的 GPU 任务活动。对外排查摘要只引用上述关联 PID 的
必要错误，不把其他任务的映射、注销信息解释为本次 perftest 的结果。
