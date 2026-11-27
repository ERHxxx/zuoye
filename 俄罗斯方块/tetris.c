/*
 * ============================================================
 *  俄罗斯方块（控制台版）
 *  环境：Visual Studio，新建 C++ 空项目或 .c 文件直接编译运行
 *  玩法：← → 移动，↑ 旋转，↓ 加速下落，空格 直接落底
 *        P 暂停，R 游戏结束后重开，Esc 退出
 * ============================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <conio.h>
#include <windows.h>

/* disableResize 用到了 user32.lib 的窗口 API，手动告诉链接器 */
#pragma comment(lib, "user32.lib")

#define FIELD_W 10          /* 场地宽（格）   */
#define FIELD_H 20          /* 场地高（格）   */
#define LEFT    2           /* 场地左上角列   */
#define TOP     1           /* 场地左上角行   */
#define CON_W   64          /* 控制台总列数（固定不变） */
#define CON_H   26          /* 控制台总行数（固定不变） */

/* 7 种方块的初始形状（4x4 矩阵） */
static const int SHAPES[7][4][4] = {
    { {0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0} },  /* I */
    { {1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0} },  /* J */
    { {0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0} },  /* L */
    { {1,1,0,0},{1,1,0,0},{0,0,0,0},{0,0,0,0} },  /* O */
    { {0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0} },  /* S */
    { {0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0} },  /* T */
    { {1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0} }   /* Z */
};

/* 每种方块对应的颜色（Windows 控制台色值） */
static const int COLORS[8] = {
    8, 11, 9, 6, 14, 10, 13, 12
};

static int  field[FIELD_H][FIELD_W];   /* 场地：0 空，1~7 为方块类型 */
static int  cur[4][4];                 /* 当前方块形状 */
static int  next[4][4];                /* 下一个方块形状 */
static int  curColor, nextColor;
static int  curX, curY;                /* 当前方块左上角在场地的坐标 */
static int  score, lines, level;
static int  gameOver, paused;

/* ---------- 基础工具 ---------- */

static void gotoxy(int x, int y)
{
    COORD c = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), c);
}

static void setColor(int c)
{
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)c);
}

static void hideCursor(void)
{
    CONSOLE_CURSOR_INFO ci = { 1, 0 };
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci);
}

/* ---------- 固定控制台：边界和方块大小恒定 ---------- */

/* 固定字体和字号（Consolas 8x16）：
 * 一个格子 = 2 个空格 = 2 列宽，字号固定了格子宽度就不会变 */
static void fixFont(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_FONT_INFOEX cfi;
    ZeroMemory(&cfi, sizeof(cfi));
    cfi.cbSize = sizeof(cfi);
    cfi.dwFontSize.X = 8;
    cfi.dwFontSize.Y = 16;
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy_s(cfi.FaceName, LF_FACESIZE, L"Consolas");
    SetCurrentConsoleFontEx(hOut, FALSE, &cfi);
}

/* 把窗口和缓冲区锁回 64x26。
 * 注意顺序：窗口矩形不能大于缓冲区，所以先把窗口缩小，再缩缓冲区，
 * 最后再把窗口贴到左上角。 */
static void lockSize(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    SMALL_RECT win = { 0, 0, CON_W - 1, CON_H - 1 };
    COORD size = { CON_W, CON_H };
    if (!GetConsoleScreenBufferInfo(hOut, &csbi))
        return;                       /* 无控制台（输出被重定向）时直接跳过 */
    if (csbi.srWindow.Right >= CON_W || csbi.srWindow.Bottom >= CON_H)
        SetConsoleWindowInfo(hOut, TRUE, &win);
    if (csbi.dwSize.X != CON_W || csbi.dwSize.Y != CON_H)
        SetConsoleScreenBufferSize(hOut, size);
    SetConsoleWindowInfo(hOut, TRUE, &win);
}

/* 去掉窗口的可缩放边框和最大化按钮，从源头禁止改变窗口大小 */
static void disableResize(void)
{
    HWND hwnd = GetConsoleWindow();
    HMENU menu;
    LONG_PTR style;
    if (!hwnd)
        return;
    style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(LONG_PTR)WS_THICKFRAME & ~(LONG_PTR)WS_MAXIMIZEBOX;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    menu = GetSystemMenu(hwnd, FALSE);
    if (menu) {
        DeleteMenu(menu, SC_SIZE, MF_BYCOMMAND);
        DeleteMenu(menu, SC_MAXIMIZE, MF_BYCOMMAND);
    }
    /* 通知系统框架样式已变，刷新边框 */
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

/* 每帧检测：用户若通过任何方式改了窗口大小或字号，下一帧立即恢复 */
static void keepConsoleFixed(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof(cfi);
    if (!GetConsoleScreenBufferInfo(hOut, &csbi))
        return;
    if (csbi.dwSize.X != CON_W || csbi.dwSize.Y != CON_H ||
        csbi.srWindow.Right != CON_W - 1 || csbi.srWindow.Bottom != CON_H - 1)
        lockSize();
    /* 字号被改（如误按 Ctrl+滚轮缩放）就改回来 */
    if (GetCurrentConsoleFontEx(hOut, FALSE, &cfi) &&
        (cfi.dwFontSize.X != 8 || cfi.dwFontSize.Y != 16))
        fixFont();
}

/* 在屏幕指定位置画一个格子（filled 非 0 时画实心块）
 * 原理：控制台里"空格"显示的是单元格的背景色，
 * 所以用背景色画两个空格，就是一块 2 列宽的实心方块，
 * 不依赖特殊字符和字体，任何终端都不会错位 */
static void drawCellAt(int sx, int sy, int color, int filled)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    gotoxy(sx, sy);
    if (filled)
        SetConsoleTextAttribute(hOut, (WORD)((color << 4) & 0xFF));  /* 同色背景 */
    else
        SetConsoleTextAttribute(hOut, 8);                           /* 深灰空格 */
    printf("  ");
    SetConsoleTextAttribute(hOut, 7);   /* 恢复默认颜色 */
}

/* 画场地坐标 (x,y) 处的格子 */
static void drawCell(int x, int y, int color, int filled)
{
    drawCellAt(LEFT + x * 2, TOP + 1 + y, color, filled);
}

/* ---------- 游戏逻辑 ---------- */

/* 以 (px,py) 为左上角放置形状 s 时是否发生碰撞 */
static int collide(const int s[4][4], int px, int py)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            if (!s[i][j]) continue;
            int x = px + j, y = py + i;
            if (x < 0 || x >= FIELD_W || y >= FIELD_H) return 1;
            if (y >= 0 && field[y][x]) return 1;
        }
    return 0;
}

/* 顺时针旋转 4x4 矩阵 */
static void rotate(int s[4][4])
{
    int t[4][4], i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            t[i][j] = s[3 - j][i];
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            s[i][j] = t[i][j];
}

/* 把当前方块固定到场地上 */
static void merge(void)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            if (cur[i][j] && curY + i >= 0)
                field[curY + i][curX + j] = curColor;
}

/* 消除满行，返回消除行数 */
static int clearLines(void)
{
    int y, x, i, count = 0;
    for (y = FIELD_H - 1; y >= 0; y--) {
        int full = 1;
        for (x = 0; x < FIELD_W; x++)
            if (!field[y][x]) { full = 0; break; }
        if (full) {
            count++;
            for (i = y; i > 0; i--)
                for (x = 0; x < FIELD_W; x++)
                    field[i][x] = field[i - 1][x];
            for (x = 0; x < FIELD_W; x++)
                field[0][x] = 0;
            y++;  /* 重新检查同一行 */
        }
    }
    return count;
}

/* 生成新方块（next -> cur，再随机生成 next） */
static void spawn(void)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            cur[i][j] = next[i][j];
    curColor = nextColor;

    {
        static const int (*src)[4] = NULL; (void)src;
    }
    {
        int t = rand() % 7, i2, j2;
        for (i2 = 0; i2 < 4; i2++)
            for (j2 = 0; j2 < 4; j2++)
                next[i2][j2] = SHAPES[t][i2][j2];
        nextColor = t + 1;
    }

    curX = FIELD_W / 2 - 2;
    curY = -1;
    if (collide(cur, curX, curY))
        gameOver = 1;
}

/* 重置整局游戏 */
static void resetGame(void)
{
    int i, j;
    for (i = 0; i < FIELD_H; i++)
        for (j = 0; j < FIELD_W; j++)
            field[i][j] = 0;
    score = lines = level = 0;
    gameOver = paused = 0;

    {
        int t = rand() % 7;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                next[i][j] = SHAPES[t][i][j];
        nextColor = t + 1;
    }
    spawn();
}

/* 当前方块下落一格；到底则固定、消行、生成新方块 */
static void drop(void)
{
    if (!collide(cur, curX, curY + 1)) {
        curY++;
    } else {
        merge();
        {
            int n = clearLines();
            if (n > 0) {
                static const int pts[5] = { 0, 100, 300, 500, 800 };
                lines += n;
                score += pts[n] * level;
                if (level < lines / 10 + 1) level = lines / 10 + 1;
                Beep(523, 60);
            }
        }
        spawn();
    }
}

/* ---------- 绘制 ---------- */

/* 画固定边框和右侧信息栏（只需画一次） */
static void drawFrame(void)
{
    int x, y;
    system("cls");
    setColor(7);

    /* 上、下边框 */
    for (x = LEFT - 1; x <= LEFT + FIELD_W * 2; x++) {
        gotoxy(x, TOP);
        printf("-");
        gotoxy(x, TOP + FIELD_H + 1);
        printf("-");
    }
    /* 左、右边框 */
    for (y = TOP + 1; y <= TOP + FIELD_H; y++) {
        gotoxy(LEFT - 1, y);
        printf("|");
        gotoxy(LEFT + FIELD_W * 2, y);
        printf("|");
    }

    /* 右侧信息栏 */
    {
        int px = LEFT + FIELD_W * 2 + 3;
        setColor(7);
        gotoxy(px, TOP + 1);  printf("俄罗斯方块");
        gotoxy(px, TOP + 3);  printf("分数 : %d", score);
        gotoxy(px, TOP + 4);  printf("等级 : %d", level);
        gotoxy(px, TOP + 5);  printf("消行 : %d", lines);
        gotoxy(px, TOP + 7);  printf("下一个:");
        gotoxy(px, TOP + 14); printf("[← →] 移动");
        gotoxy(px, TOP + 15); printf("[ ↑ ] 旋转");
        gotoxy(px, TOP + 16); printf("[ ↓ ] 加速");
        gotoxy(px, TOP + 17); printf("[空格] 落底");
        gotoxy(px, TOP + 18); printf("[ P ] 暂停");
        gotoxy(px, TOP + 19); printf("[Esc] 退出");
    }
}

/* 每帧刷新：场地 + 当前方块 + 分数区 */
static void draw(void)
{
    int x, y, i, j;

    keepConsoleFixed();   /* 窗口/字号一旦被改动，立刻恢复，边界和方块大小不变 */

    /* 场地 */
    for (y = 0; y < FIELD_H; y++)
        for (x = 0; x < FIELD_W; x++)
            drawCell(x, y, field[y][x] ? COLORS[field[y][x]] : 8, field[y][x]);

    /* 当前方块 */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            if (cur[i][j] && curY + i >= 0)
                drawCell(curX + j, curY + i, COLORS[curColor], 1);

    /* 下一个方块预览 */
    {
        int px = LEFT + FIELD_W * 2 + 3;
        setColor(8);
        for (i = 0; i < 4; i++) {
            gotoxy(px, TOP + 8 + i);
            printf("        ");
        }
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++)
                if (next[i][j])
                    drawCellAt(px + j * 2, TOP + 8 + i, COLORS[nextColor], 1);
    }

    /* 分数刷新 */
    {
        int px = LEFT + FIELD_W * 2 + 3;
        setColor(7);
        gotoxy(px, TOP + 3);  printf("分数 : %d   ", score);
        gotoxy(px, TOP + 4);  printf("等级 : %d   ", level);
        gotoxy(px, TOP + 5);  printf("消行 : %d   ", lines);
    }

    /* 暂停 / 结束提示 */
    if (paused && !gameOver) {
        setColor(0xEC);  /* 亮黄底 */
        gotoxy(LEFT + 2, TOP + FIELD_H / 2);
        printf("   已暂停，按 P 继续   ");
    }
    if (gameOver) {
        setColor(0xCE);  /* 红底 */
        gotoxy(LEFT + 2, TOP + FIELD_H / 2 - 1);
        printf("     游戏结束！        ");
        gotoxy(LEFT + 2, TOP + FIELD_H / 2);
        printf("   按 R 重开 / Esc 退出 ");
    }
    setColor(7);
}

/* ---------- 主函数 ---------- */

int main(void)
{
    int ch = 0, lastDrop, interval;
    DWORD now;

    fixFont();      /* 固定字体字号：方块大小恒定 */
    lockSize();     /* 锁定窗口/缓冲区大小：边界恒定 */
    disableResize();/* 禁止拖动边框和最大化 */
    SetConsoleTitleA("俄罗斯方块 - C语言控制台版");
    srand((unsigned)time(NULL));
    hideCursor();

    resetGame();
    drawFrame();

    lastDrop = GetTickCount();

    while (1) {
        /* ---- 输入 ---- */
        while (_kbhit()) {
            ch = _getch();
            if (ch == 224 || ch == 0) {          /* 方向键有两个字节 */
                ch = _getch();
                if (gameOver) continue;
                if (ch == 75 && !collide(cur, curX - 1, curY)) curX--;   /* ← */
                else if (ch == 77 && !collide(cur, curX + 1, curY)) curX++; /* → */
                else if (ch == 80 && !paused) drop();                     /* ↓ */
                else if (ch == 72 && !paused) {                           /* ↑ */
                    int old[4][4], ox = curX, i, j;
                    for (i = 0; i < 4; i++)
                        for (j = 0; j < 4; j++)
                            old[i][j] = cur[i][j];
                    rotate(cur);
                    /* 贴墙旋转时尝试左右挪一点 */
                    if (collide(cur, curX, curY)) {
                        int k;
                        for (k = 1; k <= 2; k++) {
                            if (!collide(cur, curX - k, curY)) { curX -= k; break; }
                            if (!collide(cur, curX + k, curY)) { curX += k; break; }
                        }
                        if (collide(cur, curX, curY)) {
                            curX = ox;
                            for (i = 0; i < 4; i++)
                                for (j = 0; j < 4; j++)
                                    cur[i][j] = old[i][j];
                        }
                    }
                }
            } else if (ch == ' ') {              /* 空格：直接落底 */
                if (!gameOver && !paused) {
                    while (!collide(cur, curX, curY + 1)) {
                        curY++;
                        score += 2;
                    }
                    drop();
                }
            } else if (ch == 27) {               /* Esc */
                break;
            } else if (ch == 'p' || ch == 'P') {
                if (!gameOver) paused = !paused;
            } else if (ch == 'r' || ch == 'R') {
                if (gameOver) {
                    resetGame();
                    drawFrame();
                }
            }
        }

        /* ---- 自动下落 ---- */
        now = GetTickCount();
        interval = 800 - (level - 1) * 70;
        if (interval < 80) interval = 80;
        if (!gameOver && !paused && (int)(now - lastDrop) >= interval) {
            drop();
            lastDrop = now;
        }

        /* ---- 渲染 ---- */
        draw();
        Sleep(16);

        if (ch == 27) break;   /* Esc 退出主循环 */
    }

    gotoxy(0, TOP + FIELD_H + 3);
    setColor(7);
    printf("再见！\n");
    return 0;
}
