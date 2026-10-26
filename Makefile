# hexpeek 的构建脚本：编译、测试、清理，就这三件事。
#
#   make                  编译出 hexpeek
#   make test             编译并跑回归测试（需要 python3）
#   make clean            删掉编译产物
#
# 变量都可以在命令行覆盖，比如把警告当错误：
#   make CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror"

CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
PYTHON ?= python3

# make 自带 CC=cc，但 Windows 的 MinGW 只提供 gcc.exe，没有 cc.exe
ifeq ($(origin CC),default)
  CC := gcc
endif

# Windows 上编译产物叫 hexpeek.exe，这里自动补后缀，clean 也认得它
ifeq ($(OS),Windows_NT)
  EXE   := .exe
  CLEAN  = del /Q $(BIN) 2>NUL
else
  EXE   :=
  CLEAN  = rm -f $(BIN)
endif

BIN := hexpeek$(EXE)
SRC := hexpeek.c

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: $(BIN)
	$(PYTHON) tests/run_tests.py

clean:
	-$(CLEAN)
