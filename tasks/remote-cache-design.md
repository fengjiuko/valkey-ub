# M1：可重建远端缓存与 Value 池

日期：2026-11-06。用户明确选择先保留本地权威副本。此决定只推进 M1 的独立
实验缓存，不批准 M2/M3 的远端权威容量语义，也不宣称 Checkpoint A 的硬件关口通过。

## 决策与落点

- `remote_value.[ch]` 是单所有者、固定 slot 的 CPU 可寻址 Value 池。arena 由调用者
  提供并保持存活；模块只分配有界元数据，不替换 zmalloc，不修改 robj/SDS 编码。
- `remote_cache.[ch]` 是独立缓存协议、同步服务端分派及本地回复发布门禁。没有隐式
  网络 I/O。native 探针用已有 `urmaTransport` 流承载完整缓存消息；不是 URMA 单边
  READ/WRITE，也不提供 CPU 直接 load/store 访问远端 UB 内存。
- `tests/urma/cache_probe.c` 是双端、单客户端、四请求的验收探针，服务进程的池保存
  客户端权威缓冲区的副本。不是长驻多租户缓存服务，没有接入 Valkey GET/SET。

维持原有 String/对象、持久化、复制、脚本、事务和模块指针契约，是因为当前没有
完整数据库异步取回/失效适配器，也没有硬件 DMA 终止及平台映射证据。

## 所有权、身份与失败

Identity 是 `{owner incarnation, object incarnation}`，另带非零 Value version；
不能用可能碰撞的 Key hash 代替对象身份。同名 Key 删除/过期后重建必须换 object
incarnation，覆盖必须更新 version。未来命令适配器应在主线程重新查找 Key、处理
本地过期，再把当前身份/版本传入发布门禁；不能跨等待持有可释放的裸 robj 指针。

Ref 是 `{pool epoch, slot generation, slot, length, CRC64}`，不是本地指针或访问凭据。
池每次启动必须选择新非零 epoch；真实分布式部署仍需独立保证 epoch/incarnation
命名空间不复用。generation 不允许回绕后复用。Token/DMA fencing 不是这些标识的功能。

PUT 先核对大小、TTL、CRC64，再复制并发布新 Ref；池满按缓存写入失败处理。
GET 核对身份、版本、epoch/generation、长度、CRC 和到期时间；损坏/过期变成 miss。
DROP/到期会撤销新访问，已借用的只读 view 仍固定 slot，最后一个 view 释放前不得
复用或销毁池。view 必须零初始化且不可复制；消费方先复制到自有发送缓冲再释放。

客户端只有在请求号、操作、对象身份/当前版本、Ref、长度和 CRC 全部匹配时才发布
回复。超时/解码失败用空回复传入，输出不变并返回 miss。权威本地副本一直保留，
缓存故障不使已确认 SET 丢数据。当前接口返回 miss，真正的数据库本地回退与缓存
重建调度尚须命令适配器接线，不能据单元测试宣称真实 Key 路径已接入。

CRC64 仅检测意外损坏，不认证远端。调用者在启动阶段先初始化现有 `crc64` 表。
TTL 是池所有者本地单调时钟的相对毫秒数，限制在 1–300000；不跨主机比较绝对时钟。

## 实验 wire v1

所有整数大端，固定头 88 字节，最大 Value 1 MiB，最多 65536 slots。
只有 PUT 请求和成功 GET 回复携带 Value 字节。空 Value 合法；空消息和 miss 不混同。

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 4 | `VKVC` |
| 4 | 1 | 版本 1 |
| 5 | 1 | PUT=1 / GET=2 / DROP=3 |
| 6 | 1 | request=0 / response=1 |
| 7 | 1 | OK=0 / MISS=1 / FULL=2 / INVALID=3；请求必须 OK |
| 8 | 4 | payload 字节数 |
| 12 | 4 | 保留，必须为零 |
| 16 | 8 | 非零 request ID |
| 24 | 8 | 非零 owner incarnation |
| 32 | 8 | 非零 object incarnation |
| 40 | 8 | 非零 Value version |
| 48 | 8 | pool epoch；PUT 请求为零 |
| 56 | 8 | slot generation；PUT 请求为零 |
| 64 | 4 | slot；PUT 请求为零 |
| 68 | 4 | 逻辑 Value 长度 |
| 72 | 8 | Value CRC64 |
| 80 | 8 | PUT 请求相对 TTL；其他消息为零 |

接收方先验证固定头与长度上限，再读取/分配 body；拒绝未知类型、非零保留字段、
非法状态组合和尾随字节。编码支持 payload 与输出重叠，解码借用 wire，不转移所有权。

## 资源和性能边界

池的物理容量是 `floor(arena_size / slot_size) * slot_size`，逻辑 live 字节、元数据、
pin 和 retired slots 分开统计；空闲碎片不算 live 数据。PUT/Expire 扫描有固定上限，
不是可扩展分配器性能方案。还需单独核算 runtime 注册/线缓冲、请求/回复 staging、
本地权威副本以及驱动内存。此阶段复制副本，不声称减少权威驻留容量或改善性能。

双端 probe 固定最多四个请求；10 秒流 I/O 期限和 30 秒 accept 等待不是同步 SDK
调用的可中断保证。引导明文、无 peer 认证，必须显式指定 `--allow-insecure-bootstrap`，
仅在获准的隔离环境运行。数据不经 TCP 回退；没有实际设备时不启动测试服务。
