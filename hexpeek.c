/*
 * hexpeek —— 一个两百多行的迷你 hexdump，用来把任意文件按字节「看」出来。
 *
 * 编译：  gcc -O2 -std=c11 -Wall -Wextra -o hexpeek hexpeek.c
 * 运行：  ./hexpeek hexpeek.c
 *
 * 这个程序的全部难点其实只有一个：把一块 unsigned char 缓冲区
 * 排版成对齐的十六进制文本。剩下的都是 fopen / fread / fclose 的耐心。
 *
 * 代码顺序：
 *   1. 选项结构与数字解析
 *   2. 三种输出格式（规范 / 紧凑 / 纯 ASCII）
 *   3. 读取与跳过
 *   4. 命令行解析
 *   5. main
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "1.0.0"

/* 每行最多显示多少字节；缓冲区要比它大，方便以后放宽限制 */
#define MAX_WIDTH 64
#define LINE_BUFFER 256

/* 三种输出样式 */
enum mode {
    MODE_CANON = 0, /* 十六进制 + ASCII，类似 hexdump -C */
    MODE_COMPACT,   /* 只要十六进制 */
    MODE_ASCII      /* 只要 ASCII */
};

struct options {
    long long skip;    /* 从文件的第几个字节开始看 */
    long long length;  /* 最多看多少字节；-1 表示看到文件结尾 */
    int width;         /* 每行显示多少字节 */
    enum mode mode;
};

/* ------------------------------------------------------------------ */
/* 1. 选项与数字解析                                                   */
/* ------------------------------------------------------------------ */

/*
 * 把 "16" / "0x10" / "1K" / "2M" 这样的字符串解析成整数。
 *
 * 用 strtoll 的 base=0：它会自己认出 "0x" 是十六进制、"0" 开头是八进制。
 * 返回值：0 成功，-1 失败。
 */
static int parse_number(const char *text, long long *out)
{
    char *end = NULL;
    long long value;

    if (text == NULL || *text == '\0') {
        return -1;
    }

    errno = 0;
    value = strtoll(text, &end, 0);
    if (end == text || errno != 0 || value < 0) {
        return -1;
    }

    /* 结尾可能还有一个容量单位，比如 4K = 4096 */
    if (*end != '\0') {
        long long unit;

        switch (*end) {
        case 'k':
        case 'K':
            unit = 1024LL;
            break;
        case 'm':
        case 'M':
            unit = 1024LL * 1024LL;
            break;
        case 'g':
        case 'G':
            unit = 1024LL * 1024LL * 1024LL;
            break;
        default:
            return -1; /* 不认识的尾巴，说明用户写错了 */
        }
        end++;
        if (*end != '\0') {
            return -1;
        }
        value *= unit;
    }

    *out = value;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 2. 三种输出格式                                                     */
/* ------------------------------------------------------------------ */

/*
 * 可打印字符的判断：0x20 ~ 0x7E 之间的 ASCII 才算。
 * 注意参数一定要用 unsigned char —— 用 char 的话 0x80 以上会变成负数，
 * 传给 isprint 属于未定义行为（这是 C 里非常经典的一个坑）。
 */
static int is_printable(unsigned char c)
{
    return c >= 0x20 && c <= 0x7e;
}

/*
 * 规范格式，一行长这样：
 *
 *   00000000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 0a        |Hello, world!.|
 *   ^偏移    ^前 8 个字节            ^后 8 个字节            ^ASCII 列
 *
 * 不足一行时，十六进制部分用空格补齐，ASCII 部分只显示真实存在的数据。
 */
static void print_canon(const unsigned char *data, size_t count,
                        long long offset, int width)
{
    int i;
    int half = width / 2; /* 中间空一格，16 字节时就是 8/8 分栏 */

    printf("%08llx  ", (unsigned long long)offset);

    for (i = 0; i < width; i++) {
        if (i < (int)count) {
            printf("%02x ", (unsigned)data[i]);
        } else {
            printf("   "); /* 没数据的地方留白，保证列对齐 */
        }
        if (half > 0 && i + 1 == half && i + 1 < width) {
            putchar(' '); /* 两栏之间的分隔空格 */
        }
    }

    putchar(' ');
    putchar('|');
    for (i = 0; i < (int)count; i++) {
        putchar(is_printable(data[i]) ? (int)data[i] : '.');
    }
    putchar('|');
    putchar('\n');
}

/* 紧凑格式：只要十六进制，没有 ASCII 列 */
static void print_compact(const unsigned char *data, size_t count, long long offset)
{
    size_t i;

    printf("%08llx  ", (unsigned long long)offset);
    for (i = 0; i < count; i++) {
        if (i > 0) {
            putchar(' ');
        }
        printf("%02x", (unsigned)data[i]);
    }
    putchar('\n');
}

/* 纯 ASCII 格式：把文件当成文本看，不可打印的字节显示成 '.' */
static void print_ascii(const unsigned char *data, size_t count, long long offset)
{
    size_t i;

    printf("%08llx  |", (unsigned long long)offset);
    for (i = 0; i < count; i++) {
        putchar(is_printable(data[i]) ? (int)data[i] : '.');
    }
    printf("|\n");
}

/* 把缓冲区里的一行按当前模式输出 */
static void print_line(const unsigned char *data, size_t count,
                       long long offset, const struct options *opt)
{
    switch (opt->mode) {
    case MODE_COMPACT:
        print_compact(data, count, offset);
        break;
    case MODE_ASCII:
        print_ascii(data, count, offset);
        break;
    case MODE_CANON:
    default:
        print_canon(data, count, offset, opt->width);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 3. 读取与跳过                                                       */
/* ------------------------------------------------------------------ */

/*
 * 跳过前 n 个字节。
 *
 * 这里故意用「读一点丢一点」而不是 fseek：
 *   - Windows 的 fseek 只认 long，文件大于 2GB 就溢出了，要用 _fseeki64；
 *   - 标准输入根本没法 seek。
 * 统一用读循环，两个问题一起消失，代价是大偏移会慢一点。
 *
 * 返回实际跳过的字节数（文件比 n 短时可能小于 n）。
 */
static long long skip_bytes(FILE *in, long long n)
{
    unsigned char scratch[4096];
    long long done = 0;

    while (done < n) {
        long long left = n - done;
        size_t want = (left < (long long)sizeof(scratch)) ? (size_t)left : sizeof(scratch);
        size_t got = fread(scratch, 1, want, in);

        if (got == 0) {
            break; /* 到文件结尾了 */
        }
        done += (long long)got;
    }
    return done;
}

/*
 * 从 in 里读出数据并一行行打印。
 *
 * start  : 打印在偏移列里的第一个数字（跳过的字节数）
 * limit  : 最多读多少字节，-1 表示不限制
 */
static void dump_stream(FILE *in, long long start, long long limit, const struct options *opt)
{
    unsigned char buffer[MAX_WIDTH];
    long long offset = start;
    long long remaining = limit;
    int width = opt->width;

    for (;;) {
        size_t want = (size_t)width;
        size_t got;

        if (remaining == 0) {
            break; /* 用户限定了长度，已经读够了 */
        }
        if (remaining > 0 && remaining < (long long)want) {
            want = (size_t)remaining;
        }

        got = fread(buffer, 1, want, in);
        if (got == 0) {
            break; /* 正常读完，或者读出错 —— 错误在下面统一处理 */
        }

        print_line(buffer, got, offset, opt);
        offset += (long long)got;

        if (remaining > 0) {
            remaining -= (long long)got;
        }
    }

    fflush(stdout);
}

/* 打开一个文件并输出它的内容。返回 0 成功，-1 失败。 */
static int process_file(const char *path, const struct options *opt)
{
    FILE *in;
    long long skipped;

    in = fopen(path, "rb"); /* "rb"：二进制模式，Windows 上千万别省 b */
    if (in == NULL) {
        fprintf(stderr, "hexpeek: 打不开 %s: %s\n", path, strerror(errno));
        return -1;
    }

    skipped = (opt->skip > 0) ? skip_bytes(in, opt->skip) : 0;
    dump_stream(in, skipped, opt->length, opt);

    if (ferror(in)) {
        fprintf(stderr, "hexpeek: 读取 %s 时出错: %s\n", path, strerror(errno));
        fclose(in);
        return -1;
    }

    fclose(in);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 4. 命令行解析                                                       */
/* ------------------------------------------------------------------ */

static void usage(FILE *out)
{
    fprintf(out,
            "用法: hexpeek [选项] [文件...]\n"
            "\n"
            "把文件按字节以十六进制 / ASCII 打印出来；不给文件名则读标准输入。\n"
            "\n"
            "选项:\n"
            "  -s, --skip N      从第 N 个字节开始（支持 0x10、1K、2M）\n"
            "  -n, --length N    最多显示 N 个字节（默认看到结尾）\n"
            "  -w, --width N     每行显示 N 个字节，1~%d（默认 16）\n"
            "  -c, --compact     只显示十六进制，不要 ASCII 列\n"
            "  -a, --ascii       只显示 ASCII 字符\n"
            "  -h, --help        显示这份帮助\n"
            "  -V, --version     显示版本号\n"
            "\n"
            "示例:\n"
            "  hexpeek a.out                 # 看一个二进制文件\n"
            "  hexpeek -s 0x100 -n 64 a.out  # 只看看第 256 字节开始的 64 个字节\n"
            "  cat a.txt | hexpeek -a        # 从标准输入读，只看文本\n",
            MAX_WIDTH);
}

/* 选项表：一个结构体数组，比一长串 if-else 好维护得多 */
struct option_spec {
    const char *long_name;
    char short_name;
    int takes_value;
    int id;
};

enum {
    OPT_SKIP = 1,
    OPT_LENGTH,
    OPT_WIDTH,
    OPT_COMPACT,
    OPT_ASCII,
    OPT_HELP,
    OPT_VERSION
};

static const struct option_spec OPTIONS[] = {
    {"skip", 's', 1, OPT_SKIP},
    {"length", 'n', 1, OPT_LENGTH},
    {"width", 'w', 1, OPT_WIDTH},
    {"compact", 'c', 0, OPT_COMPACT},
    {"ascii", 'a', 0, OPT_ASCII},
    {"help", 'h', 0, OPT_HELP},
    {"version", 'V', 0, OPT_VERSION},
};

#define OPTION_COUNT ((int)(sizeof(OPTIONS) / sizeof(OPTIONS[0])))

/*
 * 解析命令行。
 *
 * 规则（写在这里，省得以后忘了）：
 *   - 支持 "-s 16"、"-s16"、"--skip 16"、"--skip=16" 四种写法；
 *   - 遇到第一个不是选项的参数就停下，后面的全部当成文件名；
 *   - "--" 之后的一律当成文件名（所以真有个文件叫 "-n" 也能处理）。
 *
 * 返回 0 表示继续，返回 1 表示已经把 --help / --version 处理完了，
 * 返回 -1 表示参数有错。
 */
static int parse_options(int argc, char **argv, struct options *opt, int *file_start)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;
        const struct option_spec *spec = NULL;
        int id = 0;
        int k;

        /* "--" 之后全是文件名 */
        if (strcmp(arg, "--") == 0) {
            *file_start = i + 1;
            return 0;
        }
        /* 不是选项（含单独一个 "-" 表示标准输入），到此为止 */
        if (arg[0] != '-' || arg[1] == '\0') {
            *file_start = i;
            return 0;
        }

        if (arg[1] == '-') { /* 长选项 --name 或 --name=value */
            const char *name = arg + 2;
            const char *eq = strchr(name, '=');

            for (k = 0; k < OPTION_COUNT; k++) {
                size_t len = eq ? (size_t)(eq - name) : strlen(name);

                if (strlen(OPTIONS[k].long_name) == len &&
                    strncmp(OPTIONS[k].long_name, name, len) == 0) {
                    spec = &OPTIONS[k];
                    break;
                }
            }
            if (spec == NULL) {
                fprintf(stderr, "hexpeek: 不认识的选项 %s\n", arg);
                return -1;
            }
            if (spec->takes_value) {
                if (eq != NULL) {
                    value = eq + 1;
                } else if (i + 1 < argc) {
                    value = argv[++i];
                } else {
                    fprintf(stderr, "hexpeek: 选项 %s 需要一个参数\n", arg);
                    return -1;
                }
            } else if (eq != NULL) {
                fprintf(stderr, "hexpeek: 选项 %s 不接受参数\n", arg);
                return -1;
            }
        } else { /* 短选项 -s / -s16 */
            for (k = 0; k < OPTION_COUNT; k++) {
                if (OPTIONS[k].short_name == arg[1]) {
                    spec = &OPTIONS[k];
                    break;
                }
            }
            if (spec == NULL) {
                fprintf(stderr, "hexpeek: 不认识的选项 %s\n", arg);
                return -1;
            }
            if (spec->takes_value) {
                if (arg[2] != '\0') {
                    value = arg + 2; /* -s16 这种「贴着写」的写法 */
                } else if (i + 1 < argc) {
                    value = argv[++i];
                } else {
                    fprintf(stderr, "hexpeek: 选项 %s 需要一个参数\n", arg);
                    return -1;
                }
            } else if (arg[2] != '\0') {
                fprintf(stderr, "hexpeek: 选项 %c 后面不能带东西（写成 -c -a 而不是 -ca）\n", arg[1]);
                return -1;
            }
        }

        id = spec->id;

        if (id == OPT_HELP) {
            usage(stdout);
            return 1;
        }
        if (id == OPT_VERSION) {
            printf("hexpeek %s\n", VERSION);
            return 1;
        }
        if (id == OPT_COMPACT) {
            opt->mode = MODE_COMPACT;
            continue;
        }
        if (id == OPT_ASCII) {
            opt->mode = MODE_ASCII;
            continue;
        }

        /* 剩下三个都要一个数字 */
        if (id == OPT_SKIP || id == OPT_LENGTH) {
            long long number;

            if (parse_number(value, &number) != 0) {
                fprintf(stderr, "hexpeek: 看不懂这个数字：%s\n", value);
                return -1;
            }
            if (id == OPT_SKIP) {
                opt->skip = number;
            } else {
                opt->length = number;
            }
        } else if (id == OPT_WIDTH) {
            long long number;

            if (parse_number(value, &number) != 0 || number < 1 || number > MAX_WIDTH) {
                fprintf(stderr, "hexpeek: --width 需要 1~%d 之间的整数，收到 %s\n", MAX_WIDTH, value);
                return -1;
            }
            opt->width = (int)number;
        }
    }

    *file_start = argc; /* 一个文件名都没给 */
    return 0;
}

/* ------------------------------------------------------------------ */
/* 5. main                                                             */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct options opt;
    int file_start = argc;
    int rc;
    int i;
    int status = 0;

    opt.skip = 0;
    opt.length = -1;
    opt.width = 16;
    opt.mode = MODE_CANON;

    rc = parse_options(argc, argv, &opt, &file_start);
    if (rc > 0) {
        return 0; /* --help / --version 已经处理完 */
    }
    if (rc < 0) {
        fprintf(stderr, "试试 hexpeek --help\n");
        return 2;
    }

    /* 紧凑模式下默认一行 32 字节，纯 ASCII 模式默认 64 字节，更容易看 */
    if (opt.width == 16 && opt.mode == MODE_COMPACT) {
        opt.width = 32;
    } else if (opt.width == 16 && opt.mode == MODE_ASCII) {
        opt.width = 64;
    }

    if (file_start >= argc) {
        /* 没有文件名：从标准输入读 */
        dump_stream(stdin, 0, opt.length, &opt);
        if (ferror(stdin)) {
            fprintf(stderr, "hexpeek: 读标准输入出错: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }

    for (i = file_start; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            dump_stream(stdin, 0, opt.length, &opt);
        } else if (process_file(argv[i], &opt) != 0) {
            status = 1; /* 有文件失败，但继续处理后面的 */
        }
    }

    return status;
}
