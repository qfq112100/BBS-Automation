# BBS 3.3.3：LTK 仅作为 Rose 内部依赖

## 按官方说明放置

Rose 1.4.9官方[使用说明](<https://github.com/Alban1911/Rose/blob/1.4.9/README.md#ltk-patcher-requirement>)要求把两个文件放到其tools目录；[检测代码](<https://github.com/Alban1911/Rose/blob/1.4.9/injection/tools/tools_manager.py#L54-L63>)只在host与DLL同处于self.tools_dir时返回host路径。

本机已核验两个文件确实位于[Rose内部工具目录](<C:/Users/Administrator/AppData/Local/LOLBackupV3/components/rose/1.4.9/_internal/injection/tools/>)，不是只留在独立缓存目录：

- ltk_patcher_host.exe，1239304字节；SHA256 27DE1BFDBF37129E42C12B18CC7B4F75C15D39BCD7AFEFE65C4C33D0E99FE856。
- ltk_patcher_dll.dll，834816字节；SHA256 0B024A3A839F55FF7CF7EE93D9ED2078BCBB07D3B7F320463D678F8A6209E5FF。

两者均为本机用户原始导入的官方文件，Authenticode Valid；本次不重新分发签名文件，也未修改它们。

## 界面改动

- 只创建、布局、绘制Rose与League Akari两个工具。
- 不创建第三张LTK卡片及620–629号控件；相应命令范围也只覆盖两个工具。
- 内容高度由1216缩短到936，移除第三卡片留下的空白滚动区域。
- LTK后台提取缓存与Rose依赖绑定逻辑仍存在，但只是Rose准备流程的内部实现，没有独立工具入口，不需要用户点另一张卡片或手动搬文件。
- 配置、原生同步、第三方资源清单/CAB及已安装Rose文件不因移除卡片而变动。

## 验证

验收数据见[双工具界面检查](<verification-two-tools/>)。真实屏幕回归在三档预览缩放及浅色主题验证滚动、页眉/底栏裁剪、托盘菜单，并明确断言第三工具全部控件不存在；另用新EXE资源检查实际Rose目录绑定。

本次界面与放置修正不等于证明游戏DLL已经附加或皮肤已生效；真实游戏验证仍需训练模式的新日志及实际效果。
