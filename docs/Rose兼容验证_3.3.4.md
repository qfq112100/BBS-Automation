# BBS 3.3.4：Rose / Akari 一键打开与兼容候选

> 后续20:06两次真实训练已经证实1.29.1候选仍在SetWindowsHookEx阶段失败，不能视为换肤修复。最新全面核查及单一对照依据见[3.3.5检查记录](<全面检查与签名对照_3.3.5.md>)。以下准备/启动通过记录不代表游戏成功。

## 已确认的实际失败原因

18:20和18:21的新日志均先出现hook installed、dll attached与init done，随后出现 `host verification failed, refusing overlay: [VRF00000008] (18892,ltk-manager.exe)`。18892是当时的Rose进程；不能把DLL附加当作覆盖已生效。

[LTK上游明确回应](https://github.com/LeagueToolkit/ltk-manager/issues/923#issuecomment-6350238267)：该检查就是为了阻止第三方应用使用DLL。没有通过改进程名、修改校验、关闭杀毒或驱动防护绕过它。

## 兼容候选，不是已成功的承诺

- 完整官方Rose 1.4.9与League Akari 1.5.1保持完整运行文件。
- 依赖候选来自[官方v1.29.1 tag](https://github.com/LeagueToolkit/ltk-manager/tree/v1.29.1/src-tauri/resources)，两个文件保持原始字节；不下载或启动独立Manager。
- [官方合并PR905](https://github.com/LeagueToolkit/ltk-manager/pull/905)记录该构建是protocol2、验证过26.21数据；protocol3宿主认证构建未进入它。它不是Rose维护者对本机的兼容认证。
- host为官方签名有效文件，1238272字节，SHA256 `23FA1AAEDA1A0C743DA44227F179084E1FB81C7CD262144A3C8604FFC49D3BC1`。DLL为上游tag本来未签名的文件，754688字节，SHA256 `26AA7F8D5790C5BB39B45CFEEF8251149D47D5DFB7FE43648AA774D5E6BB42FA`；不是我们剥离签名的结果。
- 本机构建仅供用户个人本机使用。签名host不得直接作为公开再分发包发布；对外发布须另行满足上游许可。
- DLL静态EOL为1793516400（2026-11-01 07:00 UTC）。当前游戏PE构建时间1792188909（2026-10-16 22:15:09 UTC）在范围内；该预检不保证实际换肤。

## 用户操作只保留两个入口

工具中心只有“打开Rose”和“打开Akari”及其目录入口；点打开自动准备，不再先点准备/检查。没有独立LTK卡片、Manager安装器、跟随、强停等附加工作流。

Rose使用新版本目录，避免覆盖旧Rose及其用户生成文件。旧Akari仅在其完整清单与原始71文件收据精确匹配时复用，不重写其旧标记或用户配置。两个补丁放在Rose的 `_internal/injection/tools/` 同一目录。严格依照清单核对host/DLL，轮询不重复散列。

## 已执行验证

- 13个原生回归程序全部通过；工具管理280项、几何/滚轮20938项。最终四种界面模式各376项通过。
- 完整新Rose准备成功，两个依赖按固定SHA-256校验并位于实际tools目录；既有Akari71文件精确复用，准备阶段旧配置与旧标记哈希不变。
- 新Rose通过真实打开流程达到System ready / APP STATUS READY；当时客户端LCU未就绪，因此没有据此声称游戏注入已经初始化。
- 实测发现Harness父进程有ELECTRON_RUN_AS_NODE=1，会把Electron应用误作Node运行。现仅过滤Akari子进程环境，不改父进程/系统环境；父值测试前后仍为1。真实Akari主窗口和Mini窗口均可见，单实例打开流程通过。
- 最终桌面版本3.3.4.0，145329152字节，SHA256 `72E913047A59EBA3779F7E47AC8F699127A35477160B7B0966B9F160B2918FB7`。

## 只读会话验收补充

新增维护者诊断脚本（不是BBS运行依赖，不需要用户安装Python）。16个隔离夹具测试全部通过，覆盖旧会话过滤、附加但未覆盖、明确拒绝优先、后续对局替代先前状态、零资源重定向、截断日志与只读完整性。

在当前真实目录执行结果是 `no_session`，两个候选文件SHA-256匹配。旧原始日志目前也不存在，不能声称进行了原始归档复验；此前工具返回的三个已观察行另存为明确标注“转录片段、非原始日志”的测试输入，识别结果为 `overlay_rejected` 且DLL已附加。此片段只验证分类行为，不是新的游戏证据。报告见[只读验收结果](<verification-rose-compat/read-only-diagnostics.json>)。此脚本始终保留人工视觉确认=false，不能自动代替实际皮肤确认，也不能仅凭当前文件哈希证明旧日志使用了哪一组文件。

桌面二进制未因该补充而改动，仍为SHA256 `72E913047A59EBA3779F7E47AC8F699127A35477160B7B0966B9F160B2918FB7`。

## 验证边界

验收记录见[本机证据目录](<verification-rose-compat/>)。构建、完整性、进程启动、后端初始化通过均不等同于游戏内生效。必须看到新会话的覆盖验证/资源重定向日志，并结合实际皮肤效果，才能判定换肤已修复。

未操作示例D盘；不改动同步业务、不主动写入真实Game/缓存、不关闭系统安全防护。
