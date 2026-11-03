# 原生 URMA 通信：本地设计工作稿

状态：本地实验方案；protocol 和 session 基础模块已实现，其余组件仍为拟议设计。
没有原生 URMA ConnectionType 或硬件兼容性声明。
适用基线和 SDK 快照见 [计划](plan.md)。上游讨论草稿见
[跟踪草稿](upstream-issue-draft.md)，尚未发布。

## 决策与理由

在 `ConnectionType` seam 增加独立 URMA adapter，上层仍使用现有 RESP、
命令执行和输出缓冲。下层直接调用 liburma，不通过 RoUB、UMS 或普通 TCP
承载业务数据。TCP/TLS 只用于首版端点引导。

首版采用目标设备支持且验证过的可靠、有序模式组合；可靠性不自动意味着
同一应用字节流的顺序保证。模式、顺序及事件能力在 U1 验证后锁定，不能根据
鲲鹏处理器型号猜测 provider 或把所有平台固定成某个 RC/CTP 模式。

先用双边 Send/Receive，按需在独立后续改动评估单边数据传输。不把客户端
数据传输、远端 Value 存储和数据库事务混为一个协议。

## 结构与代码落点

```text
RESP parser / command execution / output buffers
                    |
      src/urma.c : ConnectionType adapter
                    |
      src/urma_transport.[ch] : runtime ---> liburma / provider / UB hardware
                    | uses
       src/urma_session.[ch] : bounded stream state

src/urma_protocol.[ch] : explicit wire encoding and validation
libvalkey URMA adapter : corresponding client stream contract
```

`urma.c` 拟负责监听、accept/connect、回调与 ae；transport 拟直接调用 liburma，
管理 Jetty、Segment、硬件完成和销毁；现有 session 只管理有界字节流、信用及
本地缓冲，protocol 只处理帧和能力协商，不理解 Key/Value。
图中的 session 是 transport 使用的状态组件；它自身不调用 liburma。
不引入跨 TCP/TLS/RDMA 的通用框架，不改变现有 RDMA 线协议。

现有集成点包括 `src/connection.[ch]`、`src/config.c`、`src/server.c`、
`src/Makefile` 及 CMake 源列表/依赖配置。客户端使用 libvalkey 的
`valkeyContextFuncs`；独立客户端上游改动和本仓库依赖同步分别提交。
服务端、客户端共享规范与测试向量，不依赖互相的私有结构或 Valkey 全局变量。

## 对象与接口契约

| 对象 | 所有者 | 不变量 |
| --- | --- | --- |
| Runtime | 首版主线程 | 设备上下文、全局限额及事件调度集中管理 |
| Session | 一个逻辑连接 | 状态、信用、RX/TX 缓冲和 peer 信息不外泄 |
| Operation | 在途请求 | 完成或 API 保证终止前保留缓冲及会话引用 |
| urma_connection | Valkey adapter | 包含 connection，持有 Session，沿用连接回调规则 |

会话内部以 `{status, bytes}` 表示 OK、AGAIN、EOF、ERROR。有进度时返回 OK
和连续前缀长度；随后遇到错误应锁存并在下一次调用报告，不能同时让调用方
无法确定哪些字节已消费。read/write 零长度请求明确为无副作用，不当成 EOF。

- `write(data, len)` 返回 n 后，调用方可复用前 n 字节；首版复制到会话自有
  注册缓冲区。发送完成不是对端执行确认。
- `writev` 只能消费输入的连续前缀，某段未写完不能继续后段。
- `read` 先交付已收到的有效数据，再报告正常关闭；暂不可读不返回 EOF。
- 服务端 -1/连接状态语义与 libvalkey 可恢复时返回 0 的语义由各自 adapter
  转换，不能复用未经转换的读写函数。
- transport 的进展处理返回待通知事件，不直接执行 Valkey 命令回调。

接口在实现时用 C、opaque session 和少量函数表达；不公开 liburma 内部对象。
目前 protocol/session 的函数签名已经随实现落地；adapter/runtime 的接口待对应
实现确定。现有基础模块由单元测试调用，尚未接入业务连接路径。

## 已实现的实验帧契约

这是 Valkey 侧的实验流协议，不是 URMA 标准线协议或已有客户端兼容承诺。
所有整数为大端；固定帧头 40 字节，保留字段必须为零，版本严格匹配 1.0。
解码一次只接受一帧，拒绝尾随字节；成功后的 payload 借用输入，不持有其所有权。

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 4 | Magic：ASCII `VKUR` |
| 4 | 2 | 主/次版本：1、0 |
| 6 | 1 | HELLO=1、DATA=2、CREDIT=3、CLOSE=4 |
| 7 | 1 | 保留 |
| 8 | 4 | Payload 长度 |
| 12 | 4 | 保留 |
| 16 | 8 | 非零会话代次 |
| 24 | 8 | DATA 字节偏移或 CLOSE 最终字节偏移；其他类型为零 |
| 32 | 8 | HELLO 必需能力位或 CREDIT 累计授权槽数；其他类型为零 |

HELLO payload 固定 16 字节：支持能力位 8 字节、最大 DATA 长度 4 字节、
本端接收槽数 4 字节。最大 DATA 上限 65536 字节，每端槽数上限 1024；
实际 payload 取双方最小值，发送信用取对端槽数。必需能力必须被双方支持。
DATA 不允许空 payload，偏移加长度不得溢出；CREDIT/CLOSE 不携带 payload。
端点描述、模式选择、随机代次生成及身份认证仍由未来引导流程实现。

`urmaSessionWrite` 接受前缀后返回自有缓冲上的 DATA 和 operation id；临时投递
失败必须保留并重试同一帧，禁止跳过后发送后续帧。本地完成只释放本地发送槽。
`urmaSessionRepostTicket` 给出最早已读完的接收槽票据；runtime 确认真实 repost
成功后才能调用 `urmaSessionReposted` 生成累计 CREDIT。重复信用不重复授权。

当前每会话一次申请 RX/TX 槽和数据，协议上限时约 128 MiB 加元数据；这只是
单会话边界，不是可接受的默认配置。runtime 必须在创建前执行全局内存额度准入，
并计入注册/线缓冲及控制资源开销。模型不管理硬件接收缓冲或全局额度。
`urmaSessionFree` 在 TX 仍被借用时拒绝释放，但不证明 DMA 已停止；runtime
仍须停止所有投递和回调、确认硬件 quiesce，并跟踪控制帧的在途资源。

## 建连与准入

```text
ACCEPTING -> NEGOTIATING -> RESOURCES_READY -> READY
任何失败 -> QUIESCING -> CLOSED
READY -> QUIESCING -> CLOSED
```

轻量引导连接进入 `acceptCommonHandler`，先做 maxclients 和并发握手额度
检查。`connAccept` 异步驱动协商，READY 后恰好一次调用接受完成回调，复用
protected mode、AUTH/ACL 和既有 client 创建流程。设定握手超时，禁止无界资源预分配。
首版不允许创建昂贵资源后才执行 maxclients 检查。

握手交换版本、必需能力、会话随机标识、端点标识、经验证的模式组合、最大帧
和初始信用。长度在分配前校验；不接受未知必需能力或过大长度。
明确固定字节序，不直接发送 C struct。
TCP/TLS 引导不允许默默降级成 TCP 业务；业务提交后不自动重放命令。

引导身份与 Valkey 用户身份分别处理。不能认为 TLS 引导自动加密 URMA 数据，
也不能把 UB Token 当作数据机密性保证。安全模式和部署凭据在 U1 确认。
默认关闭原生监听，配置失败明确报错。

## 数据与流控

DATA 承载 RESP 字节片段，不按 Key 或命令分帧；控制类型包括 CREDIT、CLOSE
及必要的保活。帧有明确长度、会话代次和顺序字段。数据流不得丢字节、插入字节
或改变顺序；目标模式不满足所需保证时拒绝启动或另立有界重排设计。

首版使用定长接收槽，只有接收重新投递成功后才能归还数据信用。
信用使用累计授权值并校验范围、重复与回绕；本地发送资源与远端信用都充足才提交。
控制收发保留独立配额，不能依赖被 DATA 耗尽的额度。
控制帧同样有速率/队列上限，不能因无限保留而耗尽内存。

完成记录用稳定 Operation 标识和 Session generation 归属，防止关闭、重连后的
迟到完成访问已释放对象。默认每操作有可跟踪的完成证据；批量/选择性完成作为
后续优化，必须证明可安全回收全部相关缓冲区。

## 事件、线程和销毁

首版资源只由主线程操作，先不声明 I/O 线程兼容。开启不支持的组合应明确拒绝。
完成事件每轮有预算，重装通知后再检查队列；具体 ack/rearm 顺序按固定 SDK
验证，不照抄可能丢唤醒的样例流程。
`has_pending_data`、`process_pending_data`、write barrier 和 postpone/update_state
必须保持现有契约。只通过 connhelpers 的安全调用机制执行上层 handler。

事件 fd 不一定是 socket。检查地址格式、CLIENT LIST、CLIENT KILL、优先级事件
和所有直接操作 fd 的路径。将引导 IP/port 与实际 EID/设备标识分开，未知 locality
不能当成本地可信连接。

逻辑关闭立即禁止新提交并撤销上层通知；物理释放等待在途操作完成或 API 保证
终止，再撤销远端资源、注销内存并销毁上下文。超时不构成 DMA 已终止的证据。
回调引用与硬件操作引用独立计数。异常退出和不完整初始化也必须幂等清理。

fork 子进程不使用继承的 URMA 资源；必须验证父进程继续通信及子进程持久化安全。
不能仅照搬 ibv_fork_init 就声明 URMA fork 安全。

## 构建与验证

拟增加 Linux 可选构建，默认关闭；启用后检查固定 SDK 的头文件、符号与链接。
不把开发时的 SDK 头文件 vendoring 到 deps，不让依赖浮动到 master。

验证分层：纯协议测试、Session 状态机/资源失败测试、真实 SDK 编译链接、
真实双机 UB 收发、数据库命令/故障/持久化测试。内部 fake 只能证明 Session
契约；真实 provider 路径必须单独验收。最终记录是否走 URMA，不能只凭成功返回。

兼容性标准：RESP2/3、Pipeline、大报文、慢客户端、AUTH/ACL、protected mode、
CLIENT KILL、重连、超时、maxclients、资源耗尽。数据正确性优先于带宽。
完整测试任务和后续 I/O 线程、复制/集群范围见 [任务清单](todo.md)。
