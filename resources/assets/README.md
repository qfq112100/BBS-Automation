# V3 原生精装资源包

全部图像均为真实网络下载的现成作品。本资源代理不生成任何图片/纹理/装饰/AI图，不修改主源码、RC或构建脚本。

## 交付与嵌入建议

|用途|原始资源|体积|规格/字体族|
|---|---|---:|---|
|Hero|[hero-velvet-dunes.jpg](./hero-velvet-dunes.jpg)|678,951 B|2880×1800；NASA HiRISE 2018火星真实暗蓝紫沙丘摄影|
|正文/中文路径|[NotoSansSC-Regular-UI.ttf](./NotoSansSC-Regular-UI.ttf)|2,482,940 B|Noto Sans SC / Regular / 400；7976码点|
|精简粗体固定UI|[NotoSansSC-Bold-UIOnly.ttf](./NotoSansSC-Bold-UIOnly.ttf)|286,056 B|Noto Sans SC / Bold / 700；1316码点，含当前源码522汉字|
|完整GB2312粗体备选|[NotoSansSC-Bold-UI.ttf](./NotoSansSC-Bold-UI.ttf)|2,483,508 B|Noto Sans SC / Bold / 700；7976码点|
|UI图标字体|[fa-solid-900.ttf](./fa-solid-900.ttf)|426,112 B|Font Awesome 6 Free Solid / Solid；官方6.7.2完整TTF|
|应用图标原PNG|[noto-shield-512-v2.038.png](./noto-shield-512-v2.038.png)|23,138 B|官方Google Noto Emoji现成512×512透明PNG，Apache2|
|未启用候选|[twemoji-shield-72.png](./twemoji-shield-72.png)|850 B|Twemoji15.1.0现成72×72透明PNG，CC-BY4|

父代理仅对现成PNG做ICO格式/尺寸转换；不要自画底板、盾牌、渐变或装饰。
静态TTF可由父代理机械LZMS压缩后作为资源嵌入，并在运行时纯内存还原；这与原始授权素材来源无关。

### 字体重要约束

- 正常/粗体Family均为 **Noto Sans SC**，GDI+ PrivateFontCollection实际加载已验证。
- 正文/中文动态路径必须用Regular GB2312版；所有6763个GB2312汉字和ASCII均验证无缺失。
- UIOnly粗体只用于固定标题、按钮、数字、固定提示；任意中文路径/外部文本不能用精简粗体。
- 若后续加入GB2312外罕见字或非中文脚本，请重新扩展字形集合；本包不宣称覆盖整个Unicode。
- 原始可变TTF [NotoSansSC-VF.ttf](./NotoSansSC-VF.ttf) 17,772,300B，仅供溯源/重新子集，**不要嵌入EXE**。
- 子集仅删除/实例化现成字体字形，不设计字形。OFL授权保留名只有Source；现Family不使用该保留名，版权/授权name表保留。
- [font_conversion_report.json](./font_conversion_report.json)、[bold_uionly_report.json](./bold_uionly_report.json)、[font_validation.json](./font_validation.json)给出处理与覆盖证据。

### Font Awesome常用码点

```text
shield f132   home f015   save f0c7   logs f15c
settings f013 folder f07c sync f2f1   check f00c
warning f071  close f00d  info f05a   arrow-left f060
history f1da
```

## Hero来源与许可证据状态

选用[NASA官方作品原页面](https://science.nasa.gov/photojournal/the-velvety-blue-dunes-of-melas-chasma/)的 **The Velvety Blue Dunes of Melas Chasma**，PIA22783 / ESP_055322_1680。
署名必须保留：**NASA/JPL-Caltech/Univ. of Arizona**。
2018-05-16实际轨道相机观测；NASA原本的color-infrared科学通道配色，不是AI图。本代理没有裁剪、变色或生成任何像素。

NASA源页面、搜索元数据与详细元数据均已完整下载至[evidence目录](./evidence/)。
**透明说明：HiRISE官方Image Usage Policy当前HTTP502维护；NASA品牌许可页HTTP429，JPL页HTTP403，故许可正文的实时抓取未完成，绝不能把错误页当许可全文。**
NASA/HiRISE公共领域任务图像的使用政策URL、作者和完整来源可见[来源清单](./asset_sources.json)及[NASA归属说明](../../LICENSES/visual/NASA-HiRISE-PIA22783-NOTICE.txt)。
源码/字体/PNG许可全文均已实际下载保留。Hero政策正文应在官网恢复后补抓；没有虚构作者/作品页。

## 授权与复现

- [asset_sources.json](./asset_sources.json)逐件记录下载URL、来源原页、作者、原始SHA256、授权、是否转换、尺寸与字体Family。
- [完整授权目录](../../LICENSES/visual/)包含FA完整license、Noto Sans SC OFL、Noto Emoji Apache2、可选Twemoji CC-BY4，以及[署名说明](../../LICENSES/visual/ATTRIBUTION.txt)。
- 机械处理脚本：[prepare_fonts.py](./tools/prepare_fonts.py)、[prepare_bold_uionly.py](./tools/prepare_bold_uionly.py)。
- fontTools4.61.1官方pure-Python wheel保存在[tools目录](./tools/)，脚本本地解包使用，**不pip、不安装到系统、不进入EXE**。
- bundled Python即可运行脚本；第二脚本可传工程路径。工具/原始VF/证据HTML均为开发溯源资料，不是EXE运行依赖。

未选用Unsplash候选：网站原页bot验证导致无法可靠核对具体作品页面，故未猜测作者/原页，也未带入游戏官图或未知版权素材。
