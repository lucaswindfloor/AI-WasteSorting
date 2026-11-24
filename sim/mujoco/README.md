# MuJoCo 单 PET 瓶物理验证

本实验读取现有 `pet-pick` 报告里的 M06 任务，在 MuJoCo 中驱动三轴吸盘。仿真步长为 1 ms。沿用长 5500 mm、宽 800 mm 的皮带、1 m/s 的带速，以及 2400–2900 mm 的抓取区。皮带用一段连续平移的带面近似，吸附用接触时建立的可解除约束近似。机械轴能力、瓶子质量和吸附判定阈值都是首轮试验设定，现场需要用设备数据校准。

项目提供两种运行方式。联调模式由 M07 在仿真时间轴上实际发令，网页直接读取该轮的物理轨迹和回报。离线模式读取已有报告，适合用 MuJoCo 原生窗口单独看碰撞和动作。

## Trae 在 WSL2 的操作

先在当前 WSL2 的仓库根目录拉取并建立独立 Python 环境。MuJoCo 固定为 4.0.1，需要 Python 3.11 或更新版本。Ubuntu 24.04 默认的 Python 3.12 可直接使用。已有较新的 Python 时，用它替换下面创建虚拟环境的 `python3`。 [MuJoCo 官方安装说明](https://mujoco.readthedocs.io/en/stable/python.html) · [PyPI](https://pypi.org/project/mujoco/4.0.1/)

```bash
git pull --ff-only origin main
python3 --version
sudo apt update
sudo apt install -y python3-venv
python3 -m venv .venv-mujoco
source .venv-mujoco/bin/activate
python -m pip install -r sim/mujoco/requirements.txt
python -c "import mujoco; print(mujoco.__version__)"
```

如果系统只提供 Python 3.10，可以按官方说明安装 uv，在同一仓库执行 `uv python install 3.12` 和 `uv venv --python 3.12 --seed .venv-mujoco`，然后执行上面的激活环境、安装依赖、核对版本三步。 [uv 官方安装说明](https://docs.astral.sh/uv/getting-started/installation/)

## M07 与网页联调

在同一个 WSL 终端执行：

```bash
export M01_SIM_MUJOCO_PYTHON="$(command -v python)"
make -j4 m01-sim
make m01-mujoco-test
```

测试会从新的采集数据跑到 M07 发令、MuJoCo 物理执行和 M09 命令日志。正常组会校验一次发送、接触在窗内、实际吸附、释放、箱内稳定和资源归还。降速和横向偏移组会校验未抓到、撤回以及资源归还。回执不确定组会校验只发送一次、进入待核并保留资源。

停止旧版网页服务后，在同一终端启动新版。浏览器通过网页回放物理轨迹，使用无窗口 MuJoCo。

```bash
node web/m01_sim/server.mjs
```

打开 `http://localhost:8081/`，选择「单 PET 瓶 · MuJoCo 抓取」并运行。在「抓取工位近景」试「跳到接触」、慢放和拖动时间轴。跳到接触时，网页时刻应与下载报告里 `pick-action` 的 `contact` 一致。拖到末尾，应看到瓶子在箱内稳定、M07 资源已归还。刷新页面应恢复上次报告和播放位置。三个故障场景也可在同一下拉菜单选择。

需要直接生成一份可保留的联调报告时，可以使用命令行：

```bash
mujoco_run_dir=$(mktemp -d "$PWD/build/mujoco-live.XXXXXX")
build/m01_sim --layout production --scenario pet-pick --mujoco normal --output "$mujoco_run_dir/live"
printf '%s\n' "$mujoco_run_dir/live/report.json"
```

联调报告的 `pick-task` 是 M06 计划。`pick-command` 是 M07 发送记录，`pick-action` 是 M07 接收后的状态，`pick-result` 是物理执行结果，`pick-physics` 是吸盘、瓶子和皮带坐标采样。网页同时显示 M06 接触窗和收件后按控制器预算收紧的执行有效窗。采集到 3200 ms 结束，执行回报可继续推进。首轮的视觉采集和 E05 深度仍来自原单瓶场景，只有执行和抓取物理切到 MuJoCo。

Trae 验收后回传 `make m01-mujoco-test` 的结果、网页下载的正常组报告和抓取近景截图。异常时附上场景名、网页提示和服务终端错误。

## 用旧报告独立复跑

离线模式适合从故障中单独检查物理行为。每次用新的运行目录保存之前的采集数据。

```bash
make -j4 m01-sim
mujoco_run_dir=$(mktemp -d "$PWD/build/mujoco-local.XXXXXX")
build/m01_sim --layout production --scenario pet-pick --output "$mujoco_run_dir/capture"
mujoco_input="$mujoco_run_dir/capture/report.json"

for case_name in normal slow offset; do
  python -m sim.mujoco.pet_pick \
    --input "$mujoco_input" \
    --output "$mujoco_run_dir/physics/$case_name.json" \
    --case "$case_name" || break
done
printf '%s\n' "$mujoco_run_dir"
```

也可以把 `mujoco_input` 指向已校验通过的旧版 `pet-pick` 抓取报告，跳过重新采集。输入和输出必须是不同文件。

| 用例 | 预期结果 | 验收依据 |
| --- | --- | --- |
| `normal` | `PASS normal: picked` | 接触和吸附发生在任务时间窗内，瓶子解除吸附后在测试箱底稳定下来，吸盘回到待机位。 |
| `slow` | `PASS slow: missed_contact_window` | 只把 Z 轴速度上限降为 0.08 m/s。窗口截止时吸盘还没到接触高度。 |
| `offset` | `PASS offset: no_contact` | 只把抓点沿 Y 偏移 100 mm。吸盘降到了指定高度，仍未碰到瓶子。 |

`PASS` 表示符合该用例的预期。用例不符会返回退出码 1，输入或文件出错会返回退出码 2。报告的 `events` 包含实测仿真时刻，`samples` 每 10 ms 记录编码器、吸盘和瓶子位置。正常结果的判断还会检查碰撞体是否落在箱内、与箱底接触并保持低速。

## 查看机械爪动作

无窗口测试通过后，仍在同一个 WSL 终端运行：

```bash
python -m sim.mujoco.pet_pick \
  --input "$mujoco_input" \
  --output "$mujoco_run_dir/physics/view.json" \
  --view
```

窗口默认以半速播放物理计算，可以看到瓶子随皮带进入、吸盘跟随并下降、抬升、移向料箱、释放和回位。等正常结束后再关闭窗口，提前关闭会记录 `viewer_closed`。如果 WSLg 原生窗口启动崩溃，可以只给该命令加上 `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe`。浏览器联调使用无窗口计算，无需给网页服务设置这两个变量。 [微软 WSL 图形应用指南](https://learn.microsoft.com/zh-cn/windows/wsl/tutorials/gui-apps)

本机验收后，把命令终端上的三条结果、`physics/` 下的 JSON，以及正常抓取的录屏或截图交回。若图形遇到问题，同时保留无窗口结果和完整错误输出。
