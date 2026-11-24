# M01／M02／M03 传送带采集、配准与推理仿真

## 生产比例布局

运行 `build/m01_sim --layout production`，或在网页选择默认的「生产比例 · MV-CL083」。默认带速 1 m/s，可选 2、3 m/s。生产布局支持 `normal` 和 `rgb-frame-loss`，后者丢弃第 4 个 RGB 数据帧，用于查看受影响的连续行和推理窗口。

| 参数 | 仿真假设 |
| --- | --- |
| 传送带 | 长 5,500 mm，宽 800 mm，位置自入料端向下游计 |
| 工位 | RGB、HSI、深度共用距入料端 750 mm 的扫描截面，抓取区 2,400–2,900 mm |
| 物料 | PET 饮料瓶 210 × 66 mm，铝饮料罐 122 × 66 mm。俯视图为生成的透明素材，模拟 RGB 行也从该纹理取样 |
| RGB | 8,192 像素 RGB8，1 mm 定距触发，20 个名义编码器计数/行，128 行/帧 |
| 高光谱 | 640 点 × 224 模拟波段，独立触发，在 1、2、3 m/s 时采用 5、10、15 mm 行距 |
| 深度 | 同一截面独立采样，默认每 12 ms 一次。仍可选每 8 次 RGB 编码器触发采样 |
| M03 | 每 512 RGB 行组成窗口，步长 128 行。模拟规则网格为 128 行 × 200 列，约 4 mm 横纵采样，保留原始帧、行、列与带坐标引用 |

M01 的 `RgbTriggered` 模式只等待 RGB 的触发归属。HSI 和深度采样在同一带面截面进行，各自保留曝光时刻和锁存位移并独立交付。M03 执行 RGB 轮廓规则，HSI 暗白校正和深度投影可从 M02 回放。试验图中的抓取区表示工位和计算时效。实装时需确认三个设备的实际观测面、照明与触发时序。

网页「全线」和「入料与扫描段」采用同一毫米坐标，物料图跟随编码器位移。点击「跳到首个候选」可以定位 512 行窗口及其原始 RGB 源行。窗口预览横轴是沿带 512 mm，纵轴是跨带 800 mm，两轴等比例，沿带每 128 mm 标一段。仿真从 RGB 至抓取区起点的行程时间扣留 200 ms 作为示例下游余量，其余时间用作 M03 截止。工位位置、余量和物料尺寸是可替换的设计假设。

以下各节同时适用于原有的「演示」布局。演示继续提供共享触发、联合推理和时钟故障场景。

仿真按物料和皮带的位置生成模拟设备数据。C++ 程序通过现有的 `AcquisitionService` 运行 M01 的采集、配对、分段和诊断，再通过 `M01CaptureAdapter` 将实际输出送入 M09。每轮运行都按原始设备位置、触发号和源帧行核对结果，并从归档读回验证。

M01 的每个采集段同时交给实际的 `CalibrationService`，按对应运行和世代装入模拟时钟、列映射、编码器、E05 外参和暗白参考。M03 从完整的 M02 结果组窗，调用模拟规则，再把候选回映至原始行列和皮带坐标。网页回放同一程序写出的设备、配准、推理和归档时间线。慢速播放、暂停、单步和跳转只改变查看位置。

## 1. 构建

使用 Linux 或 WSL2，准备 C++20 编译器、GNU Make 和 OpenSSL 开发库。Ubuntu 或 Debian 可以执行：

```sh
sudo apt install build-essential libssl-dev python3
git clone https://github.com/lucaswindfloor/AI-WasteSorting.git
cd AI-WasteSorting
make -j4 m01-sim
```

Python 3 用于场景回归。浏览器方式另外需要 Node.js 22 或更新版本，网页没有 npm 依赖。

## 2. 命令行运行

```sh
make m01-test
build/m01_sim --scenario normal
build/m01_sim --scenario mixed --speed-mm-s 1000 --depth independent
make m01-sim-test
```

- `make m01-test` 运行原有 M01 单元测试。
- `build/m01_sim` 实际运行 M01、M02、M03 和 M09。终端打印 `PASS` 或 `FAIL`，并给出两份报告的位置。退出码 0 表示校验通过，1 表示校验失败，2 表示配置或执行错误。
- `make m01-sim-test` 自动运行 20 组演示用例和 2 组生产布局用例，覆盖采集账本、配准、候选回映、窗口失效、模型换版和归档读回。生产用例含 1 m/s 全宽正常运行及 3 m/s RGB 丢帧。

每轮默认在 `build/m01-sim-runs/` 下创建新目录。可以用 `--output` 选择一个尚未存在的目录。

| 输出 | 内容 |
| --- | --- |
| `ledger.csv` | 每次触发的编码器计数、来源会话、RGB／HSI 帧行、到达时间、质量、M01 分段号和 M09 提交版本 |
| `report.json` | 设备输入、M02 配准、M03 推理窗口及候选源引用、模型版本事件、队列、归档和最终校验 |
| `archive/` | M09 实际写入的原始段、来源引用、缺口和各版本清单 |

## 3. 浏览器观察

```sh
node web/m01_sim/server.mjs
```

在本机浏览器打开 `http://127.0.0.1:8081`。如果要回放以下共享触发示例，先切换到「原有演示与故障场景」，再选「高光谱漏行与晚到」，点「运行仿真」。这一轮生成报告后会开始回放，可以暂停、拖动时间轴或点「下一事件」。

1. 看 PET 瓶和铝罐经过共同扫描线，核对编码器计数和已到达封包。下方是 M01 实收数据生成的三路预览，高光谱按所标注的模拟波段以伪彩显示。
2. 点「首条缺口」，查看触发 **#87** 的 RGB 源行、HSI 缺口和归档版本。点「触发瞬间」可回到曝光位置，再点「定位结果」复查后续配对与归档。定位 **#88** 时，两路应恢复正常配对。
3. 定位 **#153**，可同时看到原 HSI 缺口和晚到证据。**#154** 仍应正确配对。
4. 在「校验结果」查看计数和归档结果，下载 CSV 和 JSON。混合场景在默认设置下应有 **240 次触发、238 对配对、2 个 HSI 缺口、1 条晚到证据、30 条深度观测**。

预览默认使用「皮带坐标 · 等比例」。横轴是皮带坐标 `扫描线位置 − 曝光位移`，纵轴是横跨皮带的位置，三路同用一套毫米刻度。顶视图与虚线轮廓读取后端用于传感器采样的同一份几何；虚线仅用于对照，不是观测数据。深度只画已取得的观测条带，不插值。切换到「采集顺序 · 原始预览」可查看从早到晚的拼图，瓶嘴先过扫描线时仍出现在左侧。

触发账本中的「查看选中触发的 M02 配准」会跳到毫米坐标图。切换「补偿前」和「补偿后」可以查看两路观测的位置、沿带补偿和共同覆盖。时间表分别列出换算到主机时基的曝光时间和收件时间。HSI 样本显示原始值、校正值和使用的参考版本。E05 显示投影坐标、有效零高和缺测数。选用编码器采样时，使用每个 E05 帧明确对应的参考触发。

选「正常采集」和「2.0 m/s」，运行后在「M03 推理窗口」点「跳到首个候选」。候选会显示在当前窗口和传送带上。点击候选下的 RGB 或 HSI 源行，可以定位该触发，核对帧、行、列和 M02 配准。把「回放窗口」切回「跟随回放」，可随时间观察组窗、候选、队列状态和模型版本变化。

本地服务的默认端口是 8081，可以设置 `M01_SIM_PORT`。如使用自定义构建目录，可设置 `M01_SIM_BINARY` 指定程序，`M01_SIM_RUNS` 指定网页运行的归档目录。

## 4. 场景与模拟设备参数

| 场景 | 验收结果 |
| --- | --- |
| `normal` | RGB 和 HSI 到达顺序交替，240 次触发完整配对。 |
| `missing` | HSI #87 缺行，#88 正常。 |
| `late` | HSI #153 超时，原缺口与晚到证据分开保存。 |
| `mixed` | 两类故障同时出现，实际缺口仍落在 #87、#153。 |
| `slow-archive` | 模拟 M09 消费变慢，队列积压可见，排空后采集缓冲归零。 |
| `reconnect` | 采集世代、计数世代和设备会话切换。M01 拒绝一个旧世代回包，M02 采用新世代的标定快照。 |
| `transport-delay` | HSI 只晚回包 1 ms，M02 共同覆盖保持 240 次。 |
| `exposure-delay` | HSI 晚曝光 1 ms，两台相机位于同一扫描线。带速 2 m/s 时沿带补偿为 −2 mm，补偿后仍没有共同覆盖。 |
| `compensated` | HSI 晚曝光 1 ms，扫描线沿带速对应的距离移到下游。带速 2 m/s 时下游偏移 2 mm，补偿后对应位置重合，240 次有覆盖。 |
| `exposure-limit` | 沿用下游扫描和补偿位置，最大可接受曝光差设为 0.5 ms。M02 报告曝光时差超限。 |
| `clock-missing` | HSI 时钟映射无法核实，M01 仍有 240 对，M02 共同覆盖为 0。 |
| `count-interpolation` | #100 使用已到达的前后同步计数插值，#240 缺后一锁存，保留待核。 |
| `invalid-white` | 选定波段的白参考和暗参考相等。几何覆盖与该波段校正质量分别展示。 |
| `inference-overload` | 后端延迟设为 125 ms，单执行位、两个等待位。窗口报告超期或队列满，任务结束后缓冲归零。 |
| `model-switch` | 在第一世代结束前预热并拒绝不兼容候选，下一世代启用 v2。旧后台任务按获准策略排空，并保留 v1 身份。 |

以下命令使用相同带速比较回包与曝光两种变化。

```sh
build/m01_sim --scenario transport-delay --speed-mm-s 2000
build/m01_sim --scenario exposure-delay --speed-mm-s 2000
build/m01_sim --scenario compensated --speed-mm-s 2000
```

皮带每移动 1.5 mm 触发一次，相当于编码器每 30 个计数触发一次。RGB／HSI 按行采集，再按每包行数打包，并人为加入不同的传输延迟。E05 默认每 12 ms 独立采样。选择 `--depth encoder` 时，每 8 次编码器触发采样一次，E05 仍作为独立观测交付。深度预览中的有效 0 mm 和缺测点分别保留。

默认规格是 96 像素 RGB、80 空间像素 × 16 模拟波段 HSI，以及 64 点深度行。对照海康 MV-CL083-92GC V2 或 Specim FX17e 的实际硬件版本时，可根据规格书和 SDK 回读调整行宽、波段数和打包行数。以下命令是一组较大载荷的仿真输入：

```sh
build/m01_sim --scenario normal --speed-mm-s 2000 \
  --rgb-width 8192 --hsi-width 640 --hsi-bands 224 --packet-lines 8
```

`build/m01_sim --help` 可查看全部参数。规格和延迟用于测试 M01 接入契约。厂商 SDK、相机触发布线、真实曝光误差、光谱校准和持续带宽需要设备台架验证。

## 5. M03 运行参数与结果

默认是 `--m03 joint --m03-mask strict`。可改为 `rgb`、`hsi` 或 `split`，分别查看 RGB 轮廓、HSI 材质候选，或同时运行两条分路。选择 `off` 可以单独回放采集与配准。允许缺测时使用 `--m03-mask allow`，模拟清单每窗口最多容忍两条无效行，各输入张量有效样本比例至少为 85%。每个窗口 32 行，步长 16 行。仿真用最早相关触发的时间加 120 ms 作为处理截止。

| 2 m/s 场景，默认传感器规格 | 窗口结果 |
| --- | --- |
| 正常采集 | 14 个完整联合窗口，其中 11 个产生轮廓与材质候选，3 个无候选 |
| 高光谱只晚回包 1 ms | 14 个联合窗口按时完成 |
| 同线晚曝光 1 ms | 14 个联合窗口输入无效，选分路时两条路径仍各自完成 14 个 |
| 下游 2 mm 且晚曝光 1 ms | 恢复 14 个联合窗口 |
| 混合漏行和晚到 | 完整输入规则拒绝 4 个窗口，允许缺测时 14 个窗口按时完成 |

模拟后端从 RGB 前景颜色和高光谱端点的亮度及谱形生成候选。PET 与 Al 是本仿真场景的材质标签。时间和队列用于软件链路验收，实际物料准确率与运行性能由真实模型和目标工控机上的实测数据评估。

## 6. BCAF 512 离线分割

生产布局可把 M02 验证过的 RGB 扫描行和暗白校正后的独立 HSI 导出。BCAF 复用实际 M03 的组窗与缺行判定，在 C++ 仿真结束后逐窗运行。每窗覆盖跨带 800 mm、沿带 512 mm。RGB 有效区域为 328 行 × 512 列，HSI 为 164 行 × 256 列。两路按编码器位置对应同一块皮带，等比例调整并在上下补边，输入分别为 RGB `[1,3,512,512]` 和 HSI `[1,225,256,256]`。第 225 个光谱通道补零。HSI 在已校正的扫描行之间按位置插值，采集首尾的边缘延拓限制在一个名义 HSI 行距内，并计入报告。中间缺行超出一个行距时跳过相关窗口。

BCAF 使用公开的 RGB-512／HSI-5 网络和 SpectralWaste 的 7 类输出。PET 瓶和铝罐的仿真光谱是合成值。实测识别需核对训练与实机的波段中心、波段顺序、辐射单位和类别定义。离线报告记录模型前向的实际耗时与扣除补边的分割图。浏览器原有的 M03 候选、队列和时限仍显示 RGB 规则链路的模拟数据。

### 安装与命令行验证

在本仓库根目录执行以下命令。CPU 测试使用 PyTorch 2.7.1。使用 NVIDIA GPU 时，可在虚拟环境安装适配本机显卡驱动的 PyTorch 和 torchvision，运行时加 `--device cuda:0`。

```sh
git clone https://github.com/jonasvilhofunk/BCAF_2026.git ../BCAF_2026
git -C ../BCAF_2026 checkout cacb77e3ba5d95ad4df1cac5d7117ff48034eb35
python3.12 -m venv ../.venv-bcaf
source ../.venv-bcaf/bin/activate
python -m pip install --index-url https://download.pytorch.org/whl/cpu torch==2.7.1 torchvision==0.22.1
python -m pip install numpy Pillow PyYAML timm
make -j4 m01-sim
build/m01_sim --layout production --export-bcaf --output build/bcaf-capture
python sim/m01/bcaf_infer.py --run build/bcaf-capture --bcaf-repo ../BCAF_2026 \
  --smoke-random --limit 1 --output build/bcaf-smoke
```

`--smoke-random` 使用未训练权重检查两路张量、真实网络前向和分割图输出。测试官方模型时，从作者的 [Hugging Face 权重库](https://huggingface.co/jonasvilhofunk/BCAF_2026/tree/main) 下载完整的 `BCAF_SpectralWaste_rgb512_hsi5_best.pth`，例如放到 `../BCAF_2026/models/paper/`。可以复用上一步的采集目录。以下命令用合成反射率检查训练权重加载和分割链路。

加载器会把发布权重中省略 `backbone` 层级的 RGB／HSI 键映射到模型已有的键，再严格核对全部键和张量形状。原始 `.pth` 可直接使用。

```sh
checkpoint=../BCAF_2026/models/paper/BCAF_SpectralWaste_rgb512_hsi5_best.pth
python sim/m01/bcaf_infer.py --run build/bcaf-capture --bcaf-repo ../BCAF_2026 \
  --checkpoint "$checkpoint" --sim-reflectance --output build/bcaf-predictions
```

验证作者原始数据时，从 [SpectralWaste](https://github.com/ferpb/spectralwaste-segmentation) 下载原始 RGB／HSI，按 BCAF 的 `scripts/Preprocess.ipynb` 生成配对 `.npz` 和训练集的 `hsi_stats.yaml`。然后选一个 test 配对运行。此入口按 BCAF 原始评估预处理直接预测该图。

```sh
python sim/m01/bcaf_infer.py --npz /path/to/test-sample.npz --bcaf-repo ../BCAF_2026 \
  --hsi-stats /path/to/hsi_stats.yaml --checkpoint "$checkpoint" --output build/bcaf-test-image
```

各输出目录会保存 `bcaf-report.json`、类别索引图 `*-mask.png` 和叠加图 `*-overlay.png`。报告按触发范围列出窗口、丢帧时的跳过原因、像素到皮带毫米坐标的边界、有效图像范围、权重摘要和前向耗时。重复验证时请换一个输出目录。

### 网页分割回放

激活同一个 Python 环境，在本仓库根目录启动服务。

```sh
M01_SIM_BCAF_REPO=../BCAF_2026 M01_SIM_BCAF_CHECKPOINT="$checkpoint" \
  M01_SIM_BCAF_PYTHON="$(command -v python)" node web/m01_sim/server.mjs
```

在生产布局中选择「BCAF 512 · 离线分割」，然后运行仿真。点「跳到首个候选」或切换「回放窗口」，在 M03 面板查看该窗叠加图与前向耗时。默认设备是 CPU，可以通过 `M01_SIM_BCAF_DEVICE=cuda:0` 切换。网页默认直接使用仿真反射率，可用 `M01_SIM_BCAF_HSI_STATS` 指向与输入单位一致的训练统计文件。

## 代码位置

- `sim/m01/scene.*` 从同一场景计算 RGB 行、合成光谱和深度。
- `sim/m01/calibration.*` 为每个设备世代生成标定快照，并定义时钟和曝光故障场景。
- `sim/m01/inference.*` 构建模型清单、校验模拟制品，并从张量生成物料候选。
- `sim/m01/bcaf_export.*` 导出经 M02 校验的 RGB 行、HSI 位置和校正光谱。
- `sim/m01/bcaf_infer.py` 配准、组图、运行 BCAF 并保存分割结果。
- `sim/m01/simulator.*` 推进时间，衔接 M01、M02、M03 和 M09，核验并写报告。
- `sim/m01/main.cpp` 提供命令行参数和运行结果。
- `web/m01_sim/` 使用本机服务调用同一程序，播放数据并定位触发。
- `tests/m01_sim/scenarios.py` 通过命令行检查坐标、配准、窗口、模型状态、归档和账本。
