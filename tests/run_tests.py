#!/usr/bin/env python3
"""hexpeek 的回归测试：固定输入 -> 固定输出，逐字节比对。

    python tests/run_tests.py

之所以用 Python 而不是 shell 脚本：它在 Linux、macOS 和 Windows 上都能直接跑，
不需要额外装 bash 或 MSYS，CI 里也只要一行命令。
被测试的程序本身还是纯 C，这一点没有变。
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

RESULTS = {"pass": 0, "fail": 0}


def find_binary() -> str:
    """找出待测程序：优先用环境变量，其次试 ./hexpeek、./hexpeek.exe。"""
    candidates = []
    if os.environ.get("HEXPEEK_BIN"):
        candidates.append(os.environ["HEXPEEK_BIN"])
    candidates += ["./hexpeek", "./hexpeek.exe", "hexpeek.exe", "hexpeek"]
    for name in candidates:
        if os.path.isfile(name):
            return name
    sys.exit("错误：找不到 hexpeek，请先在仓库根目录运行 make")


BIN = find_binary()


def normalize(data: bytes) -> bytes:
    """把连续空格压成两个，并去掉行尾空格。

    十六进制列里的补白有多少个空格取决于「这一行还剩几个字节」，
    把期望值也按同样规则压缩，测试就只关心「字节顺序对不对」，
    而不关心补了几个空格。真正的列对齐由 test_alignment 单独检查。
    """
    lines = []
    for line in data.rstrip(b"\n").split(b"\n"):
        lines.append(re.sub(rb" {2,}", b"  ", line.rstrip()))
    return b"\n".join(lines)


def run(args, stdin=b""):
    """跑一次 hexpeek，返回 (退出码, 标准输出, 标准错误)。"""
    proc = subprocess.run([BIN] + args, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.returncode, proc.stdout, proc.stderr


def check(name: str, expected, actual) -> None:
    """期望值和实际值相等就记一次通过，否则打印差异。"""
    if expected == actual:
        RESULTS["pass"] += 1
    else:
        RESULTS["fail"] += 1
        print(f"FAIL: {name}")
        print(f"  期望: {expected!r}")
        print(f"  实际: {actual!r}")


def main() -> int:
    # 自己在系统临时目录下开一个普通目录，而不用 tempfile.mkdtemp()：
    # 后者在 Windows 上是按 0700 建的，某些受限环境里连自己都进不去。
    work = Path(tempfile.gettempdir()) / f"hexpeek-tests-{uuid.uuid4().hex[:8]}"
    work.mkdir()
    try:
        # 用二进制写测试数据，才能精确控制每一个字节
        hello = work / "hello.txt"
        hello.write_bytes(b"Hello, world!\n")            # 14 字节
        exact = work / "exact.bin"
        exact.write_bytes(b"0123456789abcdef")           # 正好 16 字节
        binary = work / "bin.dat"
        binary.write_bytes(bytes([0x00, 0x01, 0x7F, 0x80, 0xFF]) + b"ABC" + b"\x00")
        empty = work / "empty.bin"
        empty.write_bytes(b"")
        big = work / "big.bin"
        big.write_bytes(b"\x00" * 2048)
        ab = work / "ab.bin"
        ab.write_bytes(b"AB")
        cd = work / "cd.bin"
        cd.write_bytes(b"CD")

        # --- 1. 规范格式 -------------------------------------------------
        _, out, _ = run([str(hello)])
        check(
            "规范格式：单行 + ASCII 列",
            b"00000000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 0a  |Hello, world!.|",
            normalize(out),
        )

        _, out, _ = run([str(exact)])
        check(
            "规范格式：正好 16 字节没有补白",
            b"00000000  30 31 32 33 34 35 36 37  38 39 61 62 63 64 65 66  |0123456789abcdef|",
            normalize(out),
        )

        _, out, _ = run([str(binary)])
        check(
            "规范格式：不可打印字节显示成点",
            b"00000000  00 01 7f 80 ff 41 42 43  00  |.....ABC.|",
            normalize(out),
        )

        _, out, _ = run(["-w", "8", str(exact)])
        check(
            "规范格式：-w 8 拆成两行",
            b"00000000  30 31 32 33  34 35 36 37  |01234567|\n"
            b"00000008  38 39 61 62  63 64 65 66  |89abcdef|",
            normalize(out),
        )

        # 不管每行数据有多少，ASCII 列都应该从同一列开始
        _, out, _ = run(["-w", "4", str(exact)])
        columns = {line.index(b"|") for line in out.splitlines()}
        check("各行 ASCII 列对齐在同一列", 1, len(columns))

        # --- 2. 偏移与长度 -----------------------------------------------
        _, out, _ = run(["-s", "6", "-n", "4", str(exact)])
        check("跳过 6 个字节", b"00000006  36 37 38 39  |6789|", normalize(out))

        _, out, _ = run(["-s", "0x400", "-n", "4", str(big)])
        check("偏移支持十六进制", b"00000400  00 00 00 00  |....|", normalize(out))

        _, out, _ = run(["-s", "1K", "-n", "2", str(big)])
        check("偏移支持 K 后缀", b"00000400  00 00  |..|", normalize(out))

        _, out, _ = run(["-s", "100", str(hello)])
        check("跳过长度超过文件时没有输出", b"", normalize(out))

        _, out, _ = run(["-n", "0", str(hello)])
        check("-n 0 什么也不输出", b"", normalize(out))

        # --- 3. 紧凑 / ASCII 模式 ----------------------------------------
        _, out, _ = run(["-c", str(hello)])
        check(
            "紧凑模式没有 ASCII 列",
            b"00000000  48 65 6c 6c 6f 2c 20 77 6f 72 6c 64 21 0a",
            normalize(out),
        )

        _, out, _ = run(["-a", str(hello)])
        check("纯 ASCII 模式", b"00000000  |Hello, world!.|", normalize(out))

        _, out, _ = run(["-c", str(big)])
        first_line = out.split(b"\n")[0]
        byte_count = len(first_line.split(b"  ", 1)[1].split())
        check("紧凑模式默认一行 32 字节", 32, byte_count)

        # --- 4. 标准输入、多文件、空文件 --------------------------------
        _, out, _ = run(["-n", "8"], stdin=exact.read_bytes())
        check("从标准输入读", b"00000000  30 31 32 33 34 35 36 37  |01234567|", normalize(out))

        _, out, _ = run(["-n", "3", "-"], stdin=b"Hi\n")
        check("用 - 表示标准输入", b"00000000  48 69 0a  |Hi.|", normalize(out))

        _, out, _ = run(["-n", "2", str(ab), str(cd)])
        check(
            "一次处理多个文件",
            b"00000000  41 42  |AB|\n00000000  43 44  |CD|",
            normalize(out),
        )

        _, out, _ = run([str(empty)])
        check("空文件没有任何输出", b"", normalize(out))

        # --- 5. 错误处理 --------------------------------------------------
        code, out, err = run([str(work / "does-not-exist")])
        check("文件不存在时退出码为 1", 1, code)
        check("文件不存在时不往标准输出写东西", b"", out)

        code, _, _ = run(["--no-such-option"])
        check("未知选项退出码为 2", 2, code)

        code, _, _ = run(["--width", "999"])
        check("宽度越界退出码为 2", 2, code)

        code, out, _ = run(["--help"])
        check("--help 退出码为 0", 0, code)
        check("--help 里有用说明", True, "用法" in out.decode("utf-8", "replace"))

        code, out, _ = run(["--version"])
        check("--version 退出码为 0", 0, code)
        check("--version 带版本号", True, b"1.0.0" in out)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n通过 {RESULTS['pass']} 项，失败 {RESULTS['fail']} 项")
    return 0 if RESULTS["fail"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
