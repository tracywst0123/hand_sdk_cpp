# EtherCAT 网卡配置指南 / Network Port Guide

本文记录 zkkj 灵巧手 EtherCAT 主站（SOEM）所用网口的配置方法、参数含义，以及
2026-09-03 从 Realtek RTL8125 换到 Intel X550 的原因和过程。

---

## 0. 当前配置速查（本机实测）

| 项目 | 值 | 说明 |
|---|---|---|
| EtherCAT 网口 | `enp133s0f1` | Intel X550，PCI `0000:85:00.1` |
| 驱动 / 固件 | `ixgbe` / `0x80000c67, 1.1276.0` | 内核自带 |
| 协商结果 | 100Mb/s, Full, autoneg **on** | EtherCAT 从站固定 100BASE-TX |
| **EEE 节能** | **不支持（驱动未暴露）** | 省一项配置；已实测**不是**长线问题的原因，见 §3.0 |
| 链路稳定性 | `carrier_changes` 增量 ≈ 0 | 对照：Realtek 口同线**每分钟掉 27 次**，见 §3 |
| Flow control (pause) | RX off / TX off | EtherCAT 不能用流控 |
| 中断合并 rx-usecs | `1` | 近乎每包中断，低延迟 |
| Ring buffer | RX 512 / TX 512 | 默认值，够用 |
| NetworkManager | **unmanaged** | 见 `/etc/NetworkManager/conf.d/99-ethercat-unmanaged.conf` |
| Franka FR3 网口 | `enp132s0` | Realtek RTL8125，`r8169`，走 IP 不走 EtherCAT |
| 备用口 | `enp133s0f0` | X550 另一口，未使用，仍归 NM 管 |

实测通信量（一次正常运行后）：

```
RX:  bytes packets errors dropped  missed   mcast
  51451928  186140      0       0       0       0
TX:  bytes packets errors dropped carrier collsns
  51243417  186335      0      42       0       0
```

18 万包、**0 错误 0 丢包**（TX 那 42 个 dropped 是链路 down 期间往下发包造成的
qdisc 丢弃，属正常，不是线路质量问题）。

---

## 1. 硬件与网口命名

```
84:00.0 Realtek RTL8125 2.5GbE      -> enp132s0     (driver r8169)   给 FR3
85:00.0 Intel X550                  -> enp133s0f0   (driver ixgbe)   备用
85:00.1 Intel X550                  -> enp133s0f1   (driver ixgbe)   EtherCAT
```

网口名是 systemd 的 **Predictable Network Interface Names** 规则生成的：
`en` = ethernet，`p133` = PCI bus 133，`s0` = slot 0，`f1` = function 1（双口卡的
第二个口）。所以名字由**插槽位置**决定 —— 换 PCIe 插槽名字就会变，程序里写死的
`interface_name` 也要跟着改。

查当前对应关系：

```bash
ip -br link                                    # 所有网口和状态
lspci | grep -i eth                            # PCI 上的网卡
ethtool -i enp133s0f1                          # 某个口用什么驱动
ethtool -p enp133s0f1 10                       # 让该口的 LED 闪 10 秒，物理定位
```

`ethtool -p` 在插了好几根线不知道哪根是哪根的时候特别有用。

---

## 2. 换一个新网口的完整步骤

### 2.1 插卡，确认系统认出来了

```bash
lspci | grep -i eth
ip -br link
dmesg | grep -iE "ixgbe|r8169"
```

确认新口出现，并且驱动加载成功。X550 正常应该看到：

```
ixgbe 0000:85:00.1: Intel(R) 10 Gigabit Network Connection
ixgbe 0000:85:00.1 enp133s0f1: renamed from eth1
```

### 2.2 插网线，确认物理链路

```bash
sudo ip link set enp133s0f1 up
ethtool enp133s0f1 | grep -E "Speed|Duplex|Link detected"
```

期望结果：

```
Speed: 100Mb/s
Duplex: Full
Link detected: yes
```

EtherCAT 从站的 ESC（EtherCAT Slave Controller）**只支持 100BASE-TX 全双工**，
所以协商出 100Mb/s Full 是正确的，不是"速度没跑满"。X550 是万兆卡，能降到
100M 协商上是它支持 `100baseT/Full` 这个 link mode 的结果 —— 不是所有万兆卡都
支持 100M，有些只支持 1G 起步，那种卡**不能**直接接 EtherCAT 从站。

买卡前必须确认这一行：

```bash
ethtool enp133s0f1 | grep -A6 "Supported link modes"
#   Supported link modes:   100baseT/Full    <-- 必须有这一行
```

### 2.3 把网口从 NetworkManager 手里拿走

这一步是**必须的**。NetworkManager 会对它管理的网口反复尝试 DHCP，
每 45 秒一轮 `prepare -> config -> ip-config -> failed`，每一轮都会重新配置网卡，
正好把 SOEM 的扫描打断，表现为"时好时坏"。

```bash
sudo nano /etc/NetworkManager/conf.d/99-ethercat-unmanaged.conf
```

内容（多个口用 `;` 分隔）：

```ini
[keyfile]
unmanaged-devices=interface-name:enp132s0;interface-name:enp133s0f1
```

然后删掉 NM 自动生成的那个有线连接，并重载：

```bash
nmcli -t -f NAME,DEVICE con show                     # 找到 "Wired connection N"
sudo nmcli con delete "Wired connection 3"
sudo systemctl reload NetworkManager
nmcli -t device status | grep enp                    # 应显示 unmanaged
```

### 2.4 ⚠️ unmanaged 之后必须自己把网口拉起来

**这是最容易踩的坑。** 一旦 unmanaged，没有任何东西负责给网口置 `IFF_UP`，
所以每次重启后网口都是 `DOWN`，SOEM 会报 `ecx_init failed` 或"扫不到从站"。

而且 100BASE-TX 自协商要 **2~3 秒**，`ip link set up` 之后立刻扫描会扫不到，
必须等 carrier：

```bash
sudo ip link set enp133s0f1 up
until [ "$(cat /sys/class/net/enp133s0f1/carrier)" = 1 ]; do sleep 1; done
```

开机自动拉起（systemd oneshot）：

```ini
# /etc/systemd/system/ethercat-nic.service
[Unit]
Description=Bring up EtherCAT NIC enp133s0f1
After=network-pre.target
Wants=network-pre.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/sbin/ip link set enp133s0f1 up

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ethercat-nic.service
```

### 2.5 改代码里的网口名

以下位置都写了网口名，换口时要一起改（2026-09-03 已全部更新）：

| 文件 | 位置 |
|---|---|
| `hand_sdk_cpp/include/config.hpp` | `Config::interface_name` 默认值 |
| `hand_sdk_cpp/examples/main.cpp` | `config.interface_name` |
| `hand_sdk_cpp/examples/bit_io_test.cpp` | `argv[1]` 的默认值 |
| `hand_sdk_ros2/src/hand_sdk_ros2_node.cpp` | `declare_parameter("interface_name", ...)` |

ROS 节点也可以不改代码，启动时传参：

```bash
ros2 run hand_sdk_ros2 hand_sdk_ros2_node --ros-args -p interface_name:=enp133s0f1
```

### 2.6 给程序 raw socket 权限

SOEM 用 `AF_PACKET` raw socket，需要 `CAP_NET_RAW`：

```bash
sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+ep <可执行文件>
getcap <可执行文件>                                   # 确认生效
```

三个 capability 的含义：

- `cap_net_raw` —— 开 raw socket，**必须**，没有就 `ecx_init failed`
- `cap_net_admin` —— 配置网卡（SOEM 某些路径要用）
- `cap_sys_nice` —— 设 `SCHED_FIFO` 实时优先级（`Config::rt_priority` 默认 80），
  没有的话程序照跑，但抖动变大，日志里出现 `SCHED_FIFO failed`

⚠️ 两个坑：
1. **每次重新编译，setcap 都会失效**，必须重新执行。
2. **setcap 过的程序会丢弃 `LD_LIBRARY_PATH`**（内核对 setuid/setcap 程序的安全
   限制），ROS 节点因此可能加载不到库。ROS 节点更稳妥的做法是直接用 `sudo` 跑，
   或者把库路径写进 `/etc/ld.so.conf.d/`。

### 2.7 验证

```bash
sudo ./ecinfo.py enp133s0f1
```

正常输出（从站还没被主站配置时就该是 INIT）：

```
slaves    : 1
--- slave 0 ---
  ESC type/rev : 0xc0 / 0x02    FMMUs 3  SyncManagers 4
  station addr : 0x0000
  DL status    : 0x5617   p0=link p1=open p2=open p3=open
  AL status    : 0x0001 -> INIT
  AL status码  : 0x0000 (no error)
```

---

## 3. 长网线在 Realtek 口上为什么不稳（2026-09-03 实测）

### 3.0 结论：链路根本保持不住，不是数据被打错

用 `tools/eclinktest.sh` 在 **同一根长网线、同一个从站** 上对 Realtek 口
(`enp132s0`) 做了 3000 帧 / 1 ms 周期的压力测试，结果如下：

| 指标 | EEE off | EEE on |
|---|---|---|
| 发送 | 3000 | 3000 |
| 应答 | 2813 | 2821 |
| **丢失** | **187 (6.23 %)** | **179 (5.97 %)** |
| WKC 异常 | 0 | 0 |
| RTT p50 | 34.9 µs | 34.6 µs |
| RTT p99 | 253.8 µs | 280.1 µs |
| **`rx_crc_errors`** | **0** | **0** |
| **`carrier_changes`** | **+4** | **+4** |

空闲状态（完全不发包）下继续观察 20 秒：

```
carrier_changes: 87 -> 105  over 20 s
flap rate: 27.0 up/down cycles per minute
rx_crc_errors = 0    rx_errors = 0    rx_frame_errors = 0
```

内核日志是这样的，每 2 秒一轮，无限循环：

```
r8169 0000:84:00.0 enp132s0: Link is Up - 100Mbps/Full - flow control off
r8169 0000:84:00.0 enp132s0: Link is Down
r8169 0000:84:00.0 enp132s0: Link is Up - 100Mbps/Full - flow control off
r8169 0000:84:00.0 enp132s0: Link is Down
```

**三个结论，都是实测的：**

1. **故障机理是"链路反复重新协商"，不是"数据被干扰打错"。**
   每分钟掉线重连 **27 次**，即使一个包都不发也照掉。那 6 % 的丢包
   **完全是掉线窗口造成的** —— 链路活着的时候数据通路是干净的：
   WKC 恒为 1、无异常、RTT p50 只有 35 µs。

2. **`rx_crc_errors` 全程为 0。** 这一点很反直觉，也是我上一版结论错的地方：
   我原本让你去查 CRC 误码计数器，**但这个故障下 CRC 计数器永远是 0**，
   照那个方法查会得出"线路很干净"的错误结论。

3. **EEE 确实无关。** 6.23 % vs 5.97 %，差异在噪声范围内，
   和你自己 `--set-eee off` 之后依然不通的结论完全一致。EEE 这条彻底排除。

### 3.1 为什么 CRC 是 0 却还是断？

因为 **100BASE-TX 的"链路存在"判定和"数据正确"是两回事**。

100BASE-TX 在没有数据的时候，线上跑的是**连续的 idle 码流**（4B/5B 编码 +
MLT-3 + 加扰）。接收端 PHY 必须持续地：

- 锁住**解扰器 (descrambler)**
- 锁住**时钟恢复 (CDR)**
- 让**自适应均衡器**收敛

**这三件事任何一个失锁，PHY 立刻判定 link down 并重新训练**，而不是继续收一堆
错包。所以信号裕度不足时，典型表现是：

> **先掉链路，而不是先出 CRC 错误。**

这就是为什么 `rx_crc_errors = 0` 却每分钟断 27 次 —— 帧只在链路稳的那段时间发，
那时候信号是好的；信号一变差，链路直接没了，压根没有帧被"打错"的机会。

**所以排查顺序必须是：先看 `carrier_changes`，再看 CRC。** 只看 CRC 会漏掉这
一整类故障。

### 3.2 还没区分清楚的：是线的问题，还是这块网卡的问题？

到这一步能确定"Realtek 口保持不住链路"，但**还不能确定根因是哪一个**：

| 候选 | 说明 | 怎么证伪 |
|---|---|---|
| **A. PHY 接收裕度不足** | 线太长/线材差，RTL8125 的均衡能力不够，解扰器反复失锁 | 同一根线插到 X550 上测，若稳 → 是网卡 PHY 差异 |
| **B. r8169/RTL8125 驱动或固件 bug** | RTL8125 在 `r8169` 下有公开的链路不稳问题；本机固件 `rtl8125d-1_0.0.7` | 同上；或换 Realtek 官方 `r8125` 驱动对比 |
| **C. 线缆/水晶头本身故障** | 断芯、虚接、CCA 铜包铝 | 换一根等长的好线测 |

**决定性实验：把这根长线插回 X550，跑同样的命令**

```bash
sudo ./eclinktest.sh enp133s0f1 3000 1
```

- 若 X550 上 `carrier_changes` 增量 ≈ 0、丢包 ≈ 0 → **根因是网卡（A 或 B）**，
  换卡就是正解，本文 §0 的配置继续用。
- 若 X550 上也反复掉链路 → **根因是线**（C），换卡只是掩盖了问题，
  必须换线或按 §3.3 改拓扑。

⚠️ 之前 X550 那次 18 万包 0 错误的记录，**当时用的是不是这根长线并不确定**，
所以不能拿来直接当对照。必须重跑一次才算数。

### 3.3 如果确认是线长/线材问题

按性价比排序：

1. **换线材**：用 **Cat5e/Cat6 实心纯铜**工业级屏蔽线 (SF/UTP 或 S/FTP)。
   市面上大量 **CCA（铜包铝）**线缆衰减远超标称，是长线失败的头号元凶 ——
   检查线身印字有没有 "CCA"。
2. **缩短单段长度**：EtherCAT 每个从站的 ESC 都会**再生信号 (regeneration)**，
   不是无源集线器。中间插一个从站或 EtherCAT 分支模块，就把一段长线拆成两段，
   **每段重新享有完整的 100 m 预算**。这是 EtherCAT 拓扑的天然优势，通常比换线
   更彻底。
3. **走光纤**：超过 100 m 就上 100BASE-FX 转换器，顺带隔离地环流和电磁干扰。
4. **远离动力线**：与伺服动力线、变频器输出平行走线是最强干扰源，必须分槽，
   交叉时走 90°。

### 3.4 其它因素（与线长无关，一并记录）

这些都**不能**解释"短线好长线坏"，但影响周期稳定性：

- **EEE（802.3az）**：已实测排除，见 §3.0。仍建议在消费级网卡上关掉，属卫生措施。
- **PCIe ASPM**：Realtek 卡常默认开 L1/L1.1/L1.2，恢复要几十 µs。
  查看需 root：`sudo lspci -vv -s 84:00.0 | grep -i aspm`。
  内核参数 `pcie_aspm=off` 可全局关闭，但影响整机功耗。
- **驱动实时性**：`ixgbe` 是 Intel 官方服务器驱动；`r8169` 是社区驱动。
  本次测出 Realtek 在链路正常时 RTT p50 仅 35 µs，**数据通路本身并不慢**，
  所以驱动延迟不是本次故障的主因。
- **中断与队列**：X550 有 24 个 RX/TX 队列和独立 MSI-X，`rx-usecs` 默认 `1`，
  适合小包低延迟负载。

### 3.5 本次排查的三次结论修正（留作教训）

| 版本 | 结论 | 为什么错 |
|---|---|---|
| 第 1 版 | "EEE 是长线丢帧的原因" | 只因为它是**唯一测得出**的差异就下结论；用户实测关掉 EEE 无效 |
| 第 2 版 | "PHY 裕度不足导致 CRC 误码" | 方向对了，但**机理猜错**：实测 CRC 恒为 0，按 CRC 排查会误判成"线路干净" |
| 第 3 版（当前） | "链路每分钟重协商 27 次，数据通路本身干净" | 实测 `carrier_changes`、丢包率、RTT、CRC 四项交叉验证 |

**教训：先量出故障的"形状"（掉链路 vs 误码 vs 延迟），再去解释原因。**
`carrier_changes` 这个计数器在前两版里一次都没查过。

---

## 4. 各项 ethtool 设置的含义

### 4.1 速率 / 双工 / 自协商

```bash
sudo ethtool -s enp132s0 speed 100 duplex full autoneg on
```

逐词解释：

- `-s` / `--change` —— 修改网卡设置（区别于 `-S` 是看统计）
- `speed 100` —— 期望协商到 100 Mb/s
- `duplex full` —— 全双工（收发同时进行）。EtherCAT **必须全双工**，
  半双工会有冲突检测和退避，周期性通信直接崩
- `autoneg on` —— 开启自协商（IEEE 802.3 Clause 28）。双方交换能力，选出共同支持
  的最高配置

**关于要不要写死速率：** 一般**建议保持 `autoneg on`**，因为：

1. EtherCAT 从站 ESC 只支持 100BASE-TX Full，自协商本来就只会得到这个结果；
2. 100BASE-TX 关掉自协商后，**MDI/MDI-X 自动翻转也会失效**，必须用对交叉线/直连线；
3. 很多现代网卡（包括 X550）**根本不允许强制速率**，`autoneg off` 会直接报错。

只有在对端设备协商有 bug、每次都协商错的时候，才考虑强制。本机 `enp133s0f1`
用的是 `autoneg on`，协商结果 100Mb/s Full，正确。

查看：

```bash
ethtool enp133s0f1 | grep -E "Speed|Duplex|Auto-negotiation"
```

### 4.2 EEE 省电

**含义：** IEEE 802.3az，链路空闲时 PHY 进 LPI 低功耗态，唤醒要 20~30 µs，
可能给周期性通信引入延迟。EtherCAT 场景建议关闭，属于卫生措施。

```bash
ethtool --show-eee enp133s0f1                  # 查看
sudo ethtool --set-eee enp132s0 eee off        # 关闭（X550 不支持此选项，无需操作）
```

⚠️ **不要把 EEE 当成万能解释。** 本机已实验验证：在 Realtek 口上关掉 EEE，
长网线依然不通 —— 长线故障是 PHY 物理层裕度问题，见 §3.0 / §3.1。
EEE 与线长无关。

### 4.3 Offload 卸载功能

网卡硬件代替 CPU 做的事。查看：

```bash
ethtool -k enp133s0f1
```

本机 `enp133s0f1` 现状与是否要动：

| 选项 | 现状 | 含义 | 对 EtherCAT 的影响 |
|---|---|---|---|
| `rx/tx-checksumming` | on | 硬件算/校验 IP/TCP/UDP 校验和 | **无影响**。EtherCAT 是 EtherType `0x88A4` 的裸以太帧，没有 IP/TCP 层，硬件不会去碰它 |
| `generic-receive-offload` (GRO) | on | 把多个同流小包合并成大包再交给协议栈 | **无影响**。GRO 只合并 TCP/UDP 流，不认识 `0x88A4` |
| `large-receive-offload` (LRO) | off | 硬件层面的包合并，比 GRO 更激进 | 建议保持 off |
| `tcp-segmentation-offload` (TSO) | on | 硬件切分大 TCP 段 | 无影响，EtherCAT 不走 TCP |
| `rx/tx-vlan-offload` | on | 硬件插入/剥离 VLAN tag | EtherCAT 帧不带 VLAN，无影响 |
| `rx-vlan-filter` | on | 硬件按 VLAN ID 过滤 | 无 VLAN tag 的帧不受影响 |
| `receive-hashing` (RSS) | on | 按流哈希分散到多个队列 | 无影响，单流 |

**结论：本机不需要关任何 offload。** 网上很多 EtherCAT 教程让你把 offload 全关，
那是针对特定老网卡/老驱动的经验。X550 + ixgbe 上没必要，实测 18 万包 0 错误。

真要关（排查时用）：

```bash
sudo ethtool -K enp133s0f1 gro off gso off tso off lro off
```

（`-K` 改 offload，`-k` 查 offload）

### 4.4 中断合并 Interrupt Coalescing

```bash
ethtool -c enp133s0f1          # 查看（-C 修改）
```

本机：`rx-usecs: 1`，`tx-usecs: 0`。

**含义：** `rx-usecs` 是收到包后延迟多少微秒才触发中断，攒一批一起处理。
数值大 = 吞吐高、CPU 占用低、**延迟大**；数值小 = 延迟低、中断多、CPU 占用高。

EtherCAT 要的是**低延迟和确定性**，不是吞吐，所以 `rx-usecs=1` 正合适。
如果遇到周期抖动大，可以确认它没有被改成大值：

```bash
sudo ethtool -C enp133s0f1 rx-usecs 1 tx-usecs 0
```

⚠️ ixgbe 上把 `rx-usecs` 设成 `0`（完全关闭合并）反而可能触发中断风暴，`1` 是
推荐值。

### 4.5 Ring Buffer 环形缓冲区

```bash
ethtool -g enp133s0f1          # 查看（-G 修改）
```

本机 RX 512 / TX 512（硬件上限 32768）。

**含义：** 网卡和驱动之间的描述符队列深度。**大 = 抗突发能力强但延迟可能变大**，
小 = 延迟低但容易丢包。EtherCAT 每周期就一两个帧，512 远远够用，**不需要调**。

（对比：高吞吐服务器场景常调到 4096；EtherCAT 反而不要调大。）

### 4.6 Flow Control 流控 (pause frames)

```bash
ethtool -a enp133s0f1          # 查看（-A 修改）
```

本机：`RX: off  TX: off` —— **正确**。

**含义：** 802.3x pause frame，接收方缓冲区快满时发 pause 帧让对方暂停发送。
**EtherCAT 绝对不能开流控**：一个 pause 帧会让主站停止发送若干毫秒，周期直接被
破坏，从站 watchdog 超时掉线。从站 ESC 也不理解 pause 帧。

确保关闭：

```bash
sudo ethtool -A enp133s0f1 rx off tx off autoneg off
```

### 4.7 混杂模式 Promiscuous Mode

`ip -br link` 里能看到 `PROMISC` 标志。抓包工具（tcpdump）和某些 SOEM 版本会打开
它。EtherCAT **不强制需要**混杂模式：从站返回的帧目的 MAC 是广播
`ff:ff:ff:ff:ff:ff`，本来就收得到。看到 `PROMISC` 残留是正常的，不影响功能。

### 4.8 MTU

保持默认 1500。EtherCAT 帧最大 1518 字节（含以太头），标准 MTU 完全够。
**不要开 jumbo frame。**

---

## 5. 诊断脚本说明

脚本都在 [`tools/`](tools/) 目录下，直接用：

```bash
cd src/zkkj_hand_native/hand_sdk_cpp/tools
sudo ./up.sh enp133s0f1          # 拉起网口并等协商
sudo ./ecscan.py enp133s0f1      # 最快判断：总线上有没有从站
sudo ./ecinfo.py enp133s0f1      # 详细：每个从站的 AL 状态和错误码
sudo ./nmfix.sh                  # 把网口从 NetworkManager 摘出来（会自动调 up.sh）
sudo ./persist.sh enp133s0f1     # 装 systemd 服务，开机自动拉起
sudo ./ecstress.py enp132s0 --count 3000 --period-ms 1   # 压力测试：丢包率/RTT/误码
sudo ./eclinktest.sh enp132s0 3000 1                     # 完整链路体检（含 EEE A/B）
```

`ecscan.py` / `ecinfo.py` **只读**，不会改从站任何状态，可以在不确定的时候放心跑。

### `up.sh` —— 拉起网口并等待协商

```bash
ip link set "$IFACE" up
for i in $(seq 1 15); do
    if [ "$(cat /sys/class/net/$IFACE/carrier)" = "1" ]; then break; fi
    sleep 1
done
```

关键在**等 carrier**。`/sys/class/net/<if>/carrier` 是内核直接暴露的物理链路状态
（`1`=有载波）。100BASE-TX 自协商约 3 秒，不等就扫描必然失败。

### `nmfix.sh` —— 把网口从 NetworkManager 摘出来

写 `99-ethercat-unmanaged.conf`、删掉自动生成的 `Wired connection N`、
`systemctl reload NetworkManager`。

⚠️ 这个脚本**执行完网口会变成 DOWN** —— NM 撒手了，就没人再给它置 `IFF_UP`。
第一版漏了这步，导致网口直接掉线；现在脚本末尾会自动调用 `up.sh` 补上。

### `persist.sh` —— 装 systemd 服务实现开机自动拉起

见 §2.4。

### `ecscan.py` —— 最小 EtherCAT 探测（只读）

发一个 **BRD (Broadcast Read)** 读寄存器 `0x0000`，看有没有从站应答、
working counter 是多少。**不改任何东西**，是判断"物理链路到底通不通"的最快手段。

原理：EtherCAT 从站的 ESC 是**直通转发**的硬件，帧穿过它时被就地修改再转发出去。
BRD 的 working counter 每经过一个从站 +1，所以 `WKC=1` 就代表总线上有 1 个从站。
返回帧的源 MAC 会被 ESC 把 locally-administered 位置 1（本机看到
`a0:36:...` 变成 `a2:36:...`），这是帧确实穿过从站的铁证。

### `ecstress.py` —— 链路压力测试（只读）

按固定周期连发 N 个 BRD，统计**丢包率、RTT 分布 (p50/p99/p99.9)、抖动**，
并对比测试前后的 NIC 错误计数器和 `carrier_changes`。

它的价值在于能**区分故障类型**：

| 现象 | 指向 |
|---|---|
| `carrier_changes` 增长 | 链路保持不住 —— 信号裕度或网卡/驱动问题（§3.1） |
| `rx_crc_errors` 增长 | 数据被干扰打错 —— 线材/干扰 |
| 丢包但计数器全不动 | 帧没发上线 —— 驱动或调度问题 |
| RTT 尾部很长但不丢包 | 实时性问题 —— `SCHED_FIFO`、中断合并 |

### `eclinktest.sh` —— 一键链路体检

拉起网口 → 等协商 → 打印驱动/EEE 信息 → **在 EEE off 和 EEE on 两种设置下各跑
一遍 `ecstress.py`** → 汇总 `carrier_changes` 和内核链路事件。
做网卡/线缆对照实验时用这个，保证两次测试条件一致。

### `ecinfo.py` —— 只读总线扫描

用 **APRD (Auto-increment Physical Read)** 逐个读从站的 ESC 寄存器：

| 寄存器 | 内容 | 排障价值 |
|---|---|---|
| `0x0000/0x0001` | ESC type / revision | 确认从站芯片型号 |
| `0x0004/0x0005` | FMMU / SyncManager 数量 | 确认 ESC 能力 |
| `0x0010` | Configured Station Address | 主站配过为非 0 |
| `0x0110` | DL Status（bit 4~7 = 端口 0~3 物理链路） | 看拓扑，几根线接在哪 |
| `0x0130` | **AL Status**（1=INIT 2=PREOP 4=SAFEOP 8=OP，bit4=ERROR） | 从站卡在哪一级 |
| `0x0134` | **AL Status Code** | 卡住的**原因**，最关键 |

常见 AL Status Code：

| 码 | 含义 |
|---|---|
| `0x0000` | 无错误 |
| `0x0011` | 非法的状态切换请求 |
| `0x0016` | 邮箱配置错误（进 PREOP 失败） |
| `0x001B` | **SM watchdog 超时** —— 主站周期没跟上，或丢帧 |
| `0x001E` | 输入配置无效 |
| `0x0024` | 输入 PDO 映射错误 |
| `0x002D` / `0x004D` | **Sync error** —— DC 同步出问题 |
| `0x0030` / `0x0035` | DC 同步配置 / Sync0 周期非法 |

`0x001B` 和 `0x002D` 正是**丢帧**最典型的表现 —— 如果换网卡前老在这两个码上掉线，
基本可以确认是物理层问题（线材、线长、干扰）而不是代码问题；
配合 `ethtool -S <if> | grep -i crc` 看误码计数器一起判断。

---

## 6. 故障排查速查表

按这个顺序查，**从下往上**（先物理层再软件）：

| 现象 | 先查什么 | 命令 |
|---|---|---|
| `ecx_init failed: could not open interface` | 网口是否 DOWN；有没有 `cap_net_raw` | `ip -br link` / `getcap <bin>` |
| 扫不到从站（no slaves found） | carrier、协商速率 | `ethtool <if> \| grep -E "Speed\|Link detected"` |
| **时好时坏** | NetworkManager 是否还在管这个口 | `nmcli -t device status` / `journalctl -u NetworkManager \| grep <if>` |
| 重启后就不通 | unmanaged 后没人拉起网口 | `systemctl status ethercat-nic` |
| 运行中掉出 OP，`0x001B`/`0x002D` | **先看 `carrier_changes`**（链路是否在反复重连），再看 CRC、流控 | `cat /sys/class/net/<if>/carrier_changes` 隔 20 s 读两次 |
| 链路每隔几秒自己断一次 | 信号裕度不足或网卡/驱动问题，**CRC 可能全是 0** | `journalctl -k \| grep "Link is"` / `tools/eclinktest.sh` |
| 丢包但 CRC 全 0 | 别下"线路干净"的结论 —— 先排除链路重协商，见 §3.1 | `tools/ecstress.py <if>` |
| 周期抖动大 | `SCHED_FIFO` 是否生效、中断合并 | 日志里的 `SCHED_FIFO failed` / `ethtool -c <if>` |
| RX 计数一直是 0 | **正常现象**，不代表网卡坏 | 见下 |
| 换网口后代码还连旧口 | 代码里写死的网口名 | `grep -rn "enp1" src/` |

### 关于 "RX packets 全是 0"

这是个非常容易误判的现象。**ESC 会丢弃非 EtherCAT 帧**（DL Control 寄存器
`0x0100` 可配），所以你发出去的 DHCP、ARP 广播都**不会**被回传，RX 计数就是 0。

**RX=0 完全不能证明网卡坏了。** 唯一可靠的判据是发一个真正的 EtherCAT 帧看
WKC —— 也就是 `ecscan.py` 干的事。2026-09-03 这次排查里，RX=0 一度看着很吓人，
实际总线一直是好的。

---

## 7. 参考命令汇总

```bash
# ---- 查看 ----
ip -br link                                  # 网口和状态一览
ip -s link show enp133s0f1                   # 收发计数、错误、丢包
ethtool enp133s0f1                           # 速率/双工/协商/支持的 link modes
ethtool -i enp133s0f1                        # 驱动和固件版本
ethtool -S enp133s0f1                        # 详细硬件统计
ethtool -k enp133s0f1                        # offload 状态
ethtool -c enp133s0f1                        # 中断合并
ethtool -g enp133s0f1                        # ring buffer
ethtool -a enp133s0f1                        # 流控
ethtool --show-eee enp133s0f1                # EEE 省电
cat /sys/class/net/enp133s0f1/carrier        # 1 = 物理链路已建立
journalctl -k | grep -i "NIC Link"           # 链路 up/down 历史

# ---- 修改（EtherCAT 推荐值）----
sudo ip link set enp133s0f1 up
sudo ethtool --set-eee enp133s0f1 eee off    # 关省电（X550 不支持该选项，跳过）
sudo ethtool -A enp133s0f1 rx off tx off     # 关流控
sudo ethtool -C enp133s0f1 rx-usecs 1        # 低延迟中断

# ---- 物理定位 ----
sudo ethtool -p enp133s0f1 10                # 让网口 LED 闪 10 秒
```

---

## 8. 已知遗留问题

- 总线上目前只有 **1 个从站**，但 `Config` 里同时定义了 `left_hand_name`
  (`hand_200_L`) 和 `right_hand_name` (`hand_200_R`)。如果预期是两只手，需要单独
  排查另一只。
- `bit_io_test` 是给简单 bit-IO 从站（8 DI / 8 DO）写的诊断程序，**PDO 布局和
  21 关节的手不一样**，不要拿它直接连手，会往输出区写数据。
- `enp133s0f0`（X550 另一个口）目前仍归 NetworkManager 管，未使用。若将来做
  EtherCAT 冗余（redundancy）或接第二条总线，需要同样做 unmanaged + 开机拉起。

---

*最后更新：2026-09-03 —— Realtek RTL8125 (`enp132s0`) 换为 Intel X550
(`enp133s0f1`)，FR3 保留在 `enp132s0`，两者不再争抢同一块网卡。*
