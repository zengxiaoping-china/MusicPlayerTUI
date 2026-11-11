# MusicPlayerTUI

一个用 C 语言编写的 WSL/Linux 本地音乐播放器 TUI，界面为中文。

界面直接使用 ANSI 转义序列和 `termios` 实现，不依赖 ncurses。播放层提供两种后端：
内置的 FFmpeg（libav* 解码 + PulseAudio 输出）和 mpv（Unix Socket + JSON IPC）。
程序可以直接启动，在界面中随时添加音乐文件或文件夹，并会自动保存播放列表和常用设置。

## 功能

- 无参数直接启动，自动恢复上次的播放列表和设置
- 在程序内按 `a` 添加音乐文件或文件夹
- 递归扫描目录中的本地音乐文件，支持 `~/Music` 形式的路径
- 上下键浏览播放列表，回车播放
- 暂停/继续、上一首/下一首、快进/快退
- 音量调节、静音
- 单曲循环、列表循环、随机播放
- 实时显示播放进度、时长和播放状态
- 中文界面、UTF-8 文件名和终端窗口缩放
- 退出时自动关闭播放后端，并清理临时资源

支持的扩展名包括：`mp3`、`flac`、`wav`、`ogg`、`opus`、`m4a`、`aac`、`wma`、
`ape`、`aiff`、`mka`、`webm` 等。

## 依赖

- Linux/WSL
- C 编译器（gcc 或 clang）
- GNU Make
- [mpv](https://mpv.io/)；或以下 FFmpeg/PulseAudio 开发库：
  - `libavformat`
  - `libavcodec`
  - `libavutil`
  - `libswresample`
  - `libpulse-simple`

mpv 后端只需要安装 mpv。FFmpeg 后端需要开发头文件和库来构建；运行时使用系统
FFmpeg/PulseAudio 运行库。程序默认使用 mpv 后端；启用 FFmpeg 后可用
`--backend=ffmpeg` 或 `--backend=auto` 切换。

Ubuntu / WSL 上安装 mpv：

```sh
sudo apt update
sudo apt install -y build-essential mpv
```

如需启用内置 FFmpeg 后端，再安装开发包：

```sh
sudo apt install -y libavformat-dev libavcodec-dev libavutil-dev \
                    libswresample-dev libpulse-dev
```

WSL 的音频通常通过 WSLg（PulseAudio）输出。可以先在 WSL 终端里验证：

```sh
mpv ~/Music/some-file.flac
```

## 构建

```sh
make
```

生成的程序为 `./musicplayertui`。构建时会用 `pkg-config` 自动检测 FFmpeg 和
PulseAudio 开发库：检测成功则启用 FFmpeg 后端；否则仍可构建，但只有 mpv 后端可用。
是否启用 FFmpeg 不影响默认后端，程序仍默认使用 mpv。

可用下面的命令确认本次构建是否包含 FFmpeg 后端：

```sh
./musicplayertui --backend=ffmpeg
```

如果启动后界面显示“不可用”，说明当前构建没有 FFmpeg 支持。

```sh
make clean
make install              # 安装到 /usr/local/bin
make install PREFIX=$HOME/.local
```

## 使用

直接启动，恢复上次保存的播放列表：

```sh
./musicplayertui
```

启动时也可以直接加载音乐：

```sh
./musicplayertui ~/Music
./musicplayertui /mnt/d/Music
./musicplayertui song1.flac song2.mp3
./musicplayertui /path/to/music1 /path/to/music2
```

目录会被递归扫描，隐藏目录（以 `.` 开头）会被跳过。启动时提供的路径扫描到音乐后
会替换已保存的播放列表；不提供路径时则恢复上次的列表。未扫描到音乐时不会覆盖原列表。

### 选择播放后端

默认使用 mpv。也可以显式指定：

```sh
./musicplayertui --backend=mpv ~/Music
./musicplayertui --backend=ffmpeg ~/Music
./musicplayertui --backend=auto ~/Music
```

或使用环境变量（命令行参数优先）：

```sh
MUSICPLAYERTUI_BACKEND=ffmpeg ./musicplayertui ~/Music
```

- `mpv`：默认后端，通过 mpv 子进程播放。
- `ffmpeg`：使用内置 FFmpeg + PulseAudio 后端；后端不可用时提示错误，不回退。
- `auto`：优先尝试 FFmpeg，失败时自动回退 mpv。

当前使用的后端会显示在界面标题栏中。后端选择不会写入持久化状态。

### 在程序中添加音乐

按 `a` 打开路径输入栏，输入音乐文件或文件夹路径后按 `Enter`：

```text
 添加路径: ~/Music/_
```

- 支持绝对路径、相对路径和 `~/` 开头的路径
- 可以输入 UTF-8 中文路径
- `Backspace` 删除字符，`Esc` 取消输入
- 添加文件夹时会递归扫描其中的音频文件
- 重复路径会自动去重
- 添加成功后会立即保存，下次无参数启动时自动恢复

### 数据持久化

播放器会保存以下数据：

- 音乐来源路径（启动参数或程序内添加的文件/文件夹）
- 音量
- 循环模式
- 随机播放状态
- 当前选中的曲目

状态文件位置：

```text
$XDG_STATE_HOME/musicplayertui/state
```

如果未设置 `XDG_STATE_HOME`，使用：

```text
~/.local/state/musicplayertui/state
```

状态在添加音乐后以及正常退出时保存。静音状态和播放进度不会保存。

## 快捷键

| 按键 | 功能 |
| --- | --- |
| `j` / `↓` | 向下移动选择 |
| `k` / `↑` | 向上移动选择 |
| `PageUp` / `PageDown` | 翻页 |
| `g` / `Home` | 跳到列表顶部 |
| `G` / `End` | 跳到列表底部 |
| `Enter` | 播放选中的曲目 |
| `Space` | 暂停 / 继续 |
| `n` / `p` | 下一首 / 上一首 |
| `h` / `←` | 后退 5 秒 |
| `l` / `→` | 前进 5 秒 |
| `+` / `=` | 音量 +5% |
| `-` / `_` | 音量 -5% |
| `m` | 静音 / 取消静音 |
| `r` | 切换循环模式：关闭 → 列表循环 → 单曲循环 |
| `s` | 开启 / 关闭随机播放 |
| `a` | 添加音乐文件或文件夹 |
| `Esc` | 取消路径输入 |
| `q` / `Ctrl-C` | 退出 |

## 项目结构

```text
src/
  main.c      主循环、按键处理、界面渲染
  tui.c/h     终端 raw 模式、ANSI 绘制、UTF-8 裁剪、按键解析
  playlist.c/h 音乐目录扫描、排序、播放列表
  state.c/h   播放列表与设置持久化
  player.c/h  播放后端抽象、选择与回退
  mpv.c/h     mpv 子进程和 JSON IPC 控制
  ffmpeg_player.c/h  libav* 解码 + PulseAudio 输出
```

播放器启动一个 `mpv --idle` 子进程，并通过 `/tmp/musicplayertui-*/mpv.sock`
向它发送 `loadfile`、`seek`、`set_property` 等命令，同时轮询
`idle-active`、`time-pos`、`duration`、`pause`、`volume` 属性来更新界面。

FFmpeg 后端在进程内打开音频文件，用 libavformat/libavcodec 解码，
经 libswresample 转为 48 kHz 立体声 S16，再通过 PulseAudio 输出。
播放时会读取用户自己的 mpv 配置；循环、恢复播放、视频窗口等行为会被程序参数覆盖，
避免影响 TUI。
