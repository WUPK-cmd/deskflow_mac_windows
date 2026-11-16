# 自制版 Deskflow：Windows 与 Mac mini 使用手册

适用于本仓库的 Windows 服务端 + Mac 客户端：共用 Windows 的键鼠、双向文字/图片剪贴板，以及让 Windows 麦克风跟随鼠标切换。

源码仓库：[WUPK-cmd/deskflow_mac_windows](https://github.com/WUPK-cmd/deskflow_mac_windows)。
截至 2026-11-16，使用者已确认双向剪贴板、麦克风共享正常，改用网线直连后鼠标明显顺滑。本手册记录恢复这套环境的方法；实际延迟仍取决于网络、音频设备和应用缓冲，不能理解为测得了零延迟。

## 1. 先了解连接方式

```text
Windows 笔记本                           Mac mini
键盘、鼠标、物理麦克风                    接收键鼠和麦克风
Deskflow：服务器                        Deskflow：客户端
有线：192.168.250.1/24  ← 一根网线 →      有线：192.168.250.2/24
Wi-Fi：继续上网                         Wi-Fi：继续上网
```

Deskflow 的“服务器”就是接着键鼠的 Windows，不需要额外租服务器或部署云端服务。两台电脑需要网络互通；可以使用同一局域网，也可以像上图一样直连。公司 Wi-Fi 如果隔离设备，可能无法互连；已接网线时优先使用有线地址。

笔记本没有网口时使用 USB 转以太网适配器。普通 USB-A ↔ USB-C 数据线不能直接代替这条网络连接。网线只承载两台机器间的数据，两端仍可各自通过 Wi-Fi 上网，不需要开启 Internet 连接共享或网络桥接。

## 2. 安装与版本选择

普通使用者可直接下载本仓库 [Releases](https://github.com/WUPK-cmd/deskflow_mac_windows/releases) 中经过双端验收的安装包，无需自行编译：

| 设备 | 下载文件 | 使用方式 |
| --- | --- | --- |
| Windows x64，原本使用便携版 | `win-x64-portable.zip` | 解压到新目录，保留旧目录；停止并退出旧 GUI/Core 后，复制原 `settings` 目录到新目录，再启动新 `deskflow.exe` |
| Windows x64，全新安装或需要服务模式 | `win-x64.msi` | 先备份并退出旧版，安装对应 VC++ 运行库，再运行 MSI；从便携版切换时需另外迁移或重新配置设置 |
| Apple Silicon Mac，macOS 14+ | `macos-arm64.dmg` 或 `macos-arm64.zip` | 备份旧 `Deskflow.app` 及用户配置，退出旧 GUI/Core；DMG 中将应用拖到“应用程序”，或用 `ditto -x -k` 解压 ZIP 后复制应用 |

完整文件名包含构建提交，表中列出的是文件名结尾。选择同一个 Release 的双端包，并用 `SHA256SUMS.txt` 核对文件。Windows 可运行 `Get-FileHash -Algorithm SHA256 <下载文件>`，Mac 可运行 `shasum -a 256 <下载文件>`。

已有便携版建议继续选择便携包，方便保留当前工作配置。若证书设置引用了旧程序目录的绝对路径，请在新版本的“网络 → 安全”中选择复制过来的**同一份证书文件**，避免删除旧目录后路径失效；不要重新生成证书。Mac 的用户配置保存在应用包之外，替换应用时保留原配置与 TLS 配对资料，按系统提示给新应用重新确认所需权限。

Windows 包未签名，Mac 包采用临时签名、未经 Apple 公证，具体限制见 Release 说明。安装包不附带 VB-CABLE/BlackHole 驱动，也不会自动安装它们。测试包先用于保留备份后的验收，不要同时启动新旧两个版本。

**应用控制限制**：如果 Windows 报 `WinError 4551` 或“应用程序控制策略已阻止此文件”，说明系统要求认可的签名/信任条件。此开发设备实测会拦截下载的未签名 Windows 包，而已有的本地编译版本可以运行。应使用符合管理员策略的签名，或按本仓库流程在本地编译；不要为运行此程序关闭系统的应用控制策略。CI 的启动检查通过不能代替这一设备策略检查。

1. 两端使用本仓库同一已验收版本。从源码构建时按[构建、更新和回退说明](SelfBuilt.md)准备依赖并编译；更换 Qt、编译器或 CPU 架构时使用新构建目录。
2. 安装包查看内置 `build-info.json` 的 `commit`；源码构建在两端目录执行 `git rev-parse HEAD`。确认启动的是选定版本的应用及其 Core，避免旧版后台进程占用连接。
3. Windows 安装对应架构的 Visual C++ 运行库；Mac 按提示给实际运行的应用授予辅助功能/输入监控权限，使用麦克风的应用还需要麦克风权限。
4. Windows 选择“服务器”，Mac 选择“客户端”。在 Windows 的服务器屏幕布局里加入 Mac；屏幕名称必须与 Mac 配置的 Deskflow 计算机名称一致，再按实际左右位置摆放。
5. 开启 TLS，连接时核对对端显示的证书指纹后再信任。为麦克风功能保留客户端指纹校验。

### 麦克风版本特别说明

**源码选项 `BUILD_MICROPHONE_FOLLOW` 默认是 `OFF`；本仓库新的 Self-built Actions 构建已显式开启它，并加入 Qt Multimedia。** 下载时选择同一提交的双端包，检查 `build-info.json` 中 `microphoneFollow` 为 `true`。旧的 Actions 包可能没有麦克风功能，不能只看版本号判断。

在 [SelfBuilt.md](SelfBuilt.md) 的构建流程中额外做以下操作：

- 两端都安装与现有 Qt **版本、架构完全一致**的 Qt Multimedia 组件。使用文档中的 aqt 安装命令时，在 `install-qt` 命令末尾加 `-m qtmultimedia`。
- 在两端完整的 CMake 配置命令中加入 `-DBUILD_MICROPHONE_FOLLOW=ON`，保留原有 Qt、OpenSSL、编译器和架构参数，然后重新构建、安装/打包。
- 发布目录需要带上 Qt Multimedia 运行库及对应音频插件；沿用构建文档的部署流程。不能只复制一个新的 `deskflow-core` 可执行文件覆盖旧版。
- 在实际配置并打开音频设备后验收麦克风；编译通过、Core 能启动都不能代替音频验收。

## 3. 网线直连，Wi-Fi 继续上网

以下地址是一个示例。如 Wi-Fi、VPN 或其他网卡已使用 `192.168.250.0/24`，请两端一起改用不冲突的私有网段。

| 项目 | Windows 有线网卡 | Mac 有线网卡 |
| --- | --- | --- |
| IPv4 配置方式 | 手动 | 手动 |
| IP 地址 | `192.168.250.1` | `192.168.250.2` |
| 子网掩码 | `255.255.255.0` | `255.255.255.0` |
| 前缀长度（界面要求时） | `24` | `24` |
| 网关/路由器 | 留空 | 留空 |
| DNS | 留空 | 留空 |

Windows 在网络设置中选中**有线适配器**修改 IPv4；Mac 在“系统设置 → 网络 → 对应以太网 → 详细信息 → TCP/IP”中手动配置。保留两边 Wi-Fi 的原有自动获取地址、路由器和 DNS 设置。

Mac 的 Deskflow 服务器地址填 `192.168.250.1`，端口为 `24800`。如果界面把端口单独列出，分别填写；不要继续使用解析到 Wi-Fi 的主机名或自动发现结果。

Windows 防火墙若阻止连接，只允许所需应用/入站 TCP `24800`，并将允许范围收窄到 Mac 的有线地址。无需关闭整个防火墙。

### 确认 Deskflow 确实走网线

在 Windows PowerShell 运行以下只读命令：

```powershell
Get-NetAdapter | Select-Object Name, Status, LinkSpeed
Find-NetRoute -RemoteIPAddress 192.168.250.2
Get-NetTCPConnection -State Established -LocalPort 24800 |
    Select-Object LocalAddress, LocalPort, RemoteAddress, RemotePort, OwningProcess
```

期望路由选中有线接口，已建立连接的本地地址是 `192.168.250.1`、远端是 `192.168.250.2`。还要确认两端浏览器仍能通过 Wi-Fi 上网。

Mac 终端可只读检查：

```bash
route -n get 192.168.250.1
lsof -nP -iTCP:24800 -sTCP:ESTABLISHED
```

期望接口是有线网卡，连接两端地址为上述有线地址。Ping 可以辅助排查，但不回 Ping 不能单独证明 Deskflow 断连；实际 TCP 连接地址更有判断价值。

## 4. 双向文字和图片剪贴板

在 Windows 的“配置服务器 → 高级”（Configure Server → Advanced）中：

- 开启 **Enable clipboard sharing**。
- 将剪贴板大小上限设置为 **16**（界面显示 MB，按此值约为 16 MiB）。客户端使用服务器的限制值，无需单独设置另一套上限。
- 应用设置；需要时停止并重新启动共享，然后**重新复制**内容，再跨屏粘贴。

先在两端纯文本编辑器中各复制、跨屏、粘贴一次，再用 Windows 截图工具截图并复制，切到 Mac 的 Codex 输入框粘贴，等待图片附件出现；反向也用支持图片粘贴的应用测试。

截图的传输数据可能是未压缩位图，不能按 PNG 文件大小判断是否超限。例如约 1752 × 792 的图片，24 位像素本身就约 4 MiB，可能超过原来 3 MiB 的限制。16 MiB 适合常见截图，但部分 4K 大图仍可能超限；先缩小截图，或按需求适度调整限制。大图需要传输时间，不能把“按下粘贴后没有立刻出现附件”直接当成失败。

本功能共享的是剪贴板中可支持的文字和图像格式，不等于任意文件或文件夹传输。某个应用能否接收图片，还取决于它自身的粘贴支持。

## 5. 打通麦克风并跟随鼠标

### 图形设置快速入口

新界面把服务器/客户端分别说明为“发送键鼠”和“接收键鼠”，并在顶部显示连接状态及下一步操作。“等待其他电脑连接”代表服务器已开始监听，还没有客户端连入。

1. Windows 主窗口选择“发送键鼠”，Mac 选择“接收键鼠”，先按前文接通有线连接。
2. 安装下一节介绍的两端虚拟声卡后，在主窗口点“麦克风设置”，或打开“设置 → 麦克风”。
3. Windows 勾选“启用麦克风跟随鼠标”，选择物理麦克风和虚拟输出 **CABLE Input**，填写目标 Mac 的 Deskflow 名称及从 Mac 核对的 SHA-256 证书指纹。
4. Mac 勾选相同开关，虚拟输出选择 **BlackHole 2ch**。在“网络”页的“安全”区域启用 TLS；Windows 还需启用客户端指纹校验。
5. 点“保存”。已有运行中的 Core 会按原有设置保存流程重启并重新连接。Windows 应用选择 **CABLE Output** 作为麦克风，Mac 应用选择 **BlackHole 2ch**，再按下面的验收步骤说话测试。

“刷新设备”只更新设备列表，不录音。已经选中的设备如果缺失，会保留名称并标记不可用；选择正确设备后再保存。设备不支持 48 kHz、指纹格式不对或缺少 TLS 时，会在页面上说明原因；取消则放弃尚未保存的麦克风编辑。此界面不修改系统默认音频设备，也不替用户安装驱动或信任证书。

### 5.1 音频方向

```text
Windows 物理麦克风 → Deskflow
  鼠标在 Windows：→ CABLE Input → CABLE Output → Windows 应用
  鼠标在 Mac：    → 加密的 Deskflow 连接 → BlackHole 2ch → Mac 应用
```

虚拟声卡让普通应用能将收到的声音选作“麦克风”。这里的 Input/Output 是驱动端点名称：**Deskflow 向 CABLE Input 播放声音，Windows 应用从 CABLE Output 录入声音**，不要选反。

### 5.2 Windows：VB-CABLE

1. 从 [VB-Audio 官方网站](https://vb-audio.com/Cable/)下载适合系统架构的 VB-CABLE，完整解压，以管理员身份运行安装程序，安装后重启。已有正常工作的驱动无需反复安装。
2. 在系统声音设备中确认存在播放端 **CABLE Input (VB-Audio Virtual Cable)** 和录音端 **CABLE Output (VB-Audio Virtual Cable)**。
3. 物理麦克风保留给 Deskflow 采集，允许桌面应用访问麦克风，采样率配置为支持的 **48 kHz**。本实现支持相应的单声道或双声道设备。
4. 系统默认输入及默认通信输入可设为 **CABLE Output**。某个应用如固定选择了其他麦克风，还需在应用内改成 CABLE Output，必要时重新打开应用。

**系统声音输出继续选择真实扬声器/耳机。** 不要将整个系统的默认播放输出改为 CABLE Input，否则日常系统声音可能进虚拟线缆，或听不到声音。

### 5.3 Mac：BlackHole 2ch

已装 Homebrew 时，在 Mac 终端执行：

```bash
brew install --cask blackhole-2ch
```

按安装提示完成并重启；也可使用 [BlackHole 官方项目](https://github.com/ExistentialAudio/BlackHole)提供的安装方式。[Homebrew 安装页](https://formulae.brew.sh/cask/blackhole-2ch)明确要求重启后生效。

在“音频 MIDI 设置”中确认 **BlackHole 2ch** 存在，格式选择 **48,000 Hz**。系统声音输入选择 BlackHole 2ch；需要使用麦克风的应用也选择该输入，或使用已指向它的系统默认输入。

Mac 的声音输出继续使用真实扬声器/耳机。本场景由 Deskflow 直接写入 BlackHole，通常不需要聚合设备或多输出设备，也不需要把所有系统声音送进 BlackHole。

### 5.4 找到真正生效的配置文件并备份

日常优先使用上述图形设置页。需要恢复备份、排查加载路径或手工配置时，仍可使用下面的 INI 模板。以启动日志中的 `initial settings file:` 路径为准：

| 安装方式 | 常见配置路径 |
| --- | --- |
| Windows 便携版 | `deskflow.exe` 同目录下的 `settings/Deskflow.conf` |
| Windows 用户配置 | `%APPDATA%/Deskflow/Deskflow.conf` |
| Mac 用户配置 | `~/Library/Deskflow/Deskflow.conf` |

系统级配置或 `XDG_CONFIG_HOME` 等环境覆盖可能改变路径，不要仅凭目录里存在文件就判断它正在生效。

编辑前停止共享并退出两端 Deskflow，确认相关 Core 已退出，然后备份实际配置和 TLS 配对资料。编辑已有节中的值，避免重复追加多个 `[microphone]` 或 `[security]` 节；保留其他设置。便携版升级也要保留原 `settings` 目录。

### 5.5 Windows 服务器配置模板

```ini
[security]
tlsEnabled=true
checkPeerFingerprints=true

[microphone]
enabled=true
input=Microphone Array (Realtek(R) Audio)
output=CABLE Input (VB-Audio Virtual Cable)
target=mac-mini
targetFingerprint=<MAC_SHA256_FINGERPRINT_64_HEX>
```

替换以下项目后再保存：

- `input`：Windows 物理麦克风的准确设备名称，中文系统可能类似“麦克风阵列 (Realtek(R) Audio)”。设备名称不匹配时，查看日志列出的可用设备名称。
- `output`：Windows 虚拟线缆**播放端**的准确名称。
- `target`：Mac 在 Deskflow 中配置的计算机名称；不是 IP 地址，也不保证等于系统显示的主机名称。本例仅使用占位名称 `mac-mini`。
- `targetFingerprint`：Mac 当前 Deskflow 证书的 SHA-256 指纹，填写 64 位十六进制值，去掉分隔冒号/空格。与 Mac 显示的指纹核对，不能用 Windows 自己的指纹代替。

`checkPeerFingerprints` 是本项目的真实配置键。不要写成 `checkPeers`。重新生成 Mac 证书后，要重新核对配对信息并更新这里固定的指纹。

### 5.6 Mac 客户端配置模板

```ini
[microphone]
enabled=true
output=BlackHole 2ch
```

Mac 的 `output` 是 Deskflow 要写入的虚拟音频设备。两端都保存后**重新启动 GUI 和 Core**，让配置重新加载；只断开再连接不一定足够。

### 5.7 验收实际声音

1. 两端连接成功后，Windows 用系统输入音量表或录音应用选择 **CABLE Output**；Mac 用系统输入音量表或录音应用选择 **BlackHole 2ch**。
2. 鼠标留在 Windows，持续说话：Windows 虚拟输入应有电平，Mac 不应继续收到新的语音。
3. 鼠标切到 Mac，持续说话：Mac 虚拟输入应有电平，Windows 虚拟输入停止收到新的语音。切换时应用已有缓冲可能短暂播放完。
4. 在真正使用的 Mac 应用中选择 BlackHole 2ch，实际录入/语音输入并检查结果。再回 Windows 用 CABLE Output 做同样测试；只看到设备列表不算声音已打通。
5. 检查连续跨屏、图片复制、说话同时进行时键鼠是否正常，再检查断线重连、两端重启后的配置是否保留。

各类支持选择系统音频输入的应用通常可以使用这条通路。固定了其他设备、独占设备或缓存了旧设备的应用，仍需单独调整并验收。

这项功能切换的是 **Deskflow 输出到虚拟设备的音频**，不会从硬件层强制关闭 Windows 物理麦克风。其他应用如果仍直接选择物理麦克风，可以独立录到声音。停用时在两端设 `enabled=false` 并重启，再按需要恢复应用/系统默认输入。

### 5.8 资源占用

当前传输为 48 kHz、单声道、16 位 PCM，每 20 ms 一帧：音频有效负载为 `48,000 × 1 × 2 = 96,000 字节/秒`，约 `0.768 Mbit/s`，加上协议和 TLS 开销为约 1 Mbit/s 的量级。实际值随连接状态与传输开销变化。

仅控制 Mac 并向其发送音频时产生这部分跨机音频流量；鼠标在 Windows 时走本地虚拟声卡。CPU 和内存开销尚无统一实测数据，不能承诺固定百分比。本功能不把音频保存为录音文件；应用自己的录音、识别和网络上传行为由相应应用决定。

## 6. 日常启动、更新与回退

- 日常先确认网线和有线地址正常，启动 Windows 服务器，再启动 Mac 客户端。开机自启可在各端正常运行、权限正确后配置，并实际重启验收一次。
- 更新前记录两端构建提交、麦克风编译选项和运行目录，备份配置及 TLS 配对资料。**备份只留在本机安全位置，不上传到公共仓库。**
- 界面改造前能正常使用的源码保存在 [`stable-working-2026-11-16`](https://github.com/WUPK-cmd/deskflow_mac_windows/tree/stable-working-2026-11-16) 标签（提交 `a8d477566`）；它包含前一版文档，运行时代码对应 `a1b12757d`。Windows 当时实际使用的程序和私人配置另有本机完整备份。源码标签不包含私人配置，恢复源码后仍需开启麦克风编译选项。
- 检查源码目录 `git status --short`，先保留本地改动，再从本仓库拉取。更新两端到选定提交，并保持麦克风编译选项开启；详细命令见 [SelfBuilt.md](SelfBuilt.md)。
- 保留一份能正常使用的旧程序目录。新版本出现问题时停掉新 GUI/Core，恢复旧程序和匹配的备份，再验证连接及音频；不要同时启动新旧版本。
- 持久保存源码、本文和构建说明即可重建环境。Actions 构建产物有保留期限，若需要长期保存可运行包，应另外整理对应提交、校验和及构建选项后发布 Release。

## 7. 常见问题

| 现象 | 优先检查 |
| --- | --- |
| 插了网线仍然卡顿 | 实际 TCP 地址是否仍是 Wi-Fi；Mac 是否连了有线服务器 IP；网线/USB 网卡链路是否稳定 |
| 无法连上 Windows | Windows 是否是服务器并已启动；有线地址/掩码是否匹配；TCP 24800 是否被防火墙阻止；屏幕名称和证书信任是否正确 |
| 文字正常，图片不出现 | 服务器共享开关/大小上限；重新截图复制；等传输完成；换支持图片的应用区分接收应用问题 |
| 鼠标正常，完全没有麦克风 | 两端是否都启用编译选项并部署 Qt Multimedia；虚拟驱动是否安装重启；实际加载的配置路径是否正确 |
| 日志说找不到音频设备 | 配置中准确设备名称、48 kHz 支持；设备是否被禁用/独占；Windows 桌面应用麦克风权限 |
| Windows 有声，Mac 无声 | Mac 的 BlackHole 输出配置、应用输入、权限；服务器的 target 名称和 Mac 指纹；TLS/客户端指纹校验；Mac 是否为麦克风版本 |
| 系统有电平，某个应用无声 | 应用是否固定了旧输入、是否有麦克风权限；改选虚拟输入并重开应用，实际录入测试 |
| 安装后系统声音听不到 | 默认播放输出是否误改成 CABLE Input 或 BlackHole；恢复真实扬声器/耳机 |
| 下载的 Windows 程序被应用控制阻止 | 查看是否为 `WinError 4551`；下载包未签名，本地策略可能要求管理员认可的签名。使用已验证的本地源码编译流程或由管理员处理签名要求 |
| 改了配置不生效 | 是否改错文件；GUI 退出前是否覆盖了手改值；是否存在旧版 Core；是否完整重启两端 |
| 证书更换后麦克风停止 | 重新核对并信任对端证书，同时更新固定的 Mac SHA-256 指纹 |

## 8. 验证范围与可保留资料

- **使用者确认**：截至上文日期，双向剪贴板（含截图）、麦克风共享可用；有线直连后鼠标体验明显改善。这是当前两台设备的使用反馈，不代表所有应用和硬件组合都已验证。
- **改造前只读检查（2026-11-16）**：Windows 有线适配器链路为 100 Mbps，到 Mac 的路由选择有线接口，Deskflow 已建立的 TCP 连接为 `192.168.250.1:24800 ↔ 192.168.250.2`。当时运行的程序和配置已完整备份；新界面在独立配置环境验收，当前双端音频仍以实际用户环境的声音测试为准。
- **已有自动化证据**：源码提交 `a1b12757dd4b1af8daee425ddb5ab426ca3d98ed` 的 [Windows/macOS CI 记录](https://github.com/WUPK-cmd/deskflow_mac_windows/actions/runs/43146259286)。常规 CI 未启用麦克风，不能据此声称虚拟声卡或双端音频通过验收。
- **后续构建**：新的 [Self-built 工作流](https://github.com/WUPK-cmd/deskflow_mac_windows/actions/workflows/self-built-ci.yml)开启麦克风，并测试设置界面、打包依赖及解包后的启动。实际虚拟声卡、跨屏音频及目标应用录入仍需要按第 5.7 节单独验收，不能由 CI 代替。
- **已通过的双端安装包验收**：提交 `745e5ad91a65c57cf1b6901a5e36effb9d5a5462` 的 [原生构建与测试记录](https://github.com/WUPK-cmd/deskflow_mac_windows/actions/runs/43830970905)中，Windows 42 个、Mac 43 个测试程序全部通过；Windows MSI／便携 ZIP 和 Mac DMG／应用 ZIP 均完成生成、校验和、内置版本身份及解包/只读挂载后的独立启动检查。Mac 应用还通过临时签名验证；这不代表 Apple 公证或用户实际音频环境已经验收。
- **本次 Windows 界面与打包验收**：41 个安全回归测试通过；麦克风界面 31 项 Qt 测试通过，包括真实点击、输入、无效保存、保存及取消；真实主窗口的 4 项流程检查通过，并检查了中文显示及不同窗口宽度。未启用麦克风编译选项的构建也通过 3 个相关测试程序。MSI 采用管理解包，便携 ZIP 正常解压，二者在清除开发依赖路径后均能启动 GUI/Core 并加载随包的音频后端 DLL 依赖。此过程没有替换正在使用的版本，也未在用户机器上实际安装 MSI 产品或注册新服务。Mac 构建及打包结果以下载包对应的 CI 记录为准。
- **实验诊断代码留档**：[codex/pointer-metrics-win](https://github.com/WUPK-cmd/deskflow_mac_windows/tree/codex/pointer-metrics-win)，提交 `d0ebc3af7b139bb449152f0e75e2bc78aba68432`，包含可选的鼠标计数/耗时汇总和独立说明。它基于较早源码，不属于当前主线或本次验收版本，也不是已证明有效的卡顿修复。日常继续使用主线版本；未来需要重新诊断时再按分支说明单独构建验证。
- **新环境仍需执行**：本手册中的连接地址检查、文字/图片双向粘贴、音频电平及目标应用录入、重启/重连检查。记录构建提交、编译选项、设备名称和结果；日志共享前移除私人信息。
- **适合放进 GitHub**：源码、构建和使用文档、通用配置模板、去掉私人信息的测试说明。不要提交实际配置、证书私钥、录音或第三方驱动安装包；驱动从本文的官方网站安装。

完整设置键参考：[Configuration.md](Configuration.md)。
