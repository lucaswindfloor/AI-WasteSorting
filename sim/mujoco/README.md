# MuJoCo 单 PET 瓶物理验证

本实验读取现有 `pet-pick` 报告里的 M06 任务，在 MuJoCo 中驱动三轴吸盘。仿真步长为 1 ms。沿用长 5500 mm、宽 800 mm 的皮带、1 m/s 的带速，以及 2400–2900 mm 的抓取区。皮带用一段连续平移的带面近似，吸附用接触时建立的可解除约束近似。机械轴能力、瓶子质量和吸附判定阈值都是首轮试验设定，现场需要用设备数据校准。

现有网页继续用于采集仿真和旧回放，这一阶段从报告文件接入，物理动作在 MuJoCo 窗口中查看。物理事件记录在自己的报告里，接入实时 M07 是后续一阶段。

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

接着生成一份新的本机采集报告，并跑三组无窗口验证。请在同一个 WSL 终端执行下列命令。每次新建运行目录，方便保存之前的采集数据。

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

窗口默认以半速播放物理计算，可以看到瓶子随皮带进入、吸盘跟随并下降、抬升、移向料箱、释放和回位。等正常结束后再关闭窗口，提前关闭会记录 `viewer_closed`。WSL2 依赖 WSLg 显示窗口。如果无窗口测试正常、图形窗口打不开，在 Windows PowerShell 中执行 `wsl --update` 和 `wsl --shutdown`，再打开 Ubuntu、激活虚拟环境并重试。 [微软 WSL 图形应用指南](https://learn.microsoft.com/zh-cn/windows/wsl/tutorials/gui-apps)

本机验收后，把命令终端上的三条结果、`physics/` 下的 JSON，以及正常抓取的录屏或截图交回。若图形遇到问题，同时保留无窗口结果和完整错误输出。
