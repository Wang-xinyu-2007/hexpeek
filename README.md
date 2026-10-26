# hexpeek

一个迷你 `hexdump`：把任意文件按字节读出来，左边十六进制、右边 ASCII，并排看。

**C11，零依赖**——一个 `.c` 文件、一个 `Makefile`，`make` 就能编，没有构建系统要学。

![C](https://img.shields.io/badge/C-C11-blue)
![Dependencies](https://img.shields.io/badge/dependencies-0-brightgreen)
![Tests](https://github.com/Wang-xinyu-2007/hexpeek/actions/workflows/tests.yml/badge.svg)

---

## 为什么写这个

学 C 到「指针、数组、文件读写」这一章时，最缺的不是语法，而是一个**能看见结果**的练习：
读写文件、按字节处理、格式化输出、解析命令行，全都在这一个二百多行的小工具里。

它也不只是练习题——初学 C 的时候，「文件打不开到底是不是路径写错了」「这文件是不是下载坏了」
「这个格式的文件头到底是什么」，`hexpeek` 都能直接回答。

---

## 功能

- **规范格式**：偏移 + 16 进制 + ASCII，和 `hexdump -C` 一个样子
- **三种视图**：默认规范格式、`-c` 只要十六进制、`-a` 只要 ASCII
- **指定范围**：`-s` 从第几个字节开始（支持 `0x10`、`1K`、`2M`），`-n` 最多看多少字节
- **每行宽度**：`-w` 可调（1~64 字节）
- **管道友好**：不给文件名就读标准输入，`cat a.bin | hexpeek` 直接能用
- **多个文件**：一次看好几个，某个打不开只影响它自己
- **错误处理**：文件不存在退出码 1，参数写错退出码 2

---

## 编译

Linux / macOS（需要 gcc 和 make）：

```bash
make
```

Windows（MinGW，命令行里执行）：

```bash
gcc -O2 -std=c11 -Wall -Wextra -Wpedantic -o hexpeek.exe hexpeek.c
```

装的是 MSVC 的话：

```bash
cl /O2 hexpeek.c
```

---

## 用法

```bash
hexpeek 文件名                  # 看整个文件
hexpeek -n 64 文件名            # 只看前 64 个字节
hexpeek -s 0x100 -n 32 文件名   # 从第 256 字节开始看 32 个
hexpeek -c 文件名               # 只要十六进制，不要右边的 ASCII
hexpeek -a 文件名               # 只看文本部分
hexpeek -w 8 文件名             # 一行 8 个字节
cat a.bin | hexpeek -n 16       # 从标准输入读
hexpeek --help                  # 全部参数
```

| 选项 | 说明 |
| --- | --- |
| `-s, --skip N` | 从第 N 个字节开始，支持 `0x10`、`1K`、`2M` |
| `-n, --length N` | 最多显示 N 个字节，默认看到结尾 |
| `-w, --width N` | 每行显示 N 个字节，1~64，默认 16 |
| `-c, --compact` | 只显示十六进制 |
| `-a, --ascii` | 只显示 ASCII 字符 |
| `-h, --help` | 帮助 |
| `-V, --version` | 版本号 |

---

## 输出示例

```bash
$ printf 'Hello, hex!\n' > hello.txt
$ hexpeek hello.txt
00000000  48 65 6c 6c 6f 2c 20 68  65 78 21 0a              |Hello, hex!.|

$ hexpeek -a hello.txt
00000000  |Hello, hex!.|

$ hexpeek -c hello.txt
00000000  48 65 6c 6c 6f 2c 20 68 65 78 21 0a

$ hexpeek -w 8 hello.txt
00000000  48 65 6c 6c  6f 2c 20 68  |Hello, h|
00000008  65 78 21 0a               |ex!.|
```

几个可以对照着看的细节：

- 左边 8 位是**偏移**（十六进制），跳过了多少字节就从多少开始
- 中间是**十六进制**，不足一行的部分用空格补齐，所以 ASCII 列永远是齐的
- 右边是 **ASCII**，不可打印的字节显示成 `.`
- 中文在 UTF-8 里占 3 个字节，所以在 ASCII 列里会看到一串点——这不是乱码，是字节

---

## 代码导读

`hexpeek.c` 分了五节，按顺序读就行：

| 小节 | 内容 | 顺便能学到 |
| --- | --- | --- |
| 1. 选项与数字解析 | `parse_number()`：认 `16` / `0x10` / `1K` | `strtoll` 的 base 参数、`errno` |
| 2. 三种输出格式 | `print_canon` / `print_compact` / `print_ascii` | `%02x`、`putchar`、补齐对齐 |
| 3. 读取与跳过 | `skip_bytes()`、`dump_stream()` | `fread` 的返回值为什么是"条目数"、`"-"` 二进制模式 |
| 4. 命令行解析 | 一张选项表 + `parse_options()` | `argv` 到底是什么、为什么不用 `getopt` |
| 5. main | 串起来 | 退出码的约定 |

几个当年踩过的坑，代码里都写了注释：

1. **`isprint` 必须传 `unsigned char`**：`char` 在 x86 上是带符号的，`0xFF` 会变成 `-1`，传给 `isprint` 是未定义行为。
2. **`fopen` 要写 `"rb"`**：Windows 上文本模式的 `\r\n` 会被偷偷转换，看二进制文件就全错了。
3. **不用 `fseek` 跳偏移**：标准 `fseek` 的偏移量是 `long`，大文件超过 2GB 就溢出，而且标准输入根本不能 seek。这里改成"读一点丢一点"，两个问题一起消失。
4. **`printf` 的格式串和参数类型必须对上**：`%d` 配 `char *` 不会报错，只会打印出一个奇怪的数字。`-Wall` 会提醒你。

---

## 项目结构

```
hexpeek/
├── hexpeek.c              # 全部实现（约 380 行代码，含注释 530 行）
├── Makefile               # make / make test / make clean
├── tests/
│   └── run_tests.py       # 25 项回归测试
├── README.md
├── LICENSE
└── .gitignore
```

---

## 跑测试

```bash
make test
```

测试脚本用 Python 写（`tests/run_tests.py`），因为它不用装 bash 或 MSYS，
Windows、Linux、macOS 上都是同一条命令。Windows 上 `python3` 不存在的话：

```bash
mingw32-make test PYTHON=python
```

测试做的是最朴素的事：造一个已知内容的文件，跑一次 `hexpeek`，把输出和期望值逐字节比对。
覆盖了规范格式 / 宽度补齐 / 跳过与长度 / 十六进制和 `K` 后缀偏移 / 紧凑和 ASCII 模式 /
标准输入 / 多文件 / 空文件 / 各种错误退出码。

---

## 已知限制

- 不支持组合短选项（`-ca` 要写成 `-c -a`）
- 偏移列固定 8 位十六进制，超过 4GB 的文件会自然变宽，表格就不再对齐
- 跳过大偏移是一个字节一个字节读过去的，几 GB 的文件会明显变慢（换来的是跨平台和标准输入都能用）
- 只按字节看，不解析任何文件格式

---

## 许可

[MIT](LICENSE)
