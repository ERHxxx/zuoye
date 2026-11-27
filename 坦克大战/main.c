/* ============================================================================
 *  坦克大战 · 双人对战 Demo
 *  ---------------------------------------------------------------------------
 *  纯 C + Win32 GDI，无需任何第三方库。
 *  功能：
 *    - 迷宫地图（31×22，10×7 个房间，通道恒为 2 格宽），坦克不能穿墙、不能出界
 *    - 坦克朝向是连续角度：按 W+A 这类组合键可以斜着转向、斜着开
 *    - 两名玩家实时对战（键盘同屏双人）
 *    - 炮弹撞墙会反弹（最多反弹 5 次后消失）
 *    - 绿色砖墙 4 点血，可被炮弹打穿
 *    - 任何带伤害的东西碰到谁都掉血，不分敌我（自己的炮弹、激光、地雷同样伤自己）
 *    - 四种道具（彩色小球）：红=折射激光炮  绿=追踪弹
 *                            黄=地雷(存 2 个)  蓝=砖块(存 2 个)
 *    - 武器道具拾取即换装，固定 3 次使用机会；再拾一个就顶掉手里的旧武器
 *    - 地雷 / 砖块要按键自己找地方放：空格 放雷、K 砌砖（P2 是小键盘 1 / 0）
 *    - 阵亡时手里的道具全部作废（重生后从空手开始）
 *    - 一局 4 分钟，双方可无限复活；只要有一方阵亡，另一方就 +1 分
 *    - 先到 36 分立即获胜；时间到时分数高者获胜，同分平局
 *
 *  编译：Visual Studio 新建“空项目(C)”，添加本文件即可（字符集 Unicode）。
 *        命令行：cl /utf-8 /O2 main.c user32.lib gdi32.lib
 *
 *  ---------------------------------------------------------------------------
 *  【建议阅读顺序】整份代码是一个“每帧：先算、再画”的循环，可以按下面顺序看：
 *
 *    1. 基本参数 / 颜色 / 地图 / 类型       —— 数据长什么样
 *    2. 小工具（Blend / KeyDown / ...）      —— 画图和读键的底层积木
 *    3. 碰撞检测（TileIsWall / BoxHitsWall） —— 一切移动和反弹的地基
 *    4. 游戏逻辑（ControlTank / Fire / ...） —— 玩家操作怎么变成位移
 *    5. 子弹（BulletProbe / UpdateBullets）  —— 本程序最精巧的一段
 *    6. 绘制（Draw* / Render）               —— 状态怎么变成像素
 *    7. WinMain 主循环                       —— 把上面这些串起来的马达
 *
 *  【一句话架构】
 *    所有游戏状态都放在文件级全局变量里（g_tanks / g_bullets / g_map ...），
 *    没有对象系统，也不做动态内存分配；每帧 WinMain 调一次 UpdateGame(dt)
 *    改状态，再调一次 RenderFrame(hdc) 把状态画出来，两者互不调用。
 * ==========================================================================*/

#define WIN32_LEAN_AND_MEAN      /* 只包含最常用的 Win32 声明，加快编译 */
#include <winsock2.h>            /* 局域网 TCP：必须放在 windows.h 前 */
#include <ws2tcpip.h>            /* inet_pton / getaddrinfo */
#include <windows.h>             /* 窗口、消息、GDI 绘图 */
#include <math.h>                /* fabsf / floorf / cosf / sinf */
#include <stdio.h>               /* swprintf：把分数格式化成字符串 */
#include <stdlib.h>              /* rand：粒子随机方向 */
#include <stdint.h>              /* uint32_t：输入位掩码和网络协议 */
#include <string.h>              /* memset：一帧开始时清空数组 */
#include <time.h>                /* time：生成邀请码和会话号 */

/* ------------------------------- 基本参数 ------------------------------- */
/*  约定：
 *    - 坐标单位是“像素”，原点在场地左上角（不是窗口左上角）；
 *      真正画到屏幕上时还要再加 ARENA_X / ARENA_Y 的偏移。
 *    - 所有速度单位是“像素/秒”，不是“像素/帧”。乘 dt 之后才是位移，
 *      所以换刷新率或偶尔掉帧时，坦克和子弹的实际速度不会变。
 *    - 地图是 31 列 × 22 行的字符网格，每格 TILE 像素。            */
#define TILE          24                                  /* 每格像素 */
#define MAP_W         31                                  /* 地图列数 = 3*10+1 */
#define MAP_H         22                                  /* 地图行数 = 3*7+1  */
#define ARENA_X       12                                  /* 场地左边距 */
#define ARENA_Y       124                                 /* 场地上边距(留给 HUD：比分/计时/道具槽) */
#define WIN_W         (MAP_W * TILE + ARENA_X * 2)        /* 客户区宽度，自动推导 */
#define WIN_H         (ARENA_Y + MAP_H * TILE + 10)       /* 客户区高度，自动推导 */

#define TANK_HALF     9.0f                                /* 坦克半宽(碰撞盒) */
#define TANK_SPEED    128.0f                              /* 像素/秒 */
#define TANK_TURN     8.0f                                /* 转向角速度(弧度/秒)，约 460°/秒 */
#define BULLET_R      3.0f                                /* 子弹半径(碰撞用) */
#define BULLET_SPEED  300.0f                              /* 像素/秒，约 2.5 倍车速 */
#define MAX_BULLETS   3                                   /* 每人同时最多在场炮弹 */
#define MAX_HP        3                                   /* 每辆车 3 点血 */
#define FIRE_CD       0.34f                               /* 开火冷却(秒)，按住也不会连发 */
#define MAX_BOUNCE    5                                   /* 反弹次数上限，防止子弹无限乱窜 */
#define BULLET_LIFE   9.0f                                /* 子弹最长存活时间(秒) */
#define RESPAWN_TIME  1.2f                                /* 死亡后多久重生 */
#define SHIELD_TIME   1.5f                                /* 重生后的无敌时间，防蹲点 */
#define WIN_SCORE     36                                  /* 先到这么多分直接获胜 */
#define ROUND_TIME    240.0f                              /* 一整局时长(秒) = 4 分钟 */
#define MAX_BULLETS_ALL 64                                /* 子弹池大小(两人共用) */
#define MAX_SPARKS    96                                  /* 粒子池大小(环形复用) */

/* ---------------------------- 道具与特殊武器 ---------------------------- */
/*  四种彩色小球：
 *    红 = 折射激光炮   绿 = 追踪弹   黄 = 隐形地雷   蓝 = 可放置砖块
 *
 *  【持有规则】
 *    - 红 / 绿是“武器”，拾取即换装，一份道具给 3 次使用机会(ITEM_USES)。
 *      手里已经有一把时再拾另一个，旧的直接作废（不累加、不返还）。
 *    - 黄 / 蓝是“存货”，不占武器槽，也不被别的道具顶掉，
 *      但各最多攒 2 个(MAX_SLOT)，需要按键自己找地方放。
 *    - 不论哪种，只要阵亡一次就全部清零（复活后从空手开始）。           */
#define MAX_ITEMS     4                                   /* 场上同时最多几个道具 */
#define ITEM_R        8.0f                                /* 小球半径(拾取判定也用它) */
#define ITEM_LIFE     20.0f                               /* 道具存在时间，最后 3 秒闪烁 */
#define ITEM_SPAWN    6.0f                                /* 道具生成间隔基数(秒) */
#define ITEM_USES     3                                   /* 一个武器道具 = 3 次使用机会 */
#define MAX_SLOT      2                                   /* 地雷 / 砖块各最多存几个 */
#define ITEM_KINDS    4                                   /* 道具种类数：红绿黄蓝 */

#define MAX_LASER_PTS 10                                  /* 一条激光最多几个折点 */
#define LASER_RANGE   620.0f                              /* 激光总路程上限(含折射段) */
#define LASER_BOUNCE  3                                   /* 最多折射几次 */
#define LASER_LIFE    0.30f                               /* 光轨在屏幕上残留多久 */
#define LASER_DMG     2                                   /* 伤害：不分敌我，谁被扫到都掉这么多 */

#define MAX_MISSILES  4                                   /* 追踪弹池 */
#define MISSILE_AIM   1.4f                                /* 瞄准时间，就是留给敌人躲的时间 */
#define MISSILE_SPEED 250.0f                              /* 像素/秒 */
#define MISSILE_TURN  0.22f                               /* 转向平滑系数，越小越"笨"越容易躲 */
#define MISSILE_DMG   2                                   /* 命中伤害 */
#define MISSILE_RADIUS 30.0f                              /* 爆炸半径 */
#define MISSILE_LIFE  5.0f                                /* 飞行超时保护 */

#define MAX_MINES     10                                  /* 地雷池 */
#define MINE_ARM      0.8f                                /* 布雷后多久才生效 */
#define MINE_LIFE     45.0f                               /* 地雷存在时间 */
#define MINE_TRIGGER  14.0f                               /* 触发半径 */
#define MINE_BLAST    26.0f                               /* 爆炸伤害半径 */
#define MINE_DMG      2                                   /* 踩雷伤害 */
#define MINE_FRIENDLY 1                                   /* 1 = 自己踩到也炸（伤害不分敌我）
                                                           * 0 = 退回旧规则：只炸敌人 */

/* -------------------------------- 颜色 ---------------------------------- */
/*  全部用 RGB 宏写死；想换皮肤只改这一块。
 *  C_xxx_LO / C_xxx_HI 是同一色系的暗边和高光，用来伪造立体感。      */
#define C_BG        RGB(13, 15, 22)      /* 窗口最底层背景 */
#define C_FLOOR     RGB(25, 28, 39)      /* 场地地面 */
#define C_GRID      RGB(33, 37, 51)      /* 地面网格线 */
#define C_WALL      RGB(63, 71, 96)      /* 钢墙主色 */
#define C_WALL_HI   RGB(101, 111, 143)   /* 钢墙受光边 */
#define C_WALL_LO   RGB(37, 43, 61)      /* 钢墙背光边 */
#define C_P1        RGB(240, 88, 88)     /* 红方(玩家1) */
#define C_P2        RGB(88, 158, 255)    /* 蓝方(玩家2) */
#define C_TXT       RGB(226, 231, 243)   /* 正文文字 */
#define C_TXT_DIM   RGB(126, 136, 160)   /* 次要文字 */
#define C_BULLET    RGB(255, 216, 130)   /* 子弹 */
#define C_BRICK     RGB(70, 158, 96)     /* 砖墙主色(绿) */
#define C_BRICK_HI  RGB(112, 202, 136)   /* 砖墙受光边 */
#define C_BRICK_LO  RGB(40, 96, 58)      /* 砖缝 / 背光边 */
#define C_BRICK_CRK RGB(24, 54, 34)      /* 砖墙裂纹 */
#define C_PANEL     RGB(18, 20, 30)      /* 顶部 HUD 面板底色 */

/* 道具 / 特殊武器配色：红、绿、黄、蓝四种小球各代表一种道具 */
#define C_ITEM_LASER  RGB(240, 72, 72)   /* 红：折射激光炮 */
#define C_ITEM_HOMING RGB(72, 212, 120)  /* 绿：追踪弹 */
#define C_ITEM_MINE   RGB(248, 202, 66)  /* 黄：隐形地雷 */
#define C_ITEM_BRICK  RGB(66, 136, 250)  /* 蓝：可放置砖块 */
#define C_LASER_CORE  RGB(255, 236, 236) /* 激光亮芯 */

/* -------------------------------- 地图 ---------------------------------- */
/*  '#' = 钢墙(不可破坏)   'B' = 砖墙(4 点血，可打穿)   '.' = 地面
 *  出生点：P1 左下角房间，P2 右上角房间
 *
 *  【怎么长出来的】整张图可以看成 10×7 = 70 个 2×2 的“房间”，
 *  房间与房间之间隔一条 1 格厚的墙。先随机长出一棵生成树（保证任意两个
 *  房间之间都有路），再额外打通一部分隔墙形成环路（不然一路走到底太死），
 *  最后把剩下的一部分隔墙换成砖墙 —— 这些就是“软路”，打穿能抄近道。
 *
 *  【为什么通道永远是 2 格宽】打通隔墙时一定是把那条墙的 2 格一起凿掉，
 *  房间本身也是 2×2，所以整张图的最小通道就是 48px，比 18px 的车体宽一倍多，
 *  斜着转向、贴着墙拐弯都不会卡住。生成后做过校验：432 格地面全部连通，
 *  且每一格都属于某个 2×2 的空地方块（没有 1 格宽的窄缝）。
 *
 *  注意：这张表是“模板”，只读。开局时会被 LoadMap() 拷贝到 g_map，
 *  之后所有读写都针对 g_map —— 因为砖墙被打穿后要把那格改成 '.',
 *  而放在这里的是字符串常量，是改不了的。                            */
static const char *g_mapInit[MAP_H] = {
    "###############################",
    "#.....#.....#.....B...........#",
    "#.....#.....#.....B...........#",
    "####..#..#..####..#..#BB#..#..#",
    "#........B.....#.....B.....B..#",
    "#........B.....#.....B.....B..#",
    "#..#BB#######..#..####..#BB#..#",
    "#........B.....B..#...........#",
    "#........B.....B..#...........#",
    "#..#BB#..#..#######..#BB#..####",
    "#..B.....B.................B..#",
    "#..B.....B.................B..#",
    "#BB#..#BB####..#..####..#..#..#",
    "#.....#.....#.....#...........#",
    "#.....#.....#.....#...........#",
    "#..#BB#..#..#######..#######..#",
    "#..#.....B.....#.....#........#",
    "#..#.....B.....#.....#........#",
    "#..#..#######..#..####..#..#..#",
    "#...........#...........B.....#",
    "#...........#...........B.....#",
    "###############################",
};

/* 运行时地图（砖墙打穿后要改写，所以不能直接用字符串常量）
 * 每行多留 1 字节放 '\0'，这样它仍然可以当普通字符串使用。        */
static char g_map[MAP_H][MAP_W + 1];
static int  g_brickHp[MAP_H][MAP_W];      /* 砖墙剩余血量，0 表示不是砖墙 */

#define BRICK_HP 4                        /* 砖墙血量：挨 4 发变成地面 */

/* 出生点：用“第几列 / 第几行”表示（允许小数，方便取房间正中），
 * 并把地图尺寸一起算进来，这样改 MAP_W / MAP_H 时出生点会自动走到新角落。
 * 左下角房间占 1~2 列，中心在 1.5 列；右上角房间占 MAP_W-3~MAP_W-2 列，
 * 中心在 MAP_W-2.5 列。                                             */
#define P1_COL 1.5f
#define P1_ROW (MAP_H - 2.5f)
#define P2_COL (MAP_W - 2.5f)
#define P2_ROW 1.5f

/* -------------------------------- 类型 ---------------------------------- */
typedef struct { float x, y; } Vec2;      /* 二维向量：位置和速度都用它 */

/* 武器类型：普通弹无限，另两种靠拾取道具获得，打光自动退回普通弹 */
#define WPN_NORMAL 0
#define WPN_LASER  1
#define WPN_HOMING 2

/* 一辆坦克的全部状态。没有任何“方法”，全靠后面的函数去改它 */
typedef struct {
    Vec2       pos;        /* 场地坐标(像素)，左上角为原点 */
    Vec2       spawn;      /* 出生点，死亡重生时回到这里 */
    float      spawnAngle; /* 出生时的朝向，重生时一并复位 */
    float      angle;      /* 朝向角(弧度)：0=朝上，正方向为顺时针。
                            * 方向向量 = (sin(angle), -cos(angle))，
                            * 用连续角度而不是 4 个整数方向，才能斜着走。 */
    int        hp;
    int        score;
    float      cd;         /* 开火冷却剩余(秒)，>0 时不能开火 */
    float      shield;     /* 重生无敌剩余(秒)，>0 时免伤 */
    int        alive;
    float      respawn;    /* 还差多久重生；alive=0 时倒计时 */
    int        weapon;     /* 当前武器 WPN_*（只有一把，新道具会顶掉旧的） */
    int        ammo;       /* 特殊武器剩余发数，拾一次给 3 发，打光回普通弹 */
    int        mines;      /* 存着的地雷个数 0~2：按放置键在脚下埋一颗 */
    int        bricks;     /* 存着的砖块个数 0~2：按放置键在车头前一格砌一块 */
    COLORREF   color;
} Tank;

/* 一颗子弹。池子里 64 个，active 标记这一格是否在使用 */
typedef struct {
    Vec2  pos, vel;   /* vel 是“像素/秒”的速度矢量，反弹就是给它取反某个分量 */
    int   active;
    int   owner;      /* 0 = P1 打的，1 = P2 打的；只用来算分，不再区分敌我 */
    int   bounces;    /* 已经反弹几次，超过 MAX_BOUNCE 就消失 */
    float life;       /* 剩余寿命(秒) */
    float grace;      /* 出膛保护：这段时间内不会打到自己 */
} Bullet;

/* 火花粒子：命中、反弹、砖块碎裂时溅出来的小方块 */
typedef struct {
    Vec2  pos, vel;
    float life, maxLife;   /* 两个都要：life 递减，两者之比决定大小和透明度 */
    COLORREF color;
} Spark;

/* 折射激光的光轨。激光是"瞬间命中"的，这个结构只用来把折线画出来残影 */
typedef struct {
    int   active;
    int   owner;                       /* 谁打的，决定颜色 */
    float life;                        /* 残留时间，用来淡出 */
    int   npts;                        /* 折点个数 */
    Vec2  pts[MAX_LASER_PTS];          /* 折线顶点：起点 → 每个反射点 → 终点 */
} Laser;

/* 追踪弹。分两个阶段：先瞄准（给敌人躲避时间），再穿墙飞行 */
typedef struct {
    int   active;
    int   owner;
    int   phase;      /* 0 = 瞄准中   1 = 飞行中 */
    float timer;      /* 瞄准剩余时间 */
    float life;       /* 飞行超时保护 */
    Vec2  origin;     /* 发射时炮口位置 */
    Vec2  pos, vel;
    Vec2  lockPos;    /* 锁定目标点：发射瞬间记下敌人位置，之后不再改动 */
} Missile;

/* 地雷。布下瞬间闪一下，之后完全不可见 */
typedef struct {
    int   active;
    int   owner;      /* 埋雷的人：只影响爆炸特效颜色，谁踩上去都会炸 */
    Vec2  pos;
    float arm;        /* 布防倒计时，>0 时不会被踩爆 */
    float life;       /* 剩余存在时间 */
    float blink;      /* 布雷提示动画剩余时间 */
    int   inside;     /* 上一帧哪几辆车在触发圈里(位掩码)：只对“刚走进来”的车生效，
                       * 这样刚埋完还站在雷上的自己不会立刻被自己炸掉 */
} Mine;

/* 道具小球。type 决定颜色和效果 */
typedef struct {
    int   active;
    int   type;       /* 0 = 红(激光)  1 = 绿(追踪)  2 = 黄(地雷)  3 = 蓝(砖块) */
    Vec2  pos;
    float life;
    float phase;      /* 呼吸动画相位 */
} Item;

/* 一次固定逻辑 tick 的玩家输入。held 表示持续按住，pressed 只包含新按下边沿。 */
#define IN_UP       0x01u
#define IN_DOWN     0x02u
#define IN_LEFT     0x04u
#define IN_RIGHT    0x08u
#define IN_FIRE     0x10u
#define IN_MINE     0x20u
#define IN_BRICK    0x40u
#define IN_ACTIONS  (IN_FIRE | IN_MINE | IN_BRICK)

typedef struct {
    uint32_t held;
    uint32_t pressed;
} PlayerInput;

#define SIM_DT (1.0f / 60.0f)

/* ------------------------------- 全局状态 ------------------------------- */
/*  整个游戏没有动态内存，所有东西都是固定大小的静态数组。
 *  好处：不会泄漏、不会卡顿；代价：有上限（最多 64 发子弹、96 个粒子）。 */
static Tank   g_tanks[2];
static Bullet g_bullets[MAX_BULLETS_ALL];
static Spark  g_sparks[MAX_SPARKS];
static int    g_sparkIdx = 0;   /* 环形缓冲区游标：新粒子覆盖最老的 */

/* 道具相关：两把激光（一人一条）、追踪弹池、地雷池、道具池 */
static Laser   g_lasers[2];
static Missile g_missiles[MAX_MISSILES];
static Mine    g_mines[MAX_MINES];
static Item    g_items[MAX_ITEMS];
static float   g_itemTimer = 0.0f;   /* 还有多久冒下一个道具 */

static int    g_state = 0;      /* 0 进行中  1 结束(显示结果面板) */
static int    g_winner = -1;    /* 0 红方  1 蓝方  -1 平局 */
static int    g_endReason = 0;  /* 0 = 有人打到 36 分   1 = 时间到比分数 */
static float  g_timeLeft = ROUND_TIME;   /* 本局剩余时间(秒) */
static float  g_flash[2];       /* 被击中的白闪剩余时间 */

/* 上一逻辑 tick 的完整键位，用来生成 pressed 按下边沿。 */
static uint32_t g_prevKeys[2];
static int      g_prevR = 0;

#ifdef AUTO_DEMO
static float  g_autoT = 0.0f;   /* 自动演示计时（编译时加 /DAUTO_DEMO 才有） */
#endif

/* GDI 资源：5 档字体 + 双缓冲用的离屏画布 */
static HFONT  g_fTitle, g_fBig, g_fMid, g_fSmall, g_fHuge;
static HDC    g_memDC = NULL;
static HBITMAP g_memBmp = NULL;

/* =============================== 小工具 ================================= */

/* 按比例 t(0~1) 混合两个颜色。等价于半透明叠加，
 * 但不需要 alpha 通道，所有高光 / 暗边 / 淡出效果都是它做的。        */
static COLORREF Blend(COLORREF a, COLORREF b, float t)
{
    int r = (int)(GetRValue(a) * (1 - t) + GetRValue(b) * t);
    int g = (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t);
    int bl = (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (bl < 0) bl = 0; if (bl > 255) bl = 255;
    return RGB(r, g, bl);
}

/* 查询某个虚拟键现在是否按住。
 * 用 GetAsyncKeyState 而不是处理 WM_KEYDOWN，是因为它天然反映“当前状态”，
 * 不用管按键重复消息，也不怕窗口刚获得焦点时漏消息。                */
static int KeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;   /* 最高位=1 表示正按下 */
}

/* P1 键位同时也是两台电脑联机时各自使用的键位。 */
static uint32_t ReadP1Keys(void)
{
    uint32_t k = 0;
    if (KeyDown('W')) k |= IN_UP;
    if (KeyDown('S')) k |= IN_DOWN;
    if (KeyDown('A')) k |= IN_LEFT;
    if (KeyDown('D')) k |= IN_RIGHT;
    if (KeyDown('J')) k |= IN_FIRE;
    if (KeyDown(VK_SPACE)) k |= IN_MINE;
    if (KeyDown('K')) k |= IN_BRICK;
    return k;
}

/* 本地同键盘双人的 P2 键位保持不变。 */
static uint32_t ReadP2Keys(void)
{
    uint32_t k = 0;
    if (KeyDown(VK_UP)) k |= IN_UP;
    if (KeyDown(VK_DOWN)) k |= IN_DOWN;
    if (KeyDown(VK_LEFT)) k |= IN_LEFT;
    if (KeyDown(VK_RIGHT)) k |= IN_RIGHT;
    if (KeyDown(VK_RETURN)) k |= IN_FIRE;
    if (KeyDown(VK_NUMPAD1) || KeyDown(VK_END)) k |= IN_MINE;
    if (KeyDown(VK_NUMPAD0) || KeyDown(VK_INSERT)) k |= IN_BRICK;
    return k;
}

static PlayerInput MakeInput(int idx, uint32_t keys)
{
    PlayerInput in;
    in.held = keys;
    in.pressed = keys & ~g_prevKeys[idx];
    g_prevKeys[idx] = keys;
    return in;
}

/* 画实心矩形。GDI 的规矩：Create 出来的对象用完必须 Delete，否则泄漏 */
static void FillBox(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r;
    HBRUSH br;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    br = CreateSolidBrush(c);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

/* 画一行文字。
 * 关键三步：SelectObject 选字体（并保存旧字体）、SetBkMode(TRANSPARENT)
 * 去掉文字背后的底色方块、画完把旧字体选回去。                      */
static void DrawStr(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h,
                    const wchar_t *s, UINT fmt)
{
    HFONT old;
    RECT r;
    old = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, old);
}

/* =============================== 角度工具 =============================== */
/*  坦克朝向是一个连续的角度(弧度)，不是 4 个整数方向。
 *  约定：0 = 朝上，角度增大 = 屏幕上顺时针（因为屏幕 y 轴向下）。
 *  方向向量就是 (sin a, -cos a)：a=0 时 (0,-1) 朝上，a=π/2 时 (1,0) 朝右。 */

#define PI_F 3.14159265358979f

/* 角度 → 单位方向向量 */
static void AngleDir(float a, float *dx, float *dy)
{
    *dx = sinf(a);
    *dy = -cosf(a);
}

/* 从 a 转到 b 的最短角度差，结果落在 (-π, π]。
 * 不折这个弯的话，从 -179° 转到 +179° 会绕一整圈（358°）而不是 2°。 */
static float AngleDiff(float a, float b)
{
    float d = b - a;
    while (d >  PI_F) d -= 2.0f * PI_F;
    while (d < -PI_F) d += 2.0f * PI_F;
    return d;
}

/* ============================== 碰撞检测 ================================ */
/*  这一段是整个游戏的地基：移动、子弹反弹、砖墙破坏全都建立在
 *  “某个方块压到了哪几格墙” 这一个问题上。                            */

/* 载入 / 重置地图，恢复所有砖墙的满血状态。
 * ResetGame 和开局都会调用它，所以按 R 重开时砖墙会全部复原。      */
static void LoadMap(void)
{
    int x, y;
    for (y = 0; y < MAP_H; y++) {
        for (x = 0; x < MAP_W; x++) {
            g_map[y][x] = g_mapInit[y][x];
            g_brickHp[y][x] = (g_map[y][x] == 'B') ? BRICK_HP : 0;
        }
        g_map[y][MAP_W] = '\0';
    }
}

/* 坦克 / 子弹的阻挡判定：钢墙和砖墙都挡，砖墙被打穿后自然变成地面。
 * 越界一律当成墙返回 1 —— 这就是“坦克出不了地图”的全部实现，
 * 不需要另外写边界检查代码。                                        */
static int TileIsWall(int cx, int cy)
{
    char c;
    if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) return 1;   /* 越界视作墙 */
    c = g_map[cy][cx];
    return c == '#' || c == 'B';
}

/* 以 (x,y) 为中心、半边长 half 的方块是否压到墙（或出界）。
 *
 * 做法：把方块的四条边换算成“格子坐标范围”，再逐格查询。
 *   - 除以 TILE 再 floor，就是“这个像素属于第几格”。
 *   - 右 / 下边减 0.001 是为了避免浮点误差：方块边界正好压在格子分界线
 *     上时，不应该把右边那一格也算进来，否则贴墙时会莫名卡住。
 * 坦克用 TANK_HALF=9 调它，子弹用 BULLET_R=3 调它，逻辑完全共用。  */
static int BoxHitsWall(float x, float y, float half)
{
    int x0 = (int)floorf((x - half) / TILE);
    int x1 = (int)floorf((x + half - 0.001f) / TILE);
    int y0 = (int)floorf((y - half) / TILE);
    int y1 = (int)floorf((y + half - 0.001f) / TILE);
    int cx, cy;
    if (x0 < 0 || y0 < 0 || x1 >= MAP_W || y1 >= MAP_H) return 1;
    for (cy = y0; cy <= y1; cy++)
        for (cx = x0; cx <= x1; cx++)
            if (TileIsWall(cx, cy)) return 1;
    return 0;
}

/* 与另一辆坦克是否重叠（AABB 矩形判定）。
 * 距离小于 2*TANK_HALF 就算撞上；这里故意少算 0.5 像素留一点余量，
 * 避免两车贴住之后互相判定为重叠而永远动不了。                      */
static int HitsOtherTank(float x, float y, int me)
{
    int other = 1 - me;
    Tank *o = &g_tanks[other];
    if (!o->alive) return 0;      /* 对方已经爆了，可以开过去 */
    return fabsf(x - o->pos.x) < (TANK_HALF * 2 - 0.5f) &&
           fabsf(y - o->pos.y) < (TANK_HALF * 2 - 0.5f);
}

/* ============================== 粒子效果 ================================ */

/* 在 (x,y) 放一个火花。
 * 环形缓冲区：g_sparkIdx 一路往后走，走到头回 0，直接覆盖最老的粒子。
 * 所以数组永远不用扩容，粒子满了只是“最旧的火花提前消失”。          */
static void AddSpark(float x, float y, COLORREF c)
{
    Spark *s = &g_sparks[g_sparkIdx];
    float a = (float)(rand() % 628) / 100.0f;         /* 0~6.28 弧度 = 一整圈 */
    float sp = 30.0f + (float)(rand() % 90);          /* 随机初速 */
    g_sparkIdx = (g_sparkIdx + 1) % MAX_SPARKS;
    s->pos.x = x; s->pos.y = y;
    s->vel.x = cosf(a) * sp;
    s->vel.y = sinf(a) * sp;
    s->life = s->maxLife = 0.28f + (float)(rand() % 20) / 100.0f;
    s->color = c;
}

/* 粒子物理：速度推进 + 每帧衰减 10%（像空气阻力），寿命到期就不画 */
static void UpdateSparks(float dt)
{
    int i;
    for (i = 0; i < MAX_SPARKS; i++) {
        Spark *s = &g_sparks[i];
        if (s->life <= 0) continue;
        s->life -= dt;
        s->pos.x += s->vel.x * dt;
        s->pos.y += s->vel.y * dt;
        s->vel.x *= 0.90f;
        s->vel.y *= 0.90f;
    }
}

/* 砖墙受损：每中一枪掉 1 点血，血量归零后变成可通行的地面。
 * 注意最后那句 g_map[cy][cx] = '.' 就是“打穿”的全部实现——
 * 格子一旦变成地面，坦克能过、子弹能飞，不需要任何额外标记。      */
static void DamageBrick(int cx, int cy, float hx, float hy)
{
    int i;
    if (g_map[cy][cx] != 'B') return;

    g_brickHp[cy][cx]--;
    for (i = 0; i < 5; i++) AddSpark(hx, hy, C_BRICK_HI);   /* 命中点溅碎屑 */

    if (g_brickHp[cy][cx] <= 0) {
        g_map[cy][cx] = '.';
        g_brickHp[cy][cx] = 0;
        for (i = 0; i < 16; i++)                            /* 整块碎裂，多溅几颗 */
            AddSpark(cx * TILE + TILE * 0.5f, cy * TILE + TILE * 0.5f, C_BRICK_HI);
    }
}

/* ============================== 游戏逻辑 ================================ */

/* 初始化 / 复位一辆坦克。出生点也存一份到 spawn，供重生使用。
 * angle 是初始朝向角：0 = 朝上，PI_F = 朝下。                        */
static void InitTank(Tank *t, float col, float row, float angle, COLORREF color)
{
    t->spawn.x = (col + 0.5f) * TILE;        /* 取格子中心，不是格子左上角 */
    t->spawn.y = (row + 0.5f) * TILE;
    t->pos = t->spawn;
    t->angle = angle;
    t->spawnAngle = angle;
    t->hp = MAX_HP;
    t->score = 0;
    t->cd = 0.0f;
    t->shield = SHIELD_TIME;                 /* 开局也带 1.5 秒护盾 */
    t->alive = 1;
    t->respawn = 0.0f;
    t->weapon = WPN_NORMAL;                  /* 复活后回到普通弹 */
    t->ammo = 0;
    t->mines = 0;                            /* 开局手里什么都没有 */
    t->bricks = 0;
    t->color = color;
}

/* 重开一局：清空所有临时对象，地图恢复满血，两辆车回出生点，计时归位 */
static void ResetGame(void)
{
    memset(g_bullets, 0, sizeof(g_bullets));
    memset(g_sparks, 0, sizeof(g_sparks));
    memset(g_lasers, 0, sizeof(g_lasers));
    memset(g_missiles, 0, sizeof(g_missiles));
    memset(g_mines, 0, sizeof(g_mines));
    memset(g_items, 0, sizeof(g_items));
    g_sparkIdx = 0;
    g_itemTimer = 2.0f;              /* 开局 2 秒后掉第一个道具 */
    g_state = 0;
    g_winner = -1;
    g_endReason = 0;
    g_timeLeft = ROUND_TIME;         /* 4 分钟重新开始 */
    g_flash[0] = g_flash[1] = 0.0f;
    g_prevKeys[0] = g_prevKeys[1] = 0;
    g_prevR = 0;
    LoadMap();                       /* 砖墙全部恢复（包括上一局玩家砌的） */
    InitTank(&g_tanks[0], P1_COL, P1_ROW, 0.0f, C_P1);
    InitTank(&g_tanks[1], P2_COL, P2_ROW, PI_F, C_P2);
}

/* 结束本局。reason: 0 = 有人打到 36 分，1 = 时间到比分数。
 * 两种情况的胜负判定其实一样：分高者胜，同分算平局。              */
static void EndRound(int reason)
{
    g_state = 1;
    g_endReason = reason;
    if (g_tanks[0].score > g_tanks[1].score)      g_winner = 0;
    else if (g_tanks[1].score > g_tanks[0].score) g_winner = 1;
    else                                          g_winner = -1;
}

/* 统一伤害入口：子弹、激光、追踪弹、地雷全都走这里。
 *
 * 【计分规则】不管是谁把谁打死的 —— 包括自己把自己打死、踩到自己的雷、
 * 被自己弹回来的子弹命中 —— 只要有一方阵亡，另一方就 +1 分。
 * 所以这里根本不看“凶手是谁”，只看到底是谁死了：死者的对手得分。
 *
 * 【阵亡清空道具】死亡时手里的东西全部作废：特殊武器退回普通弹、
 * 存着的地雷和砖块一起丢光。这样“攒着不用”是有代价的 ——
 * 想留着下一波用，就得先活下来。                                  */
static void HurtTank(int idx, int dmg, float hx, float hy)
{
    Tank *t = &g_tanks[idx];
    int k;

    if (!t->alive || t->shield > 0.0f) return;   /* 护盾期内完全免伤 */

    t->hp -= dmg;
    g_flash[idx] = 0.12f;
    for (k = 0; k < 8; k++) AddSpark(hx, hy, t->color);

    if (t->hp <= 0) {
        t->hp = 0;
        t->alive = 0;
        t->respawn = RESPAWN_TIME;
        for (k = 0; k < 20; k++) AddSpark(hx, hy, t->color);

        /* 道具全部作废：武器回普通弹，存货清零 */
        t->weapon = WPN_NORMAL;
        t->ammo   = 0;
        t->mines  = 0;
        t->bricks = 0;

        /* 阵亡即送分：得分的是“另一方”，与谁开的火无关 */
        g_tanks[1 - idx].score += 1;
        if (g_tanks[1 - idx].score >= WIN_SCORE) EndRound(0);
    }
}

/* 激光探点：0 = 空   1 = 砖墙(格子号写回 pcx/pcy)   2 = 钢墙或边界 */
static int LaserAt(float x, float y, int *pcx, int *pcy)
{
    int cx = (int)floorf(x / TILE);
    int cy = (int)floorf(y / TILE);

    if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) return 2;
    if (g_map[cy][cx] == 'B') { *pcx = cx; *pcy = cy; return 1; }
    if (g_map[cy][cx] == '#') return 2;
    return 0;
}

/* 激光是否扫过某辆坦克（光束很细，用半个车体宽判定就够） */
static int LaserHitsTank(int idx, Vec2 p)
{
    Tank *t = &g_tanks[idx];
    if (!t->alive) return 0;
    return fabsf(p.x - t->pos.x) <= TANK_HALF + 1.0f &&
           fabsf(p.y - t->pos.y) <= TANK_HALF + 1.0f;
}

/* 折射激光炮：从炮口发出一束光，一小步一小步往前推。
 *   - 撞钢墙：按分量反射（和子弹完全一样的规则），所以斜射也能正确折回
 *   - 撞砖墙：光被吸收，砖掉 1 点血，光路到此为止
 *   - 总路程不超过 LASER_RANGE，折射不超过 LASER_BOUNCE 次
 * 因为是一步一步推进，每一小步都顺便检查有没有扫到人。
 *
 * 注意光的方向来自 t->angle，可能是任意角度（不只是上下左右）：
 * 每一步把 X、Y 两个分量分开推进、分开判断撞墙，谁被挡就翻谁，
 * 这正是镜面反射的定义，斜射的效果自然就对了。                    */
static void CastLaser(Tank *t, int owner)
{
    Laser *L = &g_lasers[owner];
    Vec2   p, d;
    float  left = LASER_RANGE, traveled = 0.0f;
    int    bounces = 0, npts = 0, cx = 0, cy = 0;
    int    hit[2] = { 0, 0 };

    AngleDir(t->angle, &d.x, &d.y);
    p.x = t->pos.x + d.x * (TANK_HALF + 7.0f);   /* 从炮口出发，而不是车体中心 */
    p.y = t->pos.y + d.y * (TANK_HALF + 7.0f);

    /* 车头顶着墙时炮口可能在墙体里，往回收几步直到回到空地上 */
    {
        int k = 0;
        while (k++ < 12 && LaserAt(p.x, p.y, &cx, &cy) != 0) {
            p.x -= d.x;
            p.y -= d.y;
        }
    }

    L->active = 1;
    L->owner  = owner;
    L->life   = LASER_LIFE;
    L->pts[npts++] = p;

    while (left > 0.0f) {
        const float step = 3.0f;          /* 每次推进 3px，够细，不会穿过 1 格墙 */
        int bounced = 0;

        /* ---- X 分量 ---- */
        if (fabsf(d.x) > 1e-4f) {
            float nx = p.x + d.x * step;
            int h = LaserAt(nx, p.y, &cx, &cy);
            if (h == 1) {                 /* 砖墙挡住：吸收光，砖掉血 */
                DamageBrick(cx, cy, nx, p.y);
                break;
            }
            if (h == 2) { d.x = -d.x; bounced = 1; }   /* 竖直面：只翻 x */
            else        { p.x = nx; }
        }
        /* ---- Y 分量 ---- */
        if (fabsf(d.y) > 1e-4f) {
            float ny = p.y + d.y * step;
            int h = LaserAt(p.x, ny, &cx, &cy);
            if (h == 1) {
                DamageBrick(cx, cy, p.x, ny);
                break;
            }
            if (h == 2) { d.y = -d.y; bounced = 1; }   /* 水平面：只翻 y */
            else        { p.y = ny; }
        }

        if (bounced) {                    /* 记一个折点，并计入折射次数 */
            bounces++;
            if (npts < MAX_LASER_PTS - 1) L->pts[npts++] = p;
            if (bounces > LASER_BOUNCE) break;
        }

        left     -= step;
        traveled += step;

        /* 命中判定：一次激光对同一辆坦克只结算一次。
         * 自己要被扫到必须等光走出去一段（防止出膛就自伤）。
         * 伤害不分敌我 —— 折回来的光扫到自己照样掉血。              */
        if (!hit[0] && !(owner == 0 && traveled <= 30.0f) && LaserHitsTank(0, p)) {
            hit[0] = 1;
            HurtTank(0, LASER_DMG, p.x, p.y);
        }
        if (!hit[1] && !(owner == 1 && traveled <= 30.0f) && LaserHitsTank(1, p)) {
            hit[1] = 1;
            HurtTank(1, LASER_DMG, p.x, p.y);
        }
    }

    if (npts < MAX_LASER_PTS) L->pts[npts++] = p;   /* 收尾点 */
    L->npts = npts;
}

/* 这个玩家是否已经有一发追踪弹在瞄准/飞行中（同一时间只允许一发） */
static int MissileBusy(int owner)
{
    int i;
    for (i = 0; i < MAX_MISSILES; i++)
        if (g_missiles[i].active && g_missiles[i].owner == owner) return 1;
    return 0;
}

/* 发射追踪弹：现在只是"开始瞄准"，真正飞出去要等 MISSILE_AIM 秒。
 * 瞄准期间准星实时跟着敌人，敌人看到就有时间跑。                  */
static void FireMissile(Tank *t, int owner)
{
    Tank *tg = &g_tanks[1 - owner];
    Missile *m = NULL;
    float dx, dy;
    int i;

    for (i = 0; i < MAX_MISSILES; i++)
        if (!g_missiles[i].active) { m = &g_missiles[i]; break; }
    if (!m) return;

    AngleDir(t->angle, &dx, &dy);
    m->active = 1;
    m->owner  = owner;
    m->phase  = 0;
    m->timer  = MISSILE_AIM;
    m->life   = MISSILE_LIFE;
    m->origin.x = t->pos.x + dx * (TANK_HALF + 2.0f);
    m->origin.y = t->pos.y + dy * (TANK_HALF + 2.0f);
    m->pos = m->origin;
    m->vel.x = m->vel.y = 0.0f;
    /* 敌人死了就瞄他的出生点，避免锁定失败 */
    m->lockPos = tg->alive ? tg->pos : tg->spawn;

    AddSpark(m->origin.x, m->origin.y, C_ITEM_HOMING);
}

/* 在脚下埋一颗地雷。
 *
 * 【谁来放、放在哪】地雷不再“拾取即埋”，而是攒在手里（最多 2 个），
 * 由玩家自己把车开到想要的位置，再按放置键 —— 所以是玩家决定位置。
 * 同一处不能叠两颗（10px 内算同一处），避免白扔。
 *
 * 刚埋下的 0.8 秒是“布防期”：不会炸，并且会亮一下提示位置（之后隐形）。
 * 布防期结束时如果自己还站在上面，会先把“已在圈内”记下来，
 * 这样不会一埋就把自己炸了；但走出去再走回来，照样会炸自己
 * —— 伤害不分敌我（MINE_FRIENDLY）。                              */
static void PlaceMine(Tank *t, int owner)
{
    int i;

    if (t->mines <= 0) return;                    /* 手里没雷 */
    if (!t->alive) return;

    for (i = 0; i < MAX_MINES; i++) {             /* 别在同一个位置叠雷 */
        if (g_mines[i].active &&
            fabsf(g_mines[i].pos.x - t->pos.x) < 10.0f &&
            fabsf(g_mines[i].pos.y - t->pos.y) < 10.0f) return;
    }

    for (i = 0; i < MAX_MINES; i++) {
        if (!g_mines[i].active) {
            Mine *m = &g_mines[i];
            m->active = 1;
            m->owner  = owner;
            m->pos    = t->pos;
            m->arm    = MINE_ARM;
            m->life   = MINE_LIFE;
            m->blink  = 0.9f;      /* 只亮 0.9 秒，之后彻底隐形 */
            m->inside = 0;
            t->mines--;            /* 消耗一个存货 */
            AddSpark(t->pos.x, t->pos.y, C_ITEM_MINE);
            return;
        }
    }
}

/* 算出“砖块会砌在哪一格”：车体所在格，沿车头的主方向前推一格。
 *
 * 为什么不砌在脚下？砖块是实心墙，砌在脚下会把自己的车整个封进墙里，
 * 之后任何方向都判为撞墙，等于把自己钉死。所以固定砌在前方一格 ——
 * 玩家想堵哪条路，就把车头对着那条路按放置键。
 * 斜着开的时候取“更大的那个分量”作为主方向，保证只推一格。        */
static int BrickTargetCell(const Tank *t, int *pcx, int *pcy)
{
    float dx, dy;
    int cx = (int)floorf(t->pos.x / TILE);
    int cy = (int)floorf(t->pos.y / TILE);

    AngleDir(t->angle, &dx, &dy);
    if (fabsf(dx) >= fabsf(dy)) cx += (dx >= 0.0f) ? 1 : -1;
    else                        cy += (dy >= 0.0f) ? 1 : -1;

    if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) return 0;
    *pcx = cx;
    *pcy = cy;
    return 1;
}

/* 这一格能不能砌砖：必须是空地，而且不能压到“对方”那辆车。
 * 对方那辆车我们无权挪动，所以只能拒绝；自己压到没关系 ——
 * 砌完会把自己轻轻顶出去（见 PushOutOfBrick）。                     */
static int CellFreeForBrick(int cx, int cy, int me)
{
    float x0, y0;
    int j;

    if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) return 0;
    if (g_map[cy][cx] != '.') return 0;

    x0 = (float)(cx * TILE);
    y0 = (float)(cy * TILE);
    for (j = 0; j < 2; j++) {
        Tank *t = &g_tanks[j];
        if (j == me || !t->alive) continue;
        if (t->pos.x + TANK_HALF > x0 && t->pos.x - TANK_HALF < x0 + TILE &&
            t->pos.y + TANK_HALF > y0 && t->pos.y - TANK_HALF < y0 + TILE)
            return 0;
    }
    return 1;
}

/* 砖砌好了，但车头可能正压在新砖上（车宽 18、格宽 24，车几乎总在跨界）。
 * 这时如果不管，车会卡死在墙里 —— 因为任何方向的新位置都还压着那格墙。
 * 所以沿“背离新砖”的方向把车挪出去，最多 12px：
 *   车中心在第 k 格、砖在第 k+1 格时，只要退回 9px 以内就能完全脱离，
 *   而第 k 格本身是空的，所以一定有解。
 * 万一后面被墙或对方顶死退不出去，返回 0，调用方会把砖撤掉。        */
static int PushOutOfBrick(Tank *t, int me, int cx, int cy)
{
    float bcx = (float)(cx * TILE) + TILE * 0.5f;
    float bcy = (float)(cy * TILE) + TILE * 0.5f;
    float dx = t->pos.x - bcx;
    float dy = t->pos.y - bcy;
    float sx = (dx >= 0.0f) ? 1.0f : -1.0f;
    float sy = (dy >= 0.0f) ? 1.0f : -1.0f;
    int i;

    for (i = 1; i <= 12; i++) {
        float nx = t->pos.x, ny = t->pos.y;
        if (fabsf(dx) >= fabsf(dy)) nx += sx * (float)i;   /* 只沿主方向退 */
        else                        ny += sy * (float)i;

        if (!BoxHitsWall(nx, ny, TANK_HALF) && !HitsOtherTank(nx, ny, me)) {
            t->pos.x = nx;
            t->pos.y = ny;
            return 1;
        }
    }
    return 0;
}

/* 在车头前方一格砌一块砖：和地图上的绿色砖墙完全一样（4 点血、能打穿）。
 * 它写进的就是 g_map 本身，所以之后子弹、激光、坦克移动全都不用特殊处理
 * —— DamageBrick 打到 0 血时会把这一格改回 '.'，路就又通了。        */
static void PlaceBrick(Tank *t, int owner)
{
    int cx, cy, k;

    if (t->bricks <= 0 || !t->alive) return;
    if (!BrickTargetCell(t, &cx, &cy)) return;
    if (!CellFreeForBrick(cx, cy, owner)) return;

    g_map[cy][cx] = 'B';
    g_brickHp[cy][cx] = BRICK_HP;

    if (!PushOutOfBrick(t, owner, cx, cy)) {     /* 退不出去就撤销，不扣存货 */
        g_map[cy][cx] = '.';
        g_brickHp[cy][cx] = 0;
        return;
    }

    t->bricks--;

    for (k = 0; k < 10; k++)
        AddSpark(cx * TILE + TILE * 0.5f, cy * TILE + TILE * 0.5f, C_ITEM_BRICK);
}

/* 开火。按当前武器分三路：
 *   折射激光 —— 瞬间一条光路，消耗 1 发弹药
 *   追踪弹   —— 先瞄准 1.4 秒再发射，同一时间只允许一发在流程中
 *   普通弹   —— 原来的逻辑：冷却好、场上不超过 3 发
 * 特殊武器弹药打光后自动退回普通弹。                              */
static void Fire(Tank *t, int owner)
{
    float dx, dy;                    /* 炮口方向：由朝向角算出来，可能是任意角度 */
    int i, count = 0;
    Bullet *b = NULL;

    if (t->cd > 0.0f || !t->alive) return;

    AngleDir(t->angle, &dx, &dy);

    /* ---- 折射激光炮 ---- */
    if (t->weapon == WPN_LASER && t->ammo > 0) {
        CastLaser(t, owner);
        t->ammo--;
        t->cd = 0.85f;
        if (t->ammo <= 0) { t->weapon = WPN_NORMAL; t->ammo = 0; }
        return;
    }

    /* ---- 追踪弹 ---- */
    if (t->weapon == WPN_HOMING && t->ammo > 0) {
        if (MissileBusy(owner)) return;    /* 上一发还没落地，先不能发 */
        FireMissile(t, owner);
        t->ammo--;
        t->cd = 1.5f;
        if (t->ammo <= 0) { t->weapon = WPN_NORMAL; t->ammo = 0; }
        return;
    }

    /* ---- 普通弹 ---- */
    /* 关卡一：本人场上已有几发 */
    for (i = 0; i < MAX_BULLETS_ALL; i++)
        if (g_bullets[i].active && g_bullets[i].owner == owner) count++;
    if (count >= MAX_BULLETS) return;

    /* 关卡二：找一个空槽位 */
    for (i = 0; i < MAX_BULLETS_ALL; i++)
        if (!g_bullets[i].active) { b = &g_bullets[i]; break; }
    if (!b) return;

    b->active  = 1;
    b->owner   = owner;
    b->bounces = 0;
    b->life    = BULLET_LIFE;
    b->grace   = 0.08f;       /* 出膛瞬间不打自己，之后连自己的炮都能打 */
    /* 出生位置：从车体中心沿朝向往外推到炮口，避免一出生就和自己重叠 */
    b->pos.x   = t->pos.x + dx * (TANK_HALF + 3.0f);
    b->pos.y   = t->pos.y + dy * (TANK_HALF + 3.0f);
    b->vel.x   = dx * BULLET_SPEED;
    b->vel.y   = dy * BULLET_SPEED;

    t->cd = FIRE_CD;
    AddSpark(b->pos.x, b->pos.y, C_BULLET);    /* 炮口火光 */
}

/* 尝试位移。核心是“X、Y 分开判”：
 *   先单独试 X，能走才改 X；再单独试 Y，能走才改 Y。
 * 这样贴着墙斜推时，被挡住的那个轴不动、另一个轴照走，
 * 表现就是“顺墙滑行”，而不是整个人卡死。                          */
static void MoveTank(Tank *t, float dx, float dy, int me)
{
    float nx, ny;
    if (dx != 0.0f) {
        nx = t->pos.x + dx;
        if (!BoxHitsWall(nx, t->pos.y, TANK_HALF) && !HitsOtherTank(nx, t->pos.y, me))
            t->pos.x = nx;
    }
    if (dy != 0.0f) {
        ny = t->pos.y + dy;
        if (!BoxHitsWall(t->pos.x, ny, TANK_HALF) && !HitsOtherTank(t->pos.x, ny, me))
            t->pos.y = ny;
    }
}

/* 控制一辆坦克一整帧的行为，顺序是：
 *   计时器 → 死亡倒计时 → 开火 / 放置道具 → 读方向 → 转向 → 位移
 * 把“按键”拆成一串 int 参数传进来，所以键盘和自动演示能复用
 * 同一个函数（见 UpdateGame 里的 AUTO_DEMO 分支）。
 *
 * placeMine / placeBrick 也是边沿触发：按住不放只会放一个，
 * 想连着放两个得松开再按（免得一激动把两个存货全埋在同一格）。      */
static void ControlTank(int idx, PlayerInput in, float dt)
{
    Tank *t = &g_tanks[idx];
    int vx = 0, vy = 0;
    float dx = 0.0f, dy = 0.0f;

    if (t->cd > 0.0f) t->cd -= dt;
    if (t->shield > 0.0f) t->shield -= dt;

    /* 死亡状态：只倒计时，到点回出生点满血复活 */
    if (!t->alive) {
        t->respawn -= dt;
        if (t->respawn <= 0.0f) {
            /* 出生点有可能被对手提前砌了砖（那时车是死的，砖块判定不会拦）。
             * 要是不管，一复活就整个卡在墙里，所以先把那格凿开。      */
            int sx = (int)floorf(t->spawn.x / TILE);
            int sy = (int)floorf(t->spawn.y / TILE);
            if (g_map[sy][sx] == 'B') {
                g_map[sy][sx] = '.';
                g_brickHp[sy][sx] = 0;
            }
            t->pos = t->spawn;
            t->angle = t->spawnAngle;
            t->hp = MAX_HP;
            t->alive = 1;
            t->cd = 0.0f;
            t->shield = SHIELD_TIME;
            AddSpark(t->pos.x, t->pos.y, t->color);
        }
        return;
    }

    /* 一次性动作只消费 pressed 边沿；网络输入和本地输入使用同一语义。 */
    if (in.pressed & IN_FIRE)  Fire(t, idx);
    if (in.pressed & IN_MINE)  PlaceMine(t, idx);
    if (in.pressed & IN_BRICK) PlaceBrick(t, idx);

    if (in.held & IN_UP)    vy -= 1;
    if (in.held & IN_DOWN)  vy += 1;
    if (in.held & IN_LEFT)  vx -= 1;
    if (in.held & IN_RIGHT) vx += 1;

    /* 【有角度的转向】
     * 把 4 个方向键合成一个“目标方向向量”，再换算成目标角度：
     *     单按 W          → 目标朝上(0°)
     *     W + A 一起按    → 目标朝左上(-45°)，车体一边转过去一边斜着开
     *     W + D 一起按    → 朝右上(+45°)，以此类推，任意两键组合都行
     * 松开所有键 → 保持当前朝向，停车。
     *
     * 转向不是瞬间完成的：每帧最多转 TANK_TURN*dt 弧度，所以能看到车体
     * 平滑地打方向；而且这段时间里它会“边转边走”，不会先停下来再走。 */
    if (vx != 0 || vy != 0) {
        float target = atan2f((float)vx, (float)-vy);   /* (-π, π]，0 = 朝上 */
        float diff   = AngleDiff(t->angle, target);     /* 还差多少弧度 */
        float step   = TANK_TURN * dt;                  /* 这一帧最多转多少 */
        float align;

        if (diff >  step) diff =  step;                 /* 夹住角速度 */
        if (diff < -step) diff = -step;
        t->angle += diff;

        /* 前进方向永远是“车头朝哪就走哪”，不是按键盘方向平移。
         * 车速乘 cos(剩余夹角)：正对目标时全速，差 90° 时速度为 0
         * （相当于原地打方向），斜向时自动折中成边走边转。        */
        align = cosf(AngleDiff(t->angle, target));
        if (align < 0.0f) align = 0.0f;

        AngleDir(t->angle, &dx, &dy);
        dx *= TANK_SPEED * dt * align;
        dy *= TANK_SPEED * dt * align;
        if (dx != 0.0f || dy != 0.0f) MoveTank(t, dx, dy, idx);
    }
}

/* 炮弹命中判定（逐个子弹、每个子步都调用）
 * 【不分敌我】自己的炮弹一样会打伤自己，碰到谁谁掉血；
 * 万一被自己的炮弹打死，分数是给对方 —— 见 HurtTank。          */
static void BulletHitCheck(Bullet *b)
{
    int j;
    for (j = 0; j < 2; j++) {
        Tank *t = &g_tanks[j];
        if (!t->alive) continue;                              /* 尸体不挡子弹 */
        if (t->shield > 0.0f) continue;                       /* 护盾期内免伤 */
        if (j == b->owner && b->grace > 0.0f) continue;   /* 刚出膛，先不结算自伤 */
        /* 圆 / 方近似：用矩形判，误差在视觉上完全看不出来 */
        if (fabsf(b->pos.x - t->pos.x) <= TANK_HALF + BULLET_R &&
            fabsf(b->pos.y - t->pos.y) <= TANK_HALF + BULLET_R) {

            b->active = 0;                                    /* 子弹消失 */
            HurtTank(j, 1, b->pos.x, b->pos.y);
            return;                                           /* 一发只打中一辆 */
        }
    }
}

/* 探测子弹移动到 (nx,ny) 会撞到什么：
 *   0 = 畅通    1 = 撞砖墙（当场扣 1 点血，子弹被吸收）    2 = 撞钢墙/边界（反弹）
 *
 * 顺带把“扣血”放在这里做，因为它天然知道撞的是哪一格，
 * 调用方就不用再算一次格子坐标。                                    */
static int BulletProbe(float nx, float ny)
{
    int x0 = (int)floorf((nx - BULLET_R) / TILE);
    int x1 = (int)floorf((nx + BULLET_R - 0.001f) / TILE);
    int y0 = (int)floorf((ny - BULLET_R) / TILE);
    int y1 = (int)floorf((ny + BULLET_R - 0.001f) / TILE);
    int cx, cy;

    if (x0 < 0 || y0 < 0 || x1 >= MAP_W || y1 >= MAP_H) return 2;   /* 出界=反弹 */

    for (cy = y0; cy <= y1; cy++) {
        for (cx = x0; cx <= x1; cx++) {
            if (g_map[cy][cx] == 'B') {
                DamageBrick(cx, cy, nx, ny);    /* 砖墙：掉血，子弹不反弹 */
                return 1;
            }
            if (g_map[cy][cx] == '#') return 2;  /* 钢墙：留给调用方做反射 */
        }
    }
    return 0;
}

/* 子弹推进。全程序最需要小心的一段，分三层理解：
 *
 * 【第一层：为什么要“子步”】
 *   子弹 300px/s，60 帧时每帧要走 5px，而子弹半径只有 3px、一格墙 24px。
 *   如果一帧只挪一次，“这一帧还在墙前、下一帧已经在墙后”就会穿墙飞过去。
 *   所以把一帧位移按 3px 一刀切成 steps 份，每一小步都做一次碰撞检测。
 *
 * 【第二层：为什么 X、Y 要分开判】
 *   一起判的话，斜着撞墙时分不清撞的是竖直面还是水平面，反弹方向就会错。
 *   分开之后：X 被挡只翻 vel.x，Y 被挡只翻 vel.y —— 这正是镜面反射。
 *
 * 【第三层：撞到不同东西的处理】
 *   砖墙(hit=1)：子弹直接被吸收（active=0），所以砖墙背后是安全的；
 *   钢墙(hit=2)：翻速度分量、把这一步的方向也取反、计数 +1，
 *                超过 MAX_BOUNCE 次就销毁，防止子弹永远乱飞。      */
static void UpdateBullets(float dt)
{
    int i, s, steps;
    Bullet *b;

    for (i = 0; i < MAX_BULLETS_ALL; i++) {
        b = &g_bullets[i];
        if (!b->active) continue;

        b->life -= dt;
        if (b->grace > 0.0f) b->grace -= dt;          /* 出膛保护递减 */
        if (b->life <= 0.0f) { b->active = 0; continue; }

        /* 高速子弹分步推进，避免穿墙：保证每步位移不超过 3px */
        steps = (int)(BULLET_SPEED * dt / 3.0f) + 1;
        {
            /* 把整帧位移平均摊到每一步步上 */
            float stepx = b->vel.x * dt / (float)steps;
            float stepy = b->vel.y * dt / (float)steps;

            /* b->active 也进循环条件：一旦被打掉，后面的子步不再算 */
            for (s = 0; s < steps && b->active; s++) {
                /* ---- X 方向 ---- */
                if (stepx != 0.0f) {
                    float nx = b->pos.x + stepx;
                    int hit = BulletProbe(nx, b->pos.y);
                    if (hit == 1) {              /* 砖墙：吸收子弹并掉血 */
                        b->active = 0;
                        break;
                    } else if (hit == 2) {       /* 钢墙 / 边界：反弹 */
                        b->vel.x = -b->vel.x;
                        stepx = -stepx;          /* 这一步的方向也要跟着翻 */
                        b->bounces++;
                        AddSpark(b->pos.x, b->pos.y, C_BULLET);
                        if (b->bounces > MAX_BOUNCE) { b->active = 0; break; }
                    } else {
                        b->pos.x = nx;           /* 畅通，真的走过去 */
                    }
                }
                /* ---- Y 方向（同 X，两者互不影响）---- */
                if (stepy != 0.0f) {
                    float ny = b->pos.y + stepy;
                    int hit = BulletProbe(b->pos.x, ny);
                    if (hit == 1) {
                        b->active = 0;
                        break;
                    } else if (hit == 2) {
                        b->vel.y = -b->vel.y;
                        stepy = -stepy;
                        b->bounces++;
                        AddSpark(b->pos.x, b->pos.y, C_BULLET);
                        if (b->bounces > MAX_BOUNCE) { b->active = 0; break; }
                    } else {
                        b->pos.y = ny;
                    }
                }
                /* 每走一小步就查一次有没有打中坦克（而不是一帧只查一次），
                   否则高速子弹会从坦克身上“飞过去” */
                BulletHitCheck(b);
            }
        }
    }
}

/* 追踪弹爆炸：半径内所有坦克吃伤害 —— 包括发射者自己，
 * 也就是说贴脸放追踪弹同样会把自己炸到（伤害不分敌我）。      */
static void ExplodeMissile(Missile *m)
{
    int k, j;
    m->active = 0;
    for (k = 0; k < 22; k++) AddSpark(m->pos.x, m->pos.y, C_ITEM_HOMING);
    for (j = 0; j < 2; j++) {
        Tank *t = &g_tanks[j];
        float dx, dy;
        if (!t->alive) continue;
        dx = t->pos.x - m->pos.x;
        dy = t->pos.y - m->pos.y;
        if (dx * dx + dy * dy <= MISSILE_RADIUS * MISSILE_RADIUS)
            HurtTank(j, MISSILE_DMG, m->pos.x, m->pos.y);
    }
}

/* 追踪弹推进。两个阶段：
 *   瞄准 —— 准星跟随敌人，倒计时结束的瞬间锁定敌人的当前位置
 *   飞行 —— 直奔那个锁定点，全程不判墙（所以能穿迷宫），靠近就爆
 * 敌人只要在锁定后离开那个点，就能躲开 —— 这就是"瞄准时间"的意义。 */
static void UpdateMissiles(float dt)
{
    int i, j;
    for (i = 0; i < MAX_MISSILES; i++) {
        Missile *m = &g_missiles[i];
        if (!m->active) continue;

        if (m->phase == 0) {
            Tank *tg = &g_tanks[1 - m->owner];
            if (tg->alive) m->lockPos = tg->pos;   /* 瞄准期间准星一直跟着 */
            m->timer -= dt;
            if (m->timer <= 0.0f) {                /* 时间到：锁定并发射 */
                float vx = m->lockPos.x - m->origin.x;
                float vy = m->lockPos.y - m->origin.y;
                float len = sqrtf(vx * vx + vy * vy) + 0.001f;
                m->phase = 1;
                m->pos = m->origin;
                m->vel.x = vx / len * MISSILE_SPEED;
                m->vel.y = vy / len * MISSILE_SPEED;
            }
            continue;
        }

        {
            float vx = m->lockPos.x - m->pos.x;
            float vy = m->lockPos.y - m->pos.y;
            float len = sqrtf(vx * vx + vy * vy);

            if (len < 8.0f) { ExplodeMissile(m); continue; }

            /* 朝锁定点缓慢转向（不是瞬间对准），所以跑远一点就能躲掉 */
            m->vel.x += (vx / len * MISSILE_SPEED - m->vel.x) * MISSILE_TURN;
            m->vel.y += (vy / len * MISSILE_SPEED - m->vel.y) * MISSILE_TURN;
            m->pos.x += m->vel.x * dt;
            m->pos.y += m->vel.y * dt;
            m->life  -= dt;

            if (m->life <= 0.0f) { ExplodeMissile(m); continue; }

            /* 直接撞到人也立即引爆 */
            for (j = 0; j < 2; j++) {
                Tank *t = &g_tanks[j];
                if (!t->alive) continue;
                if (fabsf(t->pos.x - m->pos.x) <= TANK_HALF + 4.0f &&
                    fabsf(t->pos.y - m->pos.y) <= TANK_HALF + 4.0f) break;
            }
            if (j < 2) ExplodeMissile(m);
        }
    }
}

/* 地雷：谁踩都一样 —— 包括埋雷的人自己（想退回“只炸敌人”就把
 * MINE_FRIENDLY 改成 0）。
 *
 * 触发用“走进来”判定，而不是“在圈里”判定：
 * 每帧把站在触发圈里的车记成位掩码 m->inside，只有“这一帧在圈里、
 * 上一帧不在”才算踩上去。这样布防结束时自己还站在雷上不会被自己炸，
 * 但只要走开再回来，一样会炸到自己。
 * 布防期(m->arm > 0) 只更新 inside，不结算伤害。                   */
static void UpdateMines(float dt)
{
    int i, j;

    for (i = 0; i < MAX_MINES; i++) {
        Mine *m = &g_mines[i];
        int mask = 0, enter;

        if (!m->active) continue;

        m->life -= dt;
        if (m->blink > 0.0f) m->blink -= dt;
        if (m->life <= 0.0f) { m->active = 0; continue; }

        /* 这一帧有哪些车在触发圈里 */
        for (j = 0; j < 2; j++) {
            Tank *t = &g_tanks[j];
            float dx, dy;
            if (!t->alive) continue;
            dx = t->pos.x - m->pos.x;
            dy = t->pos.y - m->pos.y;
            if (dx * dx + dy * dy <= MINE_TRIGGER * MINE_TRIGGER) mask |= 1 << j;
        }
        enter = mask & ~m->inside;      /* 刚刚走进来的车 */
        m->inside = mask;

        if (m->arm > 0.0f) { m->arm -= dt; continue; }    /* 布防中，只记录不触发 */

#if MINE_FRIENDLY
        if (!enter) continue;                             /* 任何人走进来都炸 */
#else
        if (!(enter & (1 << (1 - m->owner)))) continue;   /* 旧规则：只炸敌人 */
#endif

        /* 爆炸：半径内所有车都吃伤害，不区分是谁埋的、是谁踩的 */
        m->active = 0;
        for (j = 0; j < 24; j++) AddSpark(m->pos.x, m->pos.y, C_ITEM_MINE);
        for (j = 0; j < 2; j++) {
            Tank *t = &g_tanks[j];
            float dx, dy;
#if !MINE_FRIENDLY
            if (j == m->owner) continue;
#endif
            if (!t->alive) continue;
            dx = t->pos.x - m->pos.x;
            dy = t->pos.y - m->pos.y;
            if (dx * dx + dy * dy <= MINE_BLAST * MINE_BLAST)
                HurtTank(j, MINE_DMG, m->pos.x, m->pos.y);
        }
    }
}

/* 道具颜色：红 / 绿 / 黄 / 蓝，绘制和粒子都用它 */
static COLORREF ItemColor(int type)
{
    if (type == 0) return C_ITEM_LASER;
    if (type == 1) return C_ITEM_HOMING;
    if (type == 2) return C_ITEM_MINE;
    return C_ITEM_BRICK;
}

/* 在随机空地放一个道具，并且故意离两辆车都远一点（别正好掉在脸上） */
static void SpawnItem(void)
{
    Item *it = NULL;
    int i, tries, gx, gy;
    float px, py;

    for (i = 0; i < MAX_ITEMS; i++)
        if (!g_items[i].active) { it = &g_items[i]; break; }
    if (!it) return;

    for (tries = 0; tries < 60; tries++) {
        gx = rand() % MAP_W;
        gy = rand() % MAP_H;
        if (g_map[gy][gx] != '.') continue;
        px = gx * TILE + TILE * 0.5f;
        py = gy * TILE + TILE * 0.5f;
        if (fabsf(px - g_tanks[0].pos.x) < 56.0f &&
            fabsf(py - g_tanks[0].pos.y) < 56.0f) continue;
        if (fabsf(px - g_tanks[1].pos.x) < 56.0f &&
            fabsf(py - g_tanks[1].pos.y) < 56.0f) continue;

        it->active = 1;
        it->type   = rand() % ITEM_KINDS;   /* 四色等概率 */
        it->pos.x  = px;
        it->pos.y  = py;
        it->life   = ITEM_LIFE;
        it->phase  = 0.0f;
        return;
    }
}

/* 拾取结算：把“捡到的东西”写进坦克状态。
 * 返回 1 = 收下了，0 = 拿不下（地雷/砖块已经攒满 2 个）。
 *
 * 关键规则：
 *   - 红、绿是武器槽，直接覆盖 —— 拾到新的，旧武器连同剩余发数一起作废，
 *     所以“激光还剩 1 发就去捡追踪弹”是划算的，“还剩 3 发就去捡”是亏的。
 *   - 黄、蓝是存货槽，不会被别的道具顶掉，各自最多 2 个；
 *     满了就不收，道具留在地上等下一趟（免得白捡一个扔一个）。      */
static int PickItem(Tank *t, int type)
{
    if (type == 0) {                       /* 红：折射激光炮，3 次 */
        t->weapon = WPN_LASER;
        t->ammo   = ITEM_USES;
        return 1;
    }
    if (type == 1) {                       /* 绿：追踪弹，3 次 */
        t->weapon = WPN_HOMING;
        t->ammo   = ITEM_USES;
        return 1;
    }
    if (type == 2) {                       /* 黄：地雷，存 2 个 */
        if (t->mines >= MAX_SLOT) return 0;
        t->mines++;
        return 1;
    }
    if (t->bricks >= MAX_SLOT) return 0;   /* 蓝：砖块，存 2 个 */
    t->bricks++;
    return 1;
}

/* 道具的生成、超时消失、拾取结算 */
static void UpdateItems(float dt)
{
    int i, j, k;

    g_itemTimer -= dt;
    if (g_itemTimer <= 0.0f) {
        SpawnItem();
        g_itemTimer = ITEM_SPAWN + (float)(rand() % 40) / 10.0f;   /* 6~10 秒一个 */
    }

    for (i = 0; i < MAX_ITEMS; i++) {
        Item *it = &g_items[i];
        if (!it->active) continue;
        it->life  -= dt;
        it->phase += dt;
        if (it->life <= 0.0f) { it->active = 0; continue; }

        for (j = 0; j < 2; j++) {
            Tank *t = &g_tanks[j];
            float dx, dy;
            if (!t->alive) continue;
            dx = t->pos.x - it->pos.x;
            dy = t->pos.y - it->pos.y;
            if (dx * dx + dy * dy > (TANK_HALF + ITEM_R) * (TANK_HALF + ITEM_R)) continue;

            if (!PickItem(t, it->type)) break;   /* 攒满了：道具留在地上 */

            for (k = 0; k < 12; k++)
                AddSpark(it->pos.x, it->pos.y, ItemColor(it->type));
            it->active = 0;
            break;
        }
    }
}

/* 纯游戏逻辑：本地和联机主机共用，客户端绝不会调用。 */
static void SimulateTick(const PlayerInput in[2], float dt)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_flash[i] > 0.0f) g_flash[i] -= dt;
        if (g_lasers[i].active) {
            g_lasers[i].life -= dt;
            if (g_lasers[i].life <= 0.0f) g_lasers[i].active = 0;
        }
    }

    if (g_state == 1) {
        UpdateSparks(dt);
        return;
    }

    g_timeLeft -= dt;
    if (g_timeLeft <= 0.0f) {
        g_timeLeft = 0.0f;
        EndRound(1);
        return;
    }

    ControlTank(0, in[0], dt);
    ControlTank(1, in[1], dt);
    UpdateBullets(dt);
    UpdateMissiles(dt);
    UpdateMines(dt);
    UpdateItems(dt);
    UpdateSparks(dt);
}

/* 本地模式包装：负责读键盘和 R 重开，固定 tick 的核心仍在 SimulateTick。 */
static void UpdateGame(float dt)
{
    PlayerInput in[2] = { { 0, 0 }, { 0, 0 } };
    int r = KeyDown('R');

    if (g_state == 1) {
        if (r && !g_prevR) {
            ResetGame();
            g_prevR = r;
            return;
        }
        g_prevR = r;
        SimulateTick(in, dt);
        return;
    }
    g_prevR = r;

#ifdef AUTO_DEMO
    {
        static const uint32_t k[4] = { IN_UP, IN_RIGHT, IN_DOWN, IN_LEFT };
        int p0 = ((int)(g_autoT / 0.75f)) % 4;
        int p1 = ((int)(g_autoT / 0.85f) + 2) % 4;
        int f  = ((int)(g_autoT * 3.0f) % 2) ? 0 : 1;
        uint32_t k0 = k[p0], k1 = k[p1];
        g_autoT += dt;
        if (f) { k0 |= IN_FIRE; k1 |= IN_FIRE; }
        in[0] = MakeInput(0, k0);
        in[1] = MakeInput(1, k1);
    }
#else
    in[0] = MakeInput(0, ReadP1Keys());
    in[1] = MakeInput(1, ReadP2Keys());
#endif

    SimulateTick(in, dt);
}

/* =============================== 局域网联机 ============================== */

#define NET_DEFAULT_PORT 27515
#define NET_MAGIC        0x54424750u      /* "TBGP" */
#define NET_VERSION      1
#define NET_HEADER_SIZE  20
#define NET_MAX_PAYLOAD  16384
#define NET_RX_SIZE      65536
#define NET_TX_SIZE      524288
#define NET_TIMEOUT_MS   5000
#define NET_MAX_TICK_CATCHUP 8

enum {
    NET_MSG_HELLO = 1,
    NET_MSG_WELCOME,
    NET_MSG_ERROR,
    NET_MSG_READY,
    NET_MSG_LOBBY,
    NET_MSG_START,
    NET_MSG_INPUT,
    NET_MSG_SNAPSHOT,
    NET_MSG_PING,
    NET_MSG_PONG,
    NET_MSG_LEAVE
};

typedef enum {
    APP_MENU = 0,
    APP_LOCAL,
    APP_HOST,
    APP_CLIENT
} AppMode;

typedef enum {
    NET_NONE = 0,
    NET_WAIT_PEER,
    NET_CONNECTING,
    NET_HANDSHAKE,
    NET_LOBBY,
    NET_RUNNING,
    NET_DISCONNECTED
} NetPhase;

typedef struct {
    SOCKET listenSock;
    SOCKET sock;
    int    connecting;
    int    handshakeDone;

    uint32_t session;
    uint32_t round;
    uint32_t invite;
    uint32_t tick;
    uint32_t inputSeq;
    uint32_t remoteInputSeq;
    uint32_t ackInputSeq;
    uint32_t remoteHeld;
    uint32_t remotePressed;
    uint32_t lastSnapshotTick;
    uint16_t port;

    uint32_t localReady;
    uint32_t remoteReady;

    unsigned char rx[NET_RX_SIZE];
    int rxLen;
    unsigned char tx[NET_TX_SIZE];
    int txLen;

    ULONGLONG lastRecv;
    ULONGLONG lastLobbySend;
    unsigned char snapshotBuf[NET_MAX_PAYLOAD];

    wchar_t localIp[64];
    wchar_t status[160];
} NetState;

static AppMode   g_appMode = APP_MENU;
static NetPhase  g_netPhase = NET_NONE;
static NetState  g_net;
static HINSTANCE g_hInst;
static HWND      g_mainWnd;
static int       g_wsaStarted = 0;
static int       g_netQuiet = 0;   /* 测试模式：断线只改状态，不弹窗口 */

static int CreateMenuWindow(void);
/* ------------------------- 小字节序安全编码层 ------------------------- */

typedef struct {
    unsigned char *buf;
    int cap, pos, ok;
} NetWriter;

typedef struct {
    const unsigned char *buf;
    int len, pos, ok;
} NetReader;

static void NW_U8(NetWriter *w, unsigned char v)
{
    if (w->pos + 1 > w->cap) { w->ok = 0; return; }
    w->buf[w->pos++] = v;
}

static void NW_U16(NetWriter *w, uint16_t v)
{
    uint16_t n = htons(v);
    if (w->pos + 2 > w->cap) { w->ok = 0; return; }
    memcpy(w->buf + w->pos, &n, 2);
    w->pos += 2;
}

static void NW_U32(NetWriter *w, uint32_t v)
{
    uint32_t n = htonl(v);
    if (w->pos + 4 > w->cap) { w->ok = 0; return; }
    memcpy(w->buf + w->pos, &n, 4);
    w->pos += 4;
}

static void NW_I32(NetWriter *w, int32_t v) { NW_U32(w, (uint32_t)v); }

static void NW_F32(NetWriter *w, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    NW_U32(w, bits);
}

static unsigned char NR_U8(NetReader *r)
{
    if (r->pos + 1 > r->len) { r->ok = 0; return 0; }
    return r->buf[r->pos++];
}

static uint16_t NR_U16(NetReader *r)
{
    uint16_t n;
    if (r->pos + 2 > r->len) { r->ok = 0; return 0; }
    memcpy(&n, r->buf + r->pos, 2);
    r->pos += 2;
    return ntohs(n);
}

static uint32_t NR_U32(NetReader *r)
{
    uint32_t n;
    if (r->pos + 4 > r->len) { r->ok = 0; return 0; }
    memcpy(&n, r->buf + r->pos, 4);
    r->pos += 4;
    return ntohl(n);
}

static int32_t NR_I32(NetReader *r) { return (int32_t)NR_U32(r); }

static float NR_F32(NetReader *r)
{
    uint32_t bits = NR_U32(r);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static int NetFinite(float v)
{
    return v == v && fabsf(v) < 1000000.0f;
}

/* --------------------------- 完整游戏快照 ---------------------------- */

typedef struct {
    uint32_t tick, ackInputSeq;
    int state, winner, endReason, sparkIdx;
    float timeLeft, flash[2], itemTimer;
    char map[MAP_H][MAP_W + 1];
    int brickHp[MAP_H][MAP_W];
    Tank tanks[2];
    Bullet bullets[MAX_BULLETS_ALL];
    Laser lasers[2];
    Missile missiles[MAX_MISSILES];
    Mine mines[MAX_MINES];
    Item items[MAX_ITEMS];
    Spark sparks[MAX_SPARKS];
} NetSnapshot;

static NetSnapshot g_snapTmp;

static int EncodeSnapshot(unsigned char *buf, int cap)
{
    NetWriter w = { buf, cap, 0, 1 };
    int i, j;

    NW_U32(&w, g_net.tick);
    NW_U32(&w, g_net.ackInputSeq);
    NW_U8(&w, (unsigned char)g_state);
    NW_U8(&w, (unsigned char)(g_winner + 1));
    NW_U8(&w, (unsigned char)g_endReason);
    NW_F32(&w, g_timeLeft);
    NW_F32(&w, g_flash[0]);
    NW_F32(&w, g_flash[1]);
    NW_F32(&w, g_itemTimer);
    NW_U32(&w, (uint32_t)g_sparkIdx);
    for (i = 0; i < MAP_H; i++)
        for (j = 0; j < MAP_W; j++)
            NW_U8(&w, (unsigned char)g_map[i][j]);
    for (i = 0; i < MAP_H; i++)
        for (j = 0; j < MAP_W; j++)
            NW_U8(&w, (unsigned char)g_brickHp[i][j]);

    for (i = 0; i < 2; i++) {
        Tank *t = &g_tanks[i];
        NW_F32(&w, t->pos.x); NW_F32(&w, t->pos.y);
        NW_F32(&w, t->spawn.x); NW_F32(&w, t->spawn.y);
        NW_F32(&w, t->spawnAngle); NW_F32(&w, t->angle);
        NW_I32(&w, t->hp); NW_I32(&w, t->score);
        NW_F32(&w, t->cd); NW_F32(&w, t->shield);
        NW_U8(&w, (unsigned char)t->alive);
        NW_F32(&w, t->respawn);
        NW_I32(&w, t->weapon); NW_I32(&w, t->ammo);
        NW_I32(&w, t->mines); NW_I32(&w, t->bricks);
    }

    for (i = 0; i < MAX_BULLETS_ALL; i++) {
        Bullet *b = &g_bullets[i];
        NW_U8(&w, (unsigned char)b->active);
        NW_U8(&w, (unsigned char)b->owner);
        NW_U8(&w, (unsigned char)b->bounces);
        NW_F32(&w, b->pos.x); NW_F32(&w, b->pos.y);
        NW_F32(&w, b->vel.x); NW_F32(&w, b->vel.y);
        NW_F32(&w, b->life); NW_F32(&w, b->grace);
    }

    for (i = 0; i < 2; i++) {
        Laser *l = &g_lasers[i];
        NW_U8(&w, (unsigned char)l->active);
        NW_U8(&w, (unsigned char)l->owner);
        NW_F32(&w, l->life);
        NW_U8(&w, (unsigned char)l->npts);
        for (j = 0; j < MAX_LASER_PTS; j++) {
            NW_F32(&w, l->pts[j].x);
            NW_F32(&w, l->pts[j].y);
        }
    }

    for (i = 0; i < MAX_MISSILES; i++) {
        Missile *m = &g_missiles[i];
        NW_U8(&w, (unsigned char)m->active);
        NW_U8(&w, (unsigned char)m->owner);
        NW_U8(&w, (unsigned char)m->phase);
        NW_F32(&w, m->timer); NW_F32(&w, m->life);
        NW_F32(&w, m->origin.x); NW_F32(&w, m->origin.y);
        NW_F32(&w, m->pos.x); NW_F32(&w, m->pos.y);
        NW_F32(&w, m->vel.x); NW_F32(&w, m->vel.y);
        NW_F32(&w, m->lockPos.x); NW_F32(&w, m->lockPos.y);
    }

    for (i = 0; i < MAX_MINES; i++) {
        Mine *m = &g_mines[i];
        NW_U8(&w, (unsigned char)m->active);
        NW_U8(&w, (unsigned char)m->owner);
        NW_F32(&w, m->pos.x); NW_F32(&w, m->pos.y);
        NW_F32(&w, m->arm); NW_F32(&w, m->life); NW_F32(&w, m->blink);
        NW_U8(&w, (unsigned char)m->inside);
    }

    for (i = 0; i < MAX_ITEMS; i++) {
        Item *it = &g_items[i];
        NW_U8(&w, (unsigned char)it->active);
        NW_U8(&w, (unsigned char)it->type);
        NW_F32(&w, it->pos.x); NW_F32(&w, it->pos.y);
        NW_F32(&w, it->life); NW_F32(&w, it->phase);
    }

    for (i = 0; i < MAX_SPARKS; i++) {
        Spark *s = &g_sparks[i];
        NW_F32(&w, s->pos.x); NW_F32(&w, s->pos.y);
        NW_F32(&w, s->vel.x); NW_F32(&w, s->vel.y);
        NW_F32(&w, s->life); NW_F32(&w, s->maxLife);
        NW_U32(&w, (uint32_t)s->color);
    }

    return w.ok ? w.pos : -1;
}

static int DecodeSnapshot(const unsigned char *buf, int len)
{
    NetReader r = { buf, len, 0, 1 };
    NetSnapshot *s = &g_snapTmp;
    int i, j;

    memset(s, 0, sizeof(*s));
    s->tick = NR_U32(&r);
    s->ackInputSeq = NR_U32(&r);
    s->state = NR_U8(&r);
    s->winner = (int)NR_U8(&r) - 1;
    s->endReason = NR_U8(&r);
    s->timeLeft = NR_F32(&r);
    s->flash[0] = NR_F32(&r);
    s->flash[1] = NR_F32(&r);
    s->itemTimer = NR_F32(&r);
    s->sparkIdx = (int)NR_U32(&r);
    if (!r.ok) return 0;
    if (s->state > 1 || s->winner < -1 || s->winner > 1 || s->endReason > 1 ||
        s->sparkIdx < 0 || s->sparkIdx >= MAX_SPARKS ||
        !NetFinite(s->timeLeft) || !NetFinite(s->flash[0]) ||
        !NetFinite(s->flash[1]) || !NetFinite(s->itemTimer)) return 0;

    for (i = 0; i < MAP_H; i++)
        for (j = 0; j < MAP_W; j++) {
            char c = (char)NR_U8(&r);
            if (c != '#' && c != 'B' && c != '.') return 0;
            s->map[i][j] = c;
        }
    for (i = 0; i < MAP_H; i++)
        for (j = 0; j < MAP_W; j++) {
            int hp = NR_U8(&r);
            if (hp < 0 || hp > BRICK_HP) return 0;
            s->brickHp[i][j] = hp;
            if (s->map[i][j] == 'B' && hp == 0) return 0;
            if (s->map[i][j] != 'B' && hp != 0) return 0;
        }

    for (i = 0; i < 2; i++) {
        Tank *t = &s->tanks[i];
        t->pos.x = NR_F32(&r); t->pos.y = NR_F32(&r);
        t->spawn.x = NR_F32(&r); t->spawn.y = NR_F32(&r);
        t->spawnAngle = NR_F32(&r); t->angle = NR_F32(&r);
        t->hp = NR_I32(&r); t->score = NR_I32(&r);
        t->cd = NR_F32(&r); t->shield = NR_F32(&r);
        t->alive = NR_U8(&r);
        t->respawn = NR_F32(&r);
        t->weapon = NR_I32(&r); t->ammo = NR_I32(&r);
        t->mines = NR_I32(&r); t->bricks = NR_I32(&r);
        if (!r.ok) return 0;
        if (t->hp < 0 || t->hp > MAX_HP || t->score < 0 || t->score > WIN_SCORE ||
            t->alive > 1 || t->respawn < 0.0f || t->respawn > RESPAWN_TIME + 1.0f ||
            t->weapon < WPN_NORMAL || t->weapon > WPN_HOMING ||
            t->ammo < 0 || t->ammo > ITEM_USES ||
            t->mines < 0 || t->mines > MAX_SLOT || t->bricks < 0 || t->bricks > MAX_SLOT ||
            !NetFinite(t->pos.x) || !NetFinite(t->pos.y) ||
            !NetFinite(t->spawn.x) || !NetFinite(t->spawn.y) ||
            !NetFinite(t->spawnAngle) || !NetFinite(t->angle) ||
            !NetFinite(t->cd) || !NetFinite(t->shield) || !NetFinite(t->respawn)) return 0;
    }

    for (i = 0; i < MAX_BULLETS_ALL; i++) {
        Bullet *b = &s->bullets[i];
        b->active = NR_U8(&r); b->owner = NR_U8(&r); b->bounces = NR_U8(&r);
        b->pos.x = NR_F32(&r); b->pos.y = NR_F32(&r);
        b->vel.x = NR_F32(&r); b->vel.y = NR_F32(&r);
        b->life = NR_F32(&r); b->grace = NR_F32(&r);
        if (!r.ok) return 0;
        if (b->owner > 1 || b->bounces > MAX_BOUNCE + 1 ||
            !NetFinite(b->pos.x) || !NetFinite(b->pos.y) ||
            !NetFinite(b->vel.x) || !NetFinite(b->vel.y) ||
            !NetFinite(b->life) || !NetFinite(b->grace)) return 0;
    }

    for (i = 0; i < 2; i++) {
        Laser *l = &s->lasers[i];
        l->active = NR_U8(&r); l->owner = NR_U8(&r);
        l->life = NR_F32(&r); l->npts = NR_U8(&r);
        for (j = 0; j < MAX_LASER_PTS; j++) {
            l->pts[j].x = NR_F32(&r);
            l->pts[j].y = NR_F32(&r);
        }
        if (!r.ok) return 0;
        if (l->owner > 1 || l->npts > MAX_LASER_PTS || !NetFinite(l->life)) return 0;
    }

    for (i = 0; i < MAX_MISSILES; i++) {
        Missile *m = &s->missiles[i];
        m->active = NR_U8(&r); m->owner = NR_U8(&r); m->phase = NR_U8(&r);
        m->timer = NR_F32(&r); m->life = NR_F32(&r);
        m->origin.x = NR_F32(&r); m->origin.y = NR_F32(&r);
        m->pos.x = NR_F32(&r); m->pos.y = NR_F32(&r);
        m->vel.x = NR_F32(&r); m->vel.y = NR_F32(&r);
        m->lockPos.x = NR_F32(&r); m->lockPos.y = NR_F32(&r);
        if (!r.ok) return 0;
        if (m->owner > 1 || m->phase > 1 || !NetFinite(m->timer) || !NetFinite(m->life) ||
            !NetFinite(m->origin.x) || !NetFinite(m->origin.y) ||
            !NetFinite(m->pos.x) || !NetFinite(m->pos.y) ||
            !NetFinite(m->vel.x) || !NetFinite(m->vel.y) ||
            !NetFinite(m->lockPos.x) || !NetFinite(m->lockPos.y)) return 0;
    }

    for (i = 0; i < MAX_MINES; i++) {
        Mine *m = &s->mines[i];
        m->active = NR_U8(&r); m->owner = NR_U8(&r);
        m->pos.x = NR_F32(&r); m->pos.y = NR_F32(&r);
        m->arm = NR_F32(&r); m->life = NR_F32(&r); m->blink = NR_F32(&r);
        m->inside = NR_U8(&r);
        if (!r.ok) return 0;
        if (m->owner > 1 || m->inside > 3 || !NetFinite(m->pos.x) || !NetFinite(m->pos.y) ||
            !NetFinite(m->arm) || !NetFinite(m->life) || !NetFinite(m->blink)) return 0;
    }

    for (i = 0; i < MAX_ITEMS; i++) {
        Item *it = &s->items[i];
        it->active = NR_U8(&r); it->type = NR_U8(&r);
        it->pos.x = NR_F32(&r); it->pos.y = NR_F32(&r);
        it->life = NR_F32(&r); it->phase = NR_F32(&r);
        if (!r.ok) return 0;
        if (it->type >= ITEM_KINDS || !NetFinite(it->pos.x) || !NetFinite(it->pos.y) ||
            !NetFinite(it->life) || !NetFinite(it->phase)) return 0;
    }

    for (i = 0; i < MAX_SPARKS; i++) {
        Spark *sp = &s->sparks[i];
        sp->pos.x = NR_F32(&r); sp->pos.y = NR_F32(&r);
        sp->vel.x = NR_F32(&r); sp->vel.y = NR_F32(&r);
        sp->life = NR_F32(&r); sp->maxLife = NR_F32(&r);
        sp->color = (COLORREF)NR_U32(&r);
        if (!r.ok) return 0;
        if (!NetFinite(sp->pos.x) || !NetFinite(sp->pos.y) ||
            !NetFinite(sp->vel.x) || !NetFinite(sp->vel.y) ||
            !NetFinite(sp->life) || !NetFinite(sp->maxLife)) return 0;
    }

    if (r.pos != r.len) return 0;
    return 1;
}

static void ApplySnapshot(void)
{
    NetSnapshot *s = &g_snapTmp;
    COLORREF c0 = g_tanks[0].color, c1 = g_tanks[1].color;
    int i;

    g_state = s->state;
    g_winner = s->winner;
    g_endReason = s->endReason;
    g_timeLeft = s->timeLeft;
    g_flash[0] = s->flash[0];
    g_flash[1] = s->flash[1];
    g_itemTimer = s->itemTimer;
    g_sparkIdx = s->sparkIdx;
    memcpy(g_map, s->map, sizeof(g_map));
    memcpy(g_brickHp, s->brickHp, sizeof(g_brickHp));
    memcpy(g_tanks, s->tanks, sizeof(g_tanks));
    g_tanks[0].color = c0;
    g_tanks[1].color = c1;
    memcpy(g_bullets, s->bullets, sizeof(g_bullets));
    memcpy(g_lasers, s->lasers, sizeof(g_lasers));
    memcpy(g_missiles, s->missiles, sizeof(g_missiles));
    memcpy(g_mines, s->mines, sizeof(g_mines));
    memcpy(g_items, s->items, sizeof(g_items));
    memcpy(g_sparks, s->sparks, sizeof(g_sparks));
    g_net.lastSnapshotTick = s->tick;
    g_net.ackInputSeq = s->ackInputSeq;
    (void)i;
}

/* ------------------------- socket 与收发队列 ------------------------- */

static void NetSetStatus(const wchar_t *s)
{
    wcsncpy(g_net.status, s, 159);
    g_net.status[159] = L'\0';
}

static void NetCloseSocket(SOCKET *s)
{
    if (*s != INVALID_SOCKET) {
        closesocket(*s);
        *s = INVALID_SOCKET;
    }
}

static int NetSetNonBlocking(SOCKET s)
{
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

static void NetResetState(void)
{
    NetCloseSocket(&g_net.sock);
    NetCloseSocket(&g_net.listenSock);
    memset(&g_net, 0, sizeof(g_net));
    g_net.sock = INVALID_SOCKET;
    g_net.listenSock = INVALID_SOCKET;
    g_net.invite = 0;
    g_net.session = 0;
    g_net.round = 0;
    g_net.inputSeq = 0;
    g_net.lastSnapshotTick = 0;
    g_net.status[0] = L'\0';
    g_net.localIp[0] = L'\0';
}

static int NetQueueMessage(uint16_t type, const unsigned char *payload, int len,
                           uint32_t session, uint32_t round)
{
    unsigned char header[NET_HEADER_SIZE];
    uint32_t magic = htonl(NET_MAGIC);
    uint32_t plen = htonl((uint32_t)len);
    uint32_t sess = htonl(session);
    uint32_t rnd = htonl(round);
    uint16_t ver = htons(NET_VERSION);
    uint16_t typ = htons(type);

    if (len < 0 || len > NET_MAX_PAYLOAD) return 0;
    if (g_net.txLen + NET_HEADER_SIZE + len > NET_TX_SIZE) return 0;
    memcpy(header + 0, &magic, 4);
    memcpy(header + 4, &ver, 2);
    memcpy(header + 6, &typ, 2);
    memcpy(header + 8, &plen, 4);
    memcpy(header + 12, &sess, 4);
    memcpy(header + 16, &rnd, 4);
    memcpy(g_net.tx + g_net.txLen, header, NET_HEADER_SIZE);
    g_net.txLen += NET_HEADER_SIZE;
    if (len > 0) {
        memcpy(g_net.tx + g_net.txLen, payload, (size_t)len);
        g_net.txLen += len;
    }
    return 1;
}

static int NetSendSimple(uint16_t type)
{
    return NetQueueMessage(type, NULL, 0, g_net.session, g_net.round);
}

static int NetSendReady(void)
{
    unsigned char ready[1];
    ready[0] = 1;
    return NetQueueMessage(NET_MSG_READY, ready, 1, g_net.session, g_net.round);
}

static int NetFlush(void)
{
    int total = 0;
    if (g_net.sock == INVALID_SOCKET) return 0;
    while (g_net.txLen > 0) {
        int n = send(g_net.sock, (const char *)g_net.tx, (int)g_net.txLen, 0);
        if (n > 0) {
            total += n;
            if (n < g_net.txLen)
                memmove(g_net.tx, g_net.tx + n, (size_t)(g_net.txLen - n));
            g_net.txLen -= n;
        } else if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) return total;
            return -1;
        } else {
            return -1;
        }
    }
    return total;
}

static void NetDetectLocalIp(void)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in target, self;
    int selfLen = sizeof(self);

    g_net.localIp[0] = L'\0';
    if (s == INVALID_SOCKET) return;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(9);
    target.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (struct sockaddr *)&target, sizeof(target)) == 0 &&
        getsockname(s, (struct sockaddr *)&self, &selfLen) == 0) {
        char ip[32];
        if (inet_ntop(AF_INET, &self.sin_addr, ip, sizeof(ip)) != NULL) {
            MultiByteToWideChar(CP_UTF8, 0, ip, -1, g_net.localIp, 64);
        }
    }
    closesocket(s);
}

static int NetStartHost(uint16_t port)
{
    struct sockaddr_in addr;
    WSADATA wsa;

    if (!g_wsaStarted) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        g_wsaStarted = 1;
    }
    NetResetState();
    srand((unsigned)time(NULL));
    g_net.sock = INVALID_SOCKET;
    g_net.listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_net.listenSock == INVALID_SOCKET) return 0;
    if (!NetSetNonBlocking(g_net.listenSock)) {
        NetCloseSocket(&g_net.listenSock);
        return 0;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(g_net.listenSock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(g_net.listenSock, 1) == SOCKET_ERROR) {
        NetCloseSocket(&g_net.listenSock);
        return 0;
    }
    g_net.invite = (uint32_t)(rand() % 1000000);
    g_net.port = port;
    g_net.session = ((uint32_t)rand() << 16) ^ (uint32_t)GetTickCount() ^ 0x13572468u;
    if (g_net.session == 0) g_net.session = 1;
    NetDetectLocalIp();
    g_netPhase = NET_WAIT_PEER;
    g_appMode = APP_HOST;
    NetSetStatus(L"房间已创建，等待对方加入");
    return 1;
}

static int NetStartJoin(const char *ip, uint16_t port, uint32_t invite)
{
    struct sockaddr_in addr;
    WSADATA wsa;
    int ret;

    if (!g_wsaStarted) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        g_wsaStarted = 1;
    }
    NetResetState();
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return 0;
    g_net.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_net.sock == INVALID_SOCKET) return 0;
    if (!NetSetNonBlocking(g_net.sock)) {
        NetCloseSocket(&g_net.sock);
        return 0;
    }
    ret = connect(g_net.sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        NetCloseSocket(&g_net.sock);
        return 0;
    }
    g_net.invite = invite;
    g_net.session = 0;
    g_net.connecting = (ret == SOCKET_ERROR);
    g_net.lastRecv = GetTickCount64();
    g_netPhase = NET_CONNECTING;
    g_appMode = APP_CLIENT;
    NetSetStatus(L"正在连接主机...");
    return 1;
}

static void NetRejectSocket(SOCKET s, unsigned char reason)
{
    unsigned char frame[NET_HEADER_SIZE + 1];
    uint32_t magic = htonl(NET_MAGIC);
    uint32_t plen = htonl(1);
    uint32_t zero = 0;
    uint16_t ver = htons(NET_VERSION);
    uint16_t typ = htons(NET_MSG_ERROR);
    int one = 1;

    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    memcpy(frame + 0, &magic, 4);
    memcpy(frame + 4, &ver, 2);
    memcpy(frame + 6, &typ, 2);
    memcpy(frame + 8, &plen, 4);
    memcpy(frame + 12, &zero, 4);
    memcpy(frame + 16, &zero, 4);
    frame[NET_HEADER_SIZE] = reason;
    send(s, (const char *)frame, sizeof(frame), 0); /* 尽力发送，随后立即关闭 */
    (void)NetSetNonBlocking(s);
    closesocket(s);
}

static int NetSendSnapshot(void)
{
    int len = EncodeSnapshot(g_net.snapshotBuf, NET_MAX_PAYLOAD);
    if (len <= 0) return 0;
    return NetQueueMessage(NET_MSG_SNAPSHOT, g_net.snapshotBuf, len,
                           g_net.session, g_net.round);
}

static int NetSendInput(uint32_t seq, uint32_t held, uint32_t pressed)
{
    NetWriter w = { g_net.snapshotBuf, NET_MAX_PAYLOAD, 0, 1 };
    NW_U32(&w, seq);
    NW_U32(&w, held);
    NW_U32(&w, pressed);
    if (!w.ok) return 0;
    return NetQueueMessage(NET_MSG_INPUT, g_net.snapshotBuf, w.pos,
                           g_net.session, g_net.round);
}

static uint32_t NetReadOnlineKeys(void)
{
    if (GetForegroundWindow() == g_mainWnd) return ReadP1Keys();
    return 0;
}

static void NetTryStartRound(void)
{
    if (!g_net.localReady || !g_net.remoteReady) return;
    g_net.round++;
    g_net.tick = 0;
    g_net.ackInputSeq = 0;
    g_net.remoteInputSeq = 0;
    g_net.remoteHeld = 0;
    g_net.remotePressed = 0;
    g_net.localReady = 0;
    g_net.remoteReady = 0;
    g_net.lastSnapshotTick = 0;
    ResetGame();
    g_netPhase = NET_RUNNING;
    NetSetStatus(L"比赛开始");
    NetSendSimple(NET_MSG_START);
    NetSendSnapshot();
}

static void NetEnterDisconnected(const wchar_t *reason)
{
    NetCloseSocket(&g_net.sock);
    NetCloseSocket(&g_net.listenSock);
    g_netPhase = NET_DISCONNECTED;
    NetSetStatus(reason);
    if (g_netQuiet) return;
    MessageBoxW(g_mainWnd, reason, L"局域网联机已断开", MB_OK | MB_ICONINFORMATION);
    g_appMode = APP_MENU;
    g_netPhase = NET_NONE;
    NetResetState();
    ShowWindow(g_mainWnd, SW_HIDE);
    CreateMenuWindow();
}

static void NetHandleMessage(uint16_t type, const unsigned char *payload, int len,
                             uint32_t session, uint32_t round)
{
    if (type == NET_MSG_HELLO) {
        uint32_t invite;
        NetReader r = { payload, len, 0, 1 };
        unsigned char okPayload[5];
        NetWriter w = { okPayload, sizeof(okPayload), 0, 1 };

        if (g_appMode != APP_HOST || g_netPhase != NET_WAIT_PEER ||
            session != 0 || round != 0 || len != 4) {
            NetEnterDisconnected(L"收到无效的连接请求");
            return;
        }
        invite = NR_U32(&r);
        if (!r.ok || invite != g_net.invite) {
            NetRejectSocket(g_net.sock, 1);
            g_net.sock = INVALID_SOCKET;
            NetSetStatus(L"邀请码错误，继续等待连接");
            return;
        }
        g_net.session = g_net.session ? g_net.session : 1;
        NW_U32(&w, g_net.session);
        NW_U8(&w, 1);
        if (!w.ok || !NetQueueMessage(NET_MSG_WELCOME, okPayload, w.pos, g_net.session, 0)) {
            NetEnterDisconnected(L"无法发送握手确认");
            return;
        }
        NetCloseSocket(&g_net.listenSock);
        g_net.handshakeDone = 1;
        g_net.lastRecv = GetTickCount64();
        g_netPhase = NET_LOBBY;
        NetSetStatus(L"对方已加入，等待双方准备");
        return;
    }

    if (type == NET_MSG_WELCOME) {
        NetReader r = { payload, len, 0, 1 };
        uint32_t sess;
        uint8_t role;
        if (g_appMode != APP_CLIENT || g_netPhase == NET_RUNNING ||
            len != 5 || round != 0) {
            NetEnterDisconnected(L"主机返回了无效握手");
            return;
        }
        sess = NR_U32(&r);
        role = NR_U8(&r);
        if (!r.ok || sess == 0 || role != 1 || session != sess) {
            NetEnterDisconnected(L"主机拒绝了连接");
            return;
        }
        g_net.session = sess;
        g_net.handshakeDone = 1;
        g_net.lastRecv = GetTickCount64();
        g_netPhase = NET_LOBBY;
        NetSetStatus(L"连接成功，你是蓝方，按 R 准备");
        return;
    }

    /* START 是唯一允许 round 前进的消息：客户端据此进入新回合。 */
    if (type == NET_MSG_START && g_appMode == APP_CLIENT && len == 0 &&
        session == g_net.session && round == g_net.round + 1) {
        g_net.round = round;
    }

    if (session != g_net.session || round != g_net.round) {
        NetEnterDisconnected(L"收到过期或无效的联机数据");
        return;
    }

    if (type == NET_MSG_ERROR) {
        const wchar_t *reason = L"主机返回错误，连接已关闭";
        if (len == 1 && payload[0] == 1) reason = L"邀请码错误，连接被主机拒绝";
        if (len == 1 && payload[0] == 2) reason = L"房间已有玩家，连接被主机拒绝";
        NetEnterDisconnected(reason);
        return;
    }
    if (type == NET_MSG_LEAVE) {
        NetEnterDisconnected(L"对方已退出房间");
        return;
    }
    if (type == NET_MSG_READY) {
        if (len != 1) {
            NetEnterDisconnected(L"准备状态数据无效");
            return;
        }
        if (g_appMode == APP_HOST) {
            g_net.remoteReady = payload[0] ? 1 : 0;
            if (g_net.remoteReady) NetSetStatus(L"对方已准备，按 R 开始");
            NetTryStartRound();
        }
        return;
    }
    if (type == NET_MSG_LOBBY) {
        if (g_appMode == APP_CLIENT && len == 2) {
            g_net.remoteReady = payload[1] ? 1 : 0;
            if (g_net.remoteReady && !g_net.localReady)
                NetSetStatus(L"主机已准备，按 R 开始");
        }
        return;
    }
    if (type == NET_MSG_START) {
        if (g_appMode == APP_CLIENT && len == 0) {
            ResetGame();
            g_net.lastSnapshotTick = 0;
            g_net.remoteReady = 0;
            g_net.localReady = 0;
            g_netPhase = NET_RUNNING;
            NetSetStatus(L"比赛开始");
        }
        return;
    }
    if (type == NET_MSG_INPUT) {
        NetReader r = { payload, len, 0, 1 };
        uint32_t seq, held, pressed;
        if (g_appMode != APP_HOST || g_netPhase != NET_RUNNING || len != 12) {
            NetEnterDisconnected(L"收到无效的输入数据");
            return;
        }
        seq = NR_U32(&r);
        held = NR_U32(&r);
        pressed = NR_U32(&r);
        if (!r.ok || (held & ~0x7fu) || (pressed & ~0x7fu) ||
            seq == 0 || seq <= g_net.remoteInputSeq) return;
        g_net.remoteInputSeq = seq;
        g_net.remoteHeld = held;
        g_net.remotePressed |= pressed;
        return;
    }
    if (type == NET_MSG_SNAPSHOT) {
        uint32_t tick;
        if (g_appMode != APP_CLIENT || g_netPhase != NET_RUNNING) return;
        if (len < 4) {
            NetEnterDisconnected(L"快照数据无效");
            return;
        }
        tick = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
               ((uint32_t)payload[2] << 8) | (uint32_t)payload[3];
        if (tick <= g_net.lastSnapshotTick) return;
        if (!DecodeSnapshot(payload, len)) {
            NetEnterDisconnected(L"快照校验失败");
            return;
        }
        ApplySnapshot();
        return;
    }
    if (type == NET_MSG_PING) {
        NetSendSimple(NET_MSG_PONG);
        return;
    }
    (void)type;
}

static int NetParseFrames(void)
{
    while (g_net.rxLen >= NET_HEADER_SIZE) {
        uint32_t magic, plen, session, round;
        uint16_t version, type;
        int frameLen;
        const unsigned char *p = g_net.rx;

        memcpy(&magic, p + 0, 4); magic = ntohl(magic);
        memcpy(&version, p + 4, 2); version = ntohs(version);
        memcpy(&type, p + 6, 2); type = ntohs(type);
        memcpy(&plen, p + 8, 4); plen = ntohl(plen);
        memcpy(&session, p + 12, 4); session = ntohl(session);
        memcpy(&round, p + 16, 4); round = ntohl(round);
        if (magic != NET_MAGIC || version != NET_VERSION || plen > NET_MAX_PAYLOAD) {
            NetEnterDisconnected(L"联机协议数据无效");
            return 0;
        }
        frameLen = NET_HEADER_SIZE + (int)plen;
        if (g_net.rxLen < frameLen) break;
        NetHandleMessage(type, g_net.rx + NET_HEADER_SIZE, (int)plen, session, round);
        if (g_netPhase == NET_DISCONNECTED || g_net.sock == INVALID_SOCKET) return 0;
        memmove(g_net.rx, g_net.rx + frameLen, (size_t)(g_net.rxLen - frameLen));
        g_net.rxLen -= frameLen;
    }
    return 1;
}

static void NetPoll(void)
{
    ULONGLONG now = GetTickCount64();
    int r;

    if (g_appMode != APP_HOST && g_appMode != APP_CLIENT) return;
    if (g_netPhase == NET_DISCONNECTED || g_netPhase == NET_NONE) return;

    if (g_net.listenSock != INVALID_SOCKET) {
        for (;;) {
            SOCKET s = accept(g_net.listenSock, NULL, NULL);
            if (s == INVALID_SOCKET) {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) NetEnterDisconnected(L"监听房间时出现错误");
                break;
            }
            if (g_net.sock != INVALID_SOCKET) {
                NetRejectSocket(s, 2);
                continue;
            }
            g_net.sock = s;
            if (!NetSetNonBlocking(g_net.sock)) {
                NetRejectSocket(g_net.sock, 2);
                g_net.sock = INVALID_SOCKET;
                continue;
            }
            {
                int one = 1;
                setsockopt(g_net.sock, IPPROTO_TCP, TCP_NODELAY,
                           (const char *)&one, sizeof(one));
            }
            g_net.handshakeDone = 0;
            g_net.lastRecv = now;
            NetSetStatus(L"对方已连接，正在校验邀请码");
        }
    }

    if (g_net.sock != INVALID_SOCKET && g_net.connecting) {
        fd_set writeSet, exceptSet;
        struct timeval tv = { 0, 0 };
        FD_ZERO(&writeSet);
        FD_ZERO(&exceptSet);
        FD_SET(g_net.sock, &writeSet);
        FD_SET(g_net.sock, &exceptSet);
        if (select(0, NULL, &writeSet, &exceptSet, &tv) > 0) {
            int err = 0, errLen = sizeof(err);
            if (FD_ISSET(g_net.sock, &exceptSet)) {
                NetEnterDisconnected(L"无法连接主机");
                return;
            }
            if (FD_ISSET(g_net.sock, &writeSet) &&
                getsockopt(g_net.sock, SOL_SOCKET, SO_ERROR, (char *)&err, &errLen) == 0 &&
                err == 0) {
                unsigned char hello[4];
                NetWriter w = { hello, sizeof(hello), 0, 1 };
                g_net.connecting = 0;
                g_net.lastRecv = now;
                NW_U32(&w, g_net.invite);
                if (!w.ok || !NetQueueMessage(NET_MSG_HELLO, hello, 4, 0, 0)) {
                    NetEnterDisconnected(L"无法发送加入请求");
                    return;
                }
                g_netPhase = NET_HANDSHAKE;
                NetSetStatus(L"已连接主机，正在校验邀请码");
            }
        }
    }

    if (g_net.sock != INVALID_SOCKET) {
        for (;;) {
            int space = NET_RX_SIZE - g_net.rxLen;
            int n;
            if (space <= 0) {
                NetEnterDisconnected(L"接收数据过多，连接已关闭");
                return;
            }
            n = recv(g_net.sock, (char *)g_net.rx + g_net.rxLen, space, 0);
            if (n > 0) {
                g_net.rxLen += n;
                g_net.lastRecv = now;
            } else if (n == 0) {
                NetEnterDisconnected(L"对方已断开连接");
                return;
            } else {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    NetEnterDisconnected(L"网络连接出现错误");
                    return;
                }
                break;
            }
        }
        if (!NetParseFrames()) return;
    }

    if (g_appMode == APP_CLIENT && g_netPhase == NET_CONNECTING &&
        now - g_net.lastRecv > NET_TIMEOUT_MS) {
        NetEnterDisconnected(L"连接主机超时");
        return;
    }
    if (g_net.sock != INVALID_SOCKET && g_netPhase != NET_WAIT_PEER &&
        now - g_net.lastRecv > NET_TIMEOUT_MS) {
        NetEnterDisconnected(L"联机通信超时");
        return;
    }

    if (g_netPhase == NET_LOBBY) {
        r = KeyDown('R');
        if (r && !g_prevR) {
            g_net.localReady = 1;
            if (g_appMode == APP_CLIENT) NetSendReady();
            NetSetStatus(g_appMode == APP_HOST ? L"你已准备，等待对方..." : L"你已准备，等待主机...");
        }
        g_prevR = r;

        if (g_appMode == APP_HOST && now - g_net.lastLobbySend > 100) {
            unsigned char lobby[2];
            lobby[0] = (unsigned char)g_net.localReady;
            lobby[1] = (unsigned char)g_net.remoteReady;
            NetQueueMessage(NET_MSG_LOBBY, lobby, 2, g_net.session, g_net.round);
            g_net.lastLobbySend = now;
            NetTryStartRound();
        }
        if (g_appMode == APP_CLIENT && now - g_net.lastLobbySend > 1000) {
            NetSendSimple(NET_MSG_PING);
            g_net.lastLobbySend = now;
        }
    } else if (g_netPhase == NET_RUNNING && g_state == 1) {
        r = KeyDown('R');
        if (r && !g_prevR) {
            g_net.localReady = 1;
            if (g_appMode == APP_CLIENT) NetSendReady();
            NetSetStatus(L"你已准备重赛，等待对方...");
        }
        g_prevR = r;
        if (g_appMode == APP_HOST)
            NetTryStartRound();
    }

    if (NetFlush() < 0 && g_netPhase != NET_DISCONNECTED) {
        NetEnterDisconnected(L"发送联机数据失败");
    }
}

static void UpdateHostGame(float dt)
{
    PlayerInput in[2];
    in[0] = MakeInput(0, NetReadOnlineKeys());
    in[1].held = g_net.remoteHeld;
    in[1].pressed = g_net.remotePressed;
    g_net.remotePressed = 0;
    g_net.ackInputSeq = g_net.remoteInputSeq;
    SimulateTick(in, dt);
    g_net.tick++;
    NetSendSnapshot();
}

static void UpdateClientInput(float dt)
{
    uint32_t keys = NetReadOnlineKeys();
    uint32_t pressed = keys & ~g_prevKeys[1];
    (void)dt;
    g_prevKeys[1] = keys;
    g_net.inputSeq++;
    if (!NetSendInput(g_net.inputSeq, keys, pressed))
        NetEnterDisconnected(L"网络缓冲已满，连接关闭");
}

static void NetLeaveToMenu(void)
{
    if (g_net.sock != INVALID_SOCKET) {
        NetSendSimple(NET_MSG_LEAVE);
        NetFlush();
    }
    NetResetState();
    g_appMode = APP_MENU;
    g_netPhase = NET_NONE;
    ShowWindow(g_mainWnd, SW_HIDE);
    CreateMenuWindow();
}

/* ---------------------------- 启动模式窗口 ---------------------------- */

#define MENU_BTN_LOCAL 101
#define MENU_BTN_HOST  102
#define MENU_BTN_JOIN  103
#define MENU_BTN_EXIT  104
#define MENU_BTN_HOST_START 105
#define MENU_BTN_JOIN_START 106
#define MENU_BTN_BACK 107

static HWND g_menuWnd;
static HWND g_menuTitle, g_menuLocal, g_menuHost, g_menuJoin, g_menuExit;
static HWND g_hostPage, g_hostPortLabel, g_hostPortEdit, g_hostStart, g_hostBack;
static HWND g_joinPage, g_joinIpLabel, g_joinIpEdit, g_joinPortLabel, g_joinPortEdit;
static HWND g_joinInviteLabel, g_joinInviteEdit, g_joinStart, g_joinBack;
static int g_menuPage = 0;
static int g_menuChoice = 0;
static int g_menuClassRegistered = 0;

static void MenuSetPage(int page)
{
    int menu = (page == 0);
    int host = (page == 1);
    int join = (page == 2);
    g_menuPage = page;
    ShowWindow(g_menuTitle, menu ? SW_SHOW : SW_HIDE);
    ShowWindow(g_menuLocal, menu ? SW_SHOW : SW_HIDE);
    ShowWindow(g_menuHost, menu ? SW_SHOW : SW_HIDE);
    ShowWindow(g_menuJoin, menu ? SW_SHOW : SW_HIDE);
    ShowWindow(g_menuExit, menu ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hostPage, host ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hostPortLabel, host ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hostPortEdit, host ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hostStart, host ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hostBack, host ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinPage, join ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinIpLabel, join ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinIpEdit, join ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinPortLabel, join ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinPortEdit, join ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinInviteLabel, join ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinInviteEdit, join ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinStart, join ? SW_SHOW : SW_HIDE);
    ShowWindow(g_joinBack, join ? SW_SHOW : SW_HIDE);
}

static void MenuFinishChoice(void)
{
    g_menuChoice = 1;
    if (g_menuWnd) DestroyWindow(g_menuWnd);
}

static int MenuReadPort(HWND edit, uint16_t *port)
{
    wchar_t text[32];
    int value;
    GetWindowTextW(edit, text, 32);
    if (swscanf(text, L"%d", &value) != 1 || value < 1 || value > 65535) return 0;
    *port = (uint16_t)value;
    return 1;
}

static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == MENU_BTN_LOCAL) {
            g_appMode = APP_LOCAL;
            g_netPhase = NET_NONE;
            ResetGame();
            MenuFinishChoice();
            ShowWindow(g_mainWnd, SW_SHOW);
            SetForegroundWindow(g_mainWnd);
        } else if (id == MENU_BTN_HOST) {
            MenuSetPage(1);
        } else if (id == MENU_BTN_JOIN) {
            MenuSetPage(2);
        } else if (id == MENU_BTN_EXIT) {
            PostQuitMessage(0);
        } else if (id == MENU_BTN_HOST_START) {
            uint16_t port;
            if (!MenuReadPort(g_hostPortEdit, &port)) {
                MessageBoxW(hwnd, L"端口必须是 1~65535 的数字", L"输入错误", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!NetStartHost(port)) {
                MessageBoxW(hwnd, L"创建房间失败：端口可能被占用或无权限", L"创建失败", MB_OK | MB_ICONWARNING);
                return 0;
            }
            MenuFinishChoice();
            ShowWindow(g_mainWnd, SW_SHOW);
            SetForegroundWindow(g_mainWnd);
        } else if (id == MENU_BTN_JOIN_START) {
            char ip[32];
            uint16_t port;
            wchar_t inviteText[16];
            int invite = -1;
            GetWindowTextA(g_joinIpEdit, ip, 32);
            if (!MenuReadPort(g_joinPortEdit, &port)) {
                MessageBoxW(hwnd, L"端口必须是 1~65535 的数字", L"输入错误", MB_OK | MB_ICONWARNING);
                return 0;
            }
            GetWindowTextW(g_joinInviteEdit, inviteText, 16);
            if (swscanf(inviteText, L"%d", &invite) != 1 || invite < 0 || invite > 999999 ||
                (int)wcslen(inviteText) != 6) {
                MessageBoxW(hwnd, L"邀请码必须是 6 位数字", L"输入错误", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!NetStartJoin(ip, port, (uint32_t)invite)) {
                MessageBoxW(hwnd, L"无法开始连接：请检查 IP 地址是否有效", L"加入失败", MB_OK | MB_ICONWARNING);
                return 0;
            }
            MenuFinishChoice();
            ShowWindow(g_mainWnd, SW_SHOW);
            SetForegroundWindow(g_mainWnd);
        } else if (id == MENU_BTN_BACK) {
            MenuSetPage(0);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (!g_menuChoice) PostQuitMessage(0);
        g_menuWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int CreateMenuWindow(void)
{
    WNDCLASSEXW wc;
    RECT rc;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HFONT font;

    if (!g_menuClassRegistered) {
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MenuWndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"TankBattleMenuWnd";
        if (!RegisterClassExW(&wc)) return 0;
        g_menuClassRegistered = 1;
    }

    rc.left = 0; rc.top = 0; rc.right = 430; rc.bottom = 330;
    AdjustWindowRect(&rc, style, FALSE);
    g_menuChoice = 0;
    g_menuWnd = CreateWindowExW(0, L"TankBattleMenuWnd", L"坦克大战 · 选择模式",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, g_hInst, NULL);
    if (!g_menuWnd) return 0;

    g_menuTitle = CreateWindowW(L"STATIC", L"请选择游戏模式",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 15, 22, 400, 26, g_menuWnd, NULL, g_hInst, NULL);
    g_menuLocal = CreateWindowW(L"BUTTON", L"本地双人（同一键盘）",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 75, 270, 38, g_menuWnd,
        (HMENU)MENU_BTN_LOCAL, g_hInst, NULL);
    g_menuHost = CreateWindowW(L"BUTTON", L"创建局域网房间（红方 P1）",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 125, 270, 38, g_menuWnd,
        (HMENU)MENU_BTN_HOST, g_hInst, NULL);
    g_menuJoin = CreateWindowW(L"BUTTON", L"加入局域网房间（蓝方 P2）",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 175, 270, 38, g_menuWnd,
        (HMENU)MENU_BTN_JOIN, g_hInst, NULL);
    g_menuExit = CreateWindowW(L"BUTTON", L"退出游戏",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 235, 270, 34, g_menuWnd,
        (HMENU)MENU_BTN_EXIT, g_hInst, NULL);

    g_hostPage = CreateWindowW(L"STATIC", L"创建房间：设置监听端口",
        WS_CHILD | SS_CENTER, 15, 20, 400, 22, g_menuWnd, NULL, g_hInst, NULL);
    g_hostPortLabel = CreateWindowW(L"STATIC", L"端口：",
        WS_CHILD, 60, 70, 50, 20, g_menuWnd, NULL, g_hInst, NULL);
    g_hostPortEdit = CreateWindowW(L"EDIT", L"27515",
        WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 110, 66, 110, 24,
        g_menuWnd, NULL, g_hInst, NULL);
    g_hostStart = CreateWindowW(L"BUTTON", L"开始创建",
        WS_CHILD | BS_PUSHBUTTON, 105, 120, 210, 36, g_menuWnd,
        (HMENU)MENU_BTN_HOST_START, g_hInst, NULL);
    g_hostBack = CreateWindowW(L"BUTTON", L"返回",
        WS_CHILD | BS_PUSHBUTTON, 105, 180, 210, 32, g_menuWnd,
        (HMENU)MENU_BTN_BACK, g_hInst, NULL);

    g_joinPage = CreateWindowW(L"STATIC", L"加入房间：输入主机信息",
        WS_CHILD | SS_CENTER, 15, 20, 400, 22, g_menuWnd, NULL, g_hInst, NULL);
    g_joinIpLabel = CreateWindowW(L"STATIC", L"主机 IP：",
        WS_CHILD, 35, 70, 70, 20, g_menuWnd, NULL, g_hInst, NULL);
    g_joinIpEdit = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 105, 66, 180, 24, g_menuWnd, NULL, g_hInst, NULL);
    g_joinPortLabel = CreateWindowW(L"STATIC", L"端口：",
        WS_CHILD, 35, 108, 70, 20, g_menuWnd, NULL, g_hInst, NULL);
    g_joinPortEdit = CreateWindowW(L"EDIT", L"27515",
        WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 105, 104, 180, 24,
        g_menuWnd, NULL, g_hInst, NULL);
    g_joinInviteLabel = CreateWindowW(L"STATIC", L"邀请码：",
        WS_CHILD, 35, 146, 70, 20, g_menuWnd, NULL, g_hInst, NULL);
    g_joinInviteEdit = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 105, 142, 180, 24,
        g_menuWnd, NULL, g_hInst, NULL);
    g_joinStart = CreateWindowW(L"BUTTON", L"连接并加入",
        WS_CHILD | BS_PUSHBUTTON, 105, 200, 210, 36, g_menuWnd,
        (HMENU)MENU_BTN_JOIN_START, g_hInst, NULL);
    g_joinBack = CreateWindowW(L"BUTTON", L"返回",
        WS_CHILD | BS_PUSHBUTTON, 105, 258, 210, 32, g_menuWnd,
        (HMENU)MENU_BTN_BACK, g_hInst, NULL);

    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    {
        HWND controls[] = { g_menuTitle, g_menuLocal, g_menuHost, g_menuJoin, g_menuExit,
                            g_hostPage, g_hostPortLabel, g_hostPortEdit, g_hostStart, g_hostBack,
                            g_joinPage, g_joinIpLabel, g_joinIpEdit, g_joinPortLabel,
                            g_joinPortEdit, g_joinInviteLabel, g_joinInviteEdit,
                            g_joinStart, g_joinBack };
        int i;
        for (i = 0; i < (int)(sizeof(controls) / sizeof(controls[0])); i++)
            if (controls[i]) SendMessageW(controls[i], WM_SETFONT, (WPARAM)font, TRUE);
    }

    MenuSetPage(0);
    ShowWindow(g_menuWnd, SW_SHOW);
    UpdateWindow(g_menuWnd);
    return 1;
}

/* ================================ 绘制 ================================== */
/*  绘制部分完全“只读”：所有 Draw* 函数都不修改游戏状态，只把
 *  g_map / g_tanks / g_bullets 翻译成像素。
 *  想改画面层级，改最下面的 Render() 调用顺序即可。                */

static int TileIsWall(int cx, int cy);   /* 前置声明：下面画墙时要用 */

/* 钢墙：整格铺满，然后只在“朝向外面的那条边”做高光 / 暗边。
 * 为什么要看邻居：连成一片的墙如果每格都描边，看起来就像一堆散砖；
 * 只在贴地的边缘描边，整片墙才会融成一整块体积。                    */
static void DrawWallTile(HDC dc, int px, int py, int gx, int gy)
{
    FillBox(dc, px, py, TILE, TILE, C_WALL);
    FillBox(dc, px + 2, py + 2, TILE - 4, TILE - 4, Blend(C_WALL, RGB(0, 0, 0), 0.08f));

    if (!TileIsWall(gx, gy - 1)) {                    /* 上边受光 */
        FillBox(dc, px, py, TILE, 3, C_WALL_HI);
        FillBox(dc, px, py, 2, TILE, C_WALL_HI);
    }
    if (!TileIsWall(gx, gy + 1)) {                    /* 下边背光 */
        FillBox(dc, px, py + TILE - 3, TILE, 3, C_WALL_LO);
    }
    if (!TileIsWall(gx - 1, gy)) FillBox(dc, px, py, 2, TILE, Blend(C_WALL_HI, C_WALL, 0.4f));
    if (!TileIsWall(gx + 1, gy)) FillBox(dc, px + TILE - 2, py, 2, TILE, C_WALL_LO);
}

/* ---- 有向几何：把“车体坐标系”里的点换算成屏幕坐标 ----
 * 车体坐标系：f = 沿车头向前的距离，l = 向车身右侧的距离（左负右正）。
 * 有了这两个函数，坦克的每个零件都可以只按“车身朝上”描述一次，
 * 无论车头朝哪个角度，都能自动转到正确位置和角度。
 * 这比旋转整个 GDI 画布可靠：不用改 DC 的世界变换，也不担心
 * 某些绘图函数不认变换。                                        */
static void OrientedPoint(POINT *p, int cx, int cy, float a, float f, float l)
{
    float fx = sinf(a), fy = -cosf(a);   /* 车头方向 */
    float rx = cosf(a), ry = sinf(a);    /* 车体右侧方向 */
    p->x = (LONG)(cx + fx * f + rx * l + 0.5f);
    p->y = (LONG)(cy + fy * f + ry * l + 0.5f);
}

/* 有向矩形：沿车头 f0~f1、横向 l0~l1 的四个角 */
static void OrientedBox(POINT *p, int cx, int cy, float a,
                        float f0, float f1, float l0, float l1)
{
    OrientedPoint(&p[0], cx, cy, a, f0, l0);
    OrientedPoint(&p[1], cx, cy, a, f1, l0);
    OrientedPoint(&p[2], cx, cy, a, f1, l1);
    OrientedPoint(&p[3], cx, cy, a, f0, l1);
}

/* 填充多边形并描边（一笔画完，省得反复建画刷） */
static void FillPoly(HDC dc, const POINT *p, int n, COLORREF fill, COLORREF line2)
{
    HBRUSH  br = CreateSolidBrush(fill);
    HPEN    pn = CreatePen(PS_SOLID, 1, line2);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    Polygon(dc, p, n);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

/* 画一辆坦克，从下往上叠：炮管 → 车体 → 履带 → 炮塔 → 装饰。
 * 所有零件都按“车头朝上”描述，再由 OrientedPoint 转到 t->angle，
 * 所以车体可以停在任意角度（斜着开、正在打方向的过程都能正确显示）。 */
static void DrawTank(HDC dc, const Tank *t)
{
    int cx, cy, i;
    float h = TANK_HALF;
    float a;
    COLORREF dark;
    POINT barrel[4], tread[4], body[8];
    HBRUSH br;
    HPEN pen;
    HGDIOBJ ob, op;

    if (!t->alive) return;      /* 死了就不画（重生标记由别的函数负责） */

    /* 场地坐标 → 屏幕坐标：加 ARENA_X / ARENA_Y 偏移并四舍五入 */
    cx = ARENA_X + (int)(t->pos.x + 0.5f);
    cy = ARENA_Y + (int)(t->pos.y + 0.5f);
    a  = t->angle;
    dark = Blend(t->color, RGB(0, 0, 0), 0.62f);

    /* 炮管：沿车头方向伸出车体（f 从 -2 到 18），先深色轮廓再亮色内芯，
     * 末端再加一个更亮的炮口点 —— 车体可以任意角度，朝向必须一眼看清。 */
    OrientedBox(barrel, cx, cy, a, -2.0f, 18.0f, -4.0f, 4.0f);
    FillPoly(dc, barrel, 4, Blend(t->color, RGB(0, 0, 0), 0.42f),
             Blend(t->color, RGB(0, 0, 0), 0.42f));
    OrientedBox(barrel, cx, cy, a, -1.0f, 17.0f, -2.6f, 2.6f);
    FillPoly(dc, barrel, 4, Blend(t->color, RGB(255, 255, 255), 0.34f),
             Blend(t->color, RGB(255, 255, 255), 0.34f));
    OrientedBox(barrel, cx, cy, a, 14.0f, 18.0f, -2.6f, 2.6f);
    FillPoly(dc, barrel, 4, Blend(t->color, RGB(255, 255, 255), 0.78f),
             Blend(t->color, RGB(255, 255, 255), 0.78f));

    /* 车体：八角形（四角切掉 3px），比纯方块更像俯视的车身 */
    OrientedPoint(&body[0], cx, cy, a, -h + 3, -h);
    OrientedPoint(&body[1], cx, cy, a,  h - 3, -h);
    OrientedPoint(&body[2], cx, cy, a,  h, -h + 3);
    OrientedPoint(&body[3], cx, cy, a,  h,  h - 3);
    OrientedPoint(&body[4], cx, cy, a,  h - 3,  h);
    OrientedPoint(&body[5], cx, cy, a, -h + 3,  h);
    OrientedPoint(&body[6], cx, cy, a, -h,  h - 3);
    OrientedPoint(&body[7], cx, cy, a, -h, -h + 3);
    FillPoly(dc, body, 8, t->color, Blend(t->color, RGB(0, 0, 0), 0.45f));

    /* 履带：贴着车体左右两侧的两条深色长条 */
    OrientedBox(tread, cx, cy, a, -h + 1, h - 1, -h, -h + 3.5f);
    FillPoly(dc, tread, 4, dark, dark);
    OrientedBox(tread, cx, cy, a, -h + 1, h - 1, h - 3.5f, h);
    FillPoly(dc, tread, 4, dark, dark);

    /* 炮塔：比车体亮一点的圆（圆旋转后还是圆，所以不用管角度） */
    br = CreateSolidBrush(Blend(t->color, RGB(255, 255, 255), 0.30f));
    pen = CreatePen(PS_SOLID, 1, Blend(t->color, RGB(255, 255, 255), 0.12f));
    ob = SelectObject(dc, br);
    op = SelectObject(dc, pen);
    Ellipse(dc, cx - 4, cy - 4, cx + 5, cy + 5);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);

    /* 被击中白闪：把车体形状整个涂白，持续 0.12 秒 */
    {
        int idx = (t == &g_tanks[0]) ? 0 : 1;
        if (g_flash[idx] > 0.0f)
            FillPoly(dc, body, 8, RGB(255, 255, 255), RGB(255, 255, 255));
    }

    /* 重生护盾：只描边不填充的圆（NULL_BRUSH） */
    if (t->shield > 0.0f) {
        HPEN sp = CreatePen(PS_SOLID, 1, Blend(t->color, C_FLOOR, 0.25f));
        HGDIOBJ op2 = SelectObject(dc, sp);
        HGDIOBJ ob2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - (int)h - 4, cy - (int)h - 4, cx + (int)h + 5, cy + (int)h + 5);
        SelectObject(dc, op2);
        SelectObject(dc, ob2);
        DeleteObject(sp);
    }

    /* 车顶血条：3 个小格子，扣血后才显示（满血时不挡视线）。
     * 血条始终横平竖直，不跟着车体转，方便一眼读数。
     * 底下垫一块暗板，否则在深色地面上几根亮条会看着像多出来的炮管。 */
    if (t->hp < MAX_HP) {
        FillBox(dc, cx - 11, cy - (int)h - 8, 22, 5,
                Blend(C_FLOOR, RGB(0, 0, 0), 0.45f));
        for (i = 0; i < MAX_HP; i++)
            FillBox(dc, cx - 10 + i * 7, cy - (int)h - 7, 6, 3,
                    (i < t->hp) ? t->color : Blend(C_FLOOR, C_WALL, 0.35f));
    }
}

/* 砖墙：绿色砖块，血量越少颜色越暗、裂纹越多（4 血 → 无裂纹，1 血 → 3 道裂纹）。
 * 砖缝用“两行错缝砌法”：中间一条横缝，上下两排竖缝故意错开半块。 */
static void DrawBrickTile(HDC dc, int px, int py, int gx, int gy, int hp)
{
    COLORREF base = C_BRICK;
    int dmg = BRICK_HP - hp;      /* 挨了几发：0(满血) ~ 3 */
    int lvl, cracks;
    HPEN pen;
    HGDIOBJ old;

    if (dmg > 0) base = Blend(C_BRICK, RGB(0, 0, 0), 0.10f * (float)dmg);

    FillBox(dc, px, py, TILE, TILE, base);

    /* 砖缝（两行错缝砌法） */
    FillBox(dc, px, py + TILE / 2 - 1, TILE, 2, C_BRICK_LO);
    FillBox(dc, px + TILE / 2 - 1, py, 2, TILE / 2 - 1, C_BRICK_LO);
    FillBox(dc, px + TILE / 4 - 1, py + TILE / 2 + 1, 2, TILE / 2 - 1, C_BRICK_LO);
    FillBox(dc, px + TILE * 3 / 4 - 1, py + TILE / 2 + 1, 2, TILE / 2 - 1, C_BRICK_LO);

    /* 受光边：和钢墙同一套规则——只给贴着地面的边加高光 */
    if (!TileIsWall(gx, gy - 1)) FillBox(dc, px, py, TILE, 2, C_BRICK_HI);
    if (!TileIsWall(gx, gy + 1)) FillBox(dc, px, py + TILE - 3, TILE, 3, C_BRICK_LO);
    if (!TileIsWall(gx - 1, gy)) FillBox(dc, px, py, 2, TILE, Blend(C_BRICK_HI, C_BRICK, 0.4f));
    if (!TileIsWall(gx + 1, gy)) FillBox(dc, px + TILE - 2, py, 2, TILE, C_BRICK_LO);

    /* 裂纹：每条都是一段折线，最多 3 条，坐标写死（保证同一块砖的裂纹稳定） */
    cracks = dmg;
    if (cracks > 3) cracks = 3;
    if (cracks > 0) {
        pen = CreatePen(PS_SOLID, 1, C_BRICK_CRK);
        old = SelectObject(dc, pen);
        for (lvl = 0; lvl < cracks; lvl++) {
            if (lvl == 0) {
                MoveToEx(dc, px + 6, py + 3, NULL);
                LineTo(dc, px + 11, py + 11);
                LineTo(dc, px + 8, py + 18);
            } else if (lvl == 1) {
                MoveToEx(dc, px + 18, py + 4, NULL);
                LineTo(dc, px + 13, py + 12);
                LineTo(dc, px + 19, py + 20);
            } else {
                MoveToEx(dc, px + 4, py + 21, NULL);
                LineTo(dc, px + 12, py + 15);
                LineTo(dc, px + 21, py + 22);
            }
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

/* 画场地：地面 → 网格线 → 逐格分派给钢墙 / 砖墙。
 * 地图是二维数组，遍历一遍就够；31×22=682 格，每帧全画也没压力。 */
static void DrawArena(HDC dc)
{
    int gx, gy;
    HPEN grid;
    HGDIOBJ old;

    FillBox(dc, ARENA_X, ARENA_Y, MAP_W * TILE, MAP_H * TILE, C_FLOOR);

    /* 网格线：0~MAP_W 共 26 条竖线、20 条横线 */
    grid = CreatePen(PS_SOLID, 1, C_GRID);
    old = SelectObject(dc, grid);
    for (gx = 0; gx <= MAP_W; gx++) {
        MoveToEx(dc, ARENA_X + gx * TILE, ARENA_Y, NULL);
        LineTo(dc, ARENA_X + gx * TILE, ARENA_Y + MAP_H * TILE);
    }
    for (gy = 0; gy <= MAP_H; gy++) {
        MoveToEx(dc, ARENA_X, ARENA_Y + gy * TILE, NULL);
        LineTo(dc, ARENA_X + MAP_W * TILE, ARENA_Y + gy * TILE);
    }
    SelectObject(dc, old);
    DeleteObject(grid);

    /* 一个字符决定画什么，砖墙还要把剩余血量一起传进去 */
    for (gy = 0; gy < MAP_H; gy++)
        for (gx = 0; gx < MAP_W; gx++) {
            char c = g_map[gy][gx];
            if (c == '#')
                DrawWallTile(dc, ARENA_X + gx * TILE, ARENA_Y + gy * TILE, gx, gy);
            else if (c == 'B')
                DrawBrickTile(dc, ARENA_X + gx * TILE, ARENA_Y + gy * TILE, gx, gy,
                              g_brickHp[gy][gx]);
        }
}

/* 道具存货槽：画在血量方块正下方，黄点=地雷、蓝点=砖块，后面跟 ×数量。
 * 数量为 0 时整条变暗 —— 玩家扫一眼就知道自己手里还有没有东西可放。
 * rightAlign = 1 表示这一组靠右边排（P2 用），只是起点算在右侧而已，
 * 组内仍然是“点 → 文字”的顺序，两边看起来对称。                    */
static void DrawItemSlots(HDC dc, const Tank *t, int rightAlign)
{
    const COLORREF dim = RGB(44, 49, 66);
    wchar_t buf[24];
    int x0 = rightAlign ? (WIN_W - 16 - 104) : 16;
    int y = 66;

    /* ---- 黄：地雷 ---- */
    FillBox(dc, x0, y + 2, 11, 11, t->mines > 0 ? C_ITEM_MINE : dim);
    if (t->mines > 0) {
        FillBox(dc, x0 + 2, y + 4, 7, 2, Blend(C_ITEM_MINE, RGB(255, 255, 255), 0.6f));
        FillBox(dc, x0 + 5, y + 6, 2, 5, Blend(C_ITEM_MINE, RGB(0, 0, 0), 0.45f));
    }
    swprintf(buf, 24, L"雷 ×%d", t->mines);
    DrawStr(dc, g_fSmall, t->mines > 0 ? C_ITEM_MINE : C_TXT_DIM,
            x0 + 15, y - 2, 40, 18, buf, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* ---- 蓝：砖块 ---- */
    x0 += 50;
    FillBox(dc, x0, y + 2, 11, 11, t->bricks > 0 ? C_ITEM_BRICK : dim);
    if (t->bricks > 0) {
        FillBox(dc, x0, y + 6, 11, 1, Blend(C_ITEM_BRICK, RGB(0, 0, 0), 0.45f));
        FillBox(dc, x0 + 5, y + 2, 1, 5, Blend(C_ITEM_BRICK, RGB(0, 0, 0), 0.30f));
    }
    swprintf(buf, 24, L"砖 ×%d", t->bricks);
    DrawStr(dc, g_fSmall, t->bricks > 0 ? C_ITEM_BRICK : C_TXT_DIM,
            x0 + 15, y - 2, 40, 18, buf, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/* 顶部 HUD：五行——阵营名/规则、比分/计时、血量/武器、道具槽、操作提示。
 * 全是写死的坐标，改布局直接调这些数字。                          */
static void DrawHUD(HDC dc)
{
    wchar_t buf[64];
    int i;
    COLORREF tc;
    const wchar_t *wn0 = L"普通弹", *wn1 = L"普通弹";
    COLORREF wc0 = C_TXT_DIM, wc1 = C_TXT_DIM;

    FillBox(dc, 0, 0, WIN_W, ARENA_Y - 12, C_PANEL);          /* 面板 */
    FillBox(dc, 0, ARENA_Y - 13, WIN_W, 1, RGB(46, 52, 70));   /* 面板下沿分隔线 */

    /* 第一行：阵营名 + 规则提示 */
    DrawStr(dc, g_fSmall, C_P1, 16, 2, 110, 18, L"红方 P1",
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawStr(dc, g_fSmall, C_P2, WIN_W - 126, 2, 110, 18, L"蓝方 P2",
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DrawStr(dc, g_fSmall, C_TXT_DIM, 0, 2, WIN_W, 18,
            L"4 分钟  ·  任一方阵亡，对方 +1 分  ·  先到 36 分获胜",
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* 第二行：左分数 / 中央倒计时 / 右分数 */
    swprintf(buf, 64, L"%d", g_tanks[0].score);
    DrawStr(dc, g_fBig, C_P1, 16, 16, 120, 36, buf,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    swprintf(buf, 64, L"%d", g_tanks[1].score);
    DrawStr(dc, g_fBig, C_P2, WIN_W - 136, 16, 120, 36, buf,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    {
        int total = (int)(g_timeLeft + 0.99f);   /* 向上取整，开局显示 4:00 */
        if (total < 0) total = 0;
        swprintf(buf, 64, L"%d:%02d", total / 60, total % 60);
        tc = (g_timeLeft <= 30.0f) ? RGB(255, 110, 100) : C_TXT;   /* 最后 30 秒变红 */
        DrawStr(dc, g_fBig, tc, 0, 16, WIN_W, 36, buf,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* 第三行：血量方块 + 当前武器（有道具时显示剩余次数） */
    for (i = 0; i < MAX_HP; i++) {
        FillBox(dc, 16 + i * 13, 52, 10, 10,
                (i < g_tanks[0].hp && g_tanks[0].alive) ? C_P1 : RGB(48, 54, 72));
        FillBox(dc, WIN_W - 36 - i * 13, 52, 10, 10,
                (i < g_tanks[1].hp && g_tanks[1].alive) ? C_P2 : RGB(48, 54, 72));
    }

    if (g_tanks[0].weapon == WPN_LASER)       { wn0 = L"激光炮"; wc0 = C_ITEM_LASER; }
    else if (g_tanks[0].weapon == WPN_HOMING) { wn0 = L"追踪弹"; wc0 = C_ITEM_HOMING; }
    if (g_tanks[1].weapon == WPN_LASER)       { wn1 = L"激光炮"; wc1 = C_ITEM_LASER; }
    else if (g_tanks[1].weapon == WPN_HOMING) { wn1 = L"追踪弹"; wc1 = C_ITEM_HOMING; }

    if (g_tanks[0].weapon == WPN_NORMAL) swprintf(buf, 64, L"%s", wn0);
    else                                 swprintf(buf, 64, L"%s ×%d", wn0, g_tanks[0].ammo);
    DrawStr(dc, g_fSmall, wc0, 62, 48, 150, 18, buf,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (g_tanks[1].weapon == WPN_NORMAL) swprintf(buf, 64, L"%s", wn1);
    else                                 swprintf(buf, 64, L"%s ×%d", wn1, g_tanks[1].ammo);
    DrawStr(dc, g_fSmall, wc1, WIN_W - 212, 48, 150, 18, buf,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    /* 第四行：道具存货槽（血量 UI 正下方，一眼可见有没有雷 / 砖） */
    DrawItemSlots(dc, &g_tanks[0], 0);
    DrawItemSlots(dc, &g_tanks[1], 1);

    /* 第五、六行：规则备忘 + 操作提示 */
    DrawStr(dc, g_fSmall, C_TXT_DIM, 0, 84, WIN_W, 13,
            L"斜向：两键同按   ·   红=激光 绿=追踪 黄=地雷 蓝=砖块   ·   每件 3 次   ·   阵亡清空",
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawStr(dc, g_fSmall, C_TXT_DIM, 0, 97, WIN_W, 13,
            L"P1  WASD·J 开火·空格 布雷·K 砌砖        P2  方向键·Enter 开火·小键盘1 布雷·小键盘0 砌砖",
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* 粒子：颜色随寿命从本色淡回地面色，尺寸从 4px 缩到 1px，
 * 所以看起来是“渐渐熄灭”，而不是突然消失。                          */
static void DrawSparks(HDC dc)
{
    int i;
    for (i = 0; i < MAX_SPARKS; i++) {
        Spark *s = &g_sparks[i];
        int px, py, sz;
        float k;
        if (s->life <= 0.0f) continue;
        k = s->life / s->maxLife;                  /* 1 → 0 的剩余比例 */
        px = ARENA_X + (int)s->pos.x;
        py = ARENA_Y + (int)s->pos.y;
        sz = (int)(4.0f * k) + 1;
        FillBox(dc, px, py, sz, sz, Blend(C_FLOOR, s->color, k));
    }
}

/* 子弹：三层同心圆（外光晕 → 本体 → 高光点），视觉上有发光感 */
static void DrawBullets(HDC dc)
{
    int i;
    for (i = 0; i < MAX_BULLETS_ALL; i++) {
        Bullet *b = &g_bullets[i];
        int cx, cy;
        HBRUSH br;
        HGDIOBJ ob, op;
        if (!b->active) continue;
        cx = ARENA_X + (int)b->pos.x;
        cy = ARENA_Y + (int)b->pos.y;

        br = CreateSolidBrush(Blend(C_FLOOR, C_BULLET, 0.55f));   /* 外圈暗光晕 */
        ob = SelectObject(dc, br);
        op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - 6, cy - 6, cx + 6, cy + 6);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);

        br = CreateSolidBrush(C_BULLET);                          /* 本体 */
        ob = SelectObject(dc, br);
        op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - 3, cy - 3, cx + 4, cy + 4);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);

        FillBox(dc, cx - 1, cy - 1, 2, 2, RGB(255, 250, 230));    /* 高光点 */
    }
}

/* 地雷：埋下的瞬间是“显型”的 —— 先亮 0.35 秒（黄圈 + 雷体 + 四根刺），
 * 再用 0.55 秒淡出，之后彻底隐形（规则要求）。
 * 它画在坦克下面，所以被车压住时只能看到露出来的边。            */
static void DrawMines(HDC dc)
{
    int i, k;
    for (i = 0; i < MAX_MINES; i++) {
        Mine *m = &g_mines[i];
        int cx, cy, r;
        float a;
        HBRUSH br;
        HPEN pen;
        HGDIOBJ ob, op;

        if (!m->active) continue;
        if (m->blink <= 0.0f) continue;          /* 提示结束 → 完全隐形 */

        /* 前 0.35 秒完全不透明，之后按剩余时间淡出 */
        a = (m->blink > 0.55f) ? 1.0f : (m->blink / 0.55f);
        cx = ARENA_X + (int)m->pos.x;
        cy = ARENA_Y + (int)m->pos.y;
        r  = (int)(9.0f * (0.55f + 0.45f * a));  /* 淡出时圈也一起收缩 */

        /* 外圈冲击环 */
        pen = CreatePen(PS_SOLID, 2, Blend(C_FLOOR, C_ITEM_MINE, a * 0.9f));
        op = SelectObject(dc, pen);
        ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);

        /* 雷体：深色圆盘 */
        br = CreateSolidBrush(Blend(C_FLOOR, RGB(26, 28, 18), a));
        ob = SelectObject(dc, br);
        op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - 5, cy - 5, cx + 6, cy + 6);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);

        /* 四根短刺 + 中心黄点 */
        pen = CreatePen(PS_SOLID, 1, Blend(C_FLOOR, C_ITEM_MINE, a));
        op = SelectObject(dc, pen);
        for (k = 0; k < 4; k++) {
            static const int sx[4] = { 1, 1, -1, -1 };
            static const int sy[4] = { 1, -1, 1, -1 };
            MoveToEx(dc, cx, cy, NULL);
            LineTo(dc, cx + sx[k] * 6, cy + sy[k] * 6);
        }
        SelectObject(dc, op);
        DeleteObject(pen);
        FillBox(dc, cx - 1, cy - 1, 3, 3, Blend(C_FLOOR, C_ITEM_MINE, a));
    }
}

/* 道具小球：外圈光晕 + 球体 + 高光 + 中间符号。
 * 四种颜色对应四种道具，中间的小符号是为了色弱玩家也能区分。      */
static void DrawItems(HDC dc)
{
    int i;
    for (i = 0; i < MAX_ITEMS; i++) {
        Item *it = &g_items[i];
        int cx, cy, r;
        COLORREF c;
        HBRUSH br;
        HPEN pen;
        HGDIOBJ ob, op;
        float pulse;

        if (!it->active) continue;
        /* 最后 3 秒快速闪烁，提示"快消失了" */
        if (it->life < 3.0f && ((int)(it->life * 8.0f) & 1)) continue;

        c = ItemColor(it->type);
        cx = ARENA_X + (int)it->pos.x;
        cy = ARENA_Y + (int)it->pos.y;
        pulse = 1.0f + 0.10f * sinf(it->phase * 6.0f);   /* 轻微呼吸 */
        r = (int)(ITEM_R * pulse);

        /* 光晕 */
        br = CreateSolidBrush(Blend(C_FLOOR, c, 0.28f));
        ob = SelectObject(dc, br);
        op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - r - 3, cy - r - 3, cx + r + 4, cy + r + 4);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);

        /* 球体 */
        br = CreateSolidBrush(c);
        ob = SelectObject(dc, br);
        op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);

        /* 左上角高光 */
        br = CreateSolidBrush(Blend(c, RGB(255, 255, 255), 0.70f));
        ob = SelectObject(dc, br);
        op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - r / 2 - 2, cy - r / 2 - 2, cx - 1, cy - 2);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);

        /* 中间符号：0 闪电 / 1 准星 / 2 中心点 / 3 小砖块
         * 光靠颜色区分对色弱玩家不友好，所以每种道具再给一个形状。 */
        pen = CreatePen(PS_SOLID, 1, Blend(c, RGB(0, 0, 0), 0.55f));
        op = SelectObject(dc, pen);
        ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (it->type == 0) {
            MoveToEx(dc, cx - 2, cy - 5, NULL);
            LineTo(dc, cx + 1, cy - 1);
            LineTo(dc, cx - 1, cy + 1);
            LineTo(dc, cx + 2, cy + 5);
        } else if (it->type == 1) {
            Ellipse(dc, cx - 4, cy - 4, cx + 5, cy + 5);
            Ellipse(dc, cx - 1, cy - 1, cx + 2, cy + 2);
        } else if (it->type == 2) {
            Ellipse(dc, cx - 2, cy - 2, cx + 3, cy + 3);
        } else {
            Rectangle(dc, cx - 4, cy - 4, cx + 5, cy + 5);   /* 方框 = 一块砖 */
            MoveToEx(dc, cx - 4, cy, NULL);
            LineTo(dc, cx + 5, cy);
        }
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
    }
}

/* 激光光轨：三层叠画（粗光晕 → 中层 → 亮芯），整体按剩余寿命淡出。
 * 折线顶点就是 CastLaser 记录下来的起点、各反射点、终点。          */
static void DrawLaser(HDC dc)
{
    int i, s;
    for (i = 0; i < 2; i++) {
        Laser *L = &g_lasers[i];
        COLORREF c;
        float k;
        HPEN pen;
        HGDIOBJ old;

        if (!L->active || L->npts < 2) continue;
        k = L->life / LASER_LIFE;
        if (k < 0.0f) k = 0.0f;
        c = g_tanks[L->owner].color;

        pen = CreatePen(PS_SOLID, 7, Blend(C_FLOOR, c, 0.30f * k));
        old = SelectObject(dc, pen);
        for (s = 0; s < L->npts - 1; s++) {
            MoveToEx(dc, ARENA_X + (int)L->pts[s].x, ARENA_Y + (int)L->pts[s].y, NULL);
            LineTo(dc, ARENA_X + (int)L->pts[s + 1].x, ARENA_Y + (int)L->pts[s + 1].y);
        }
        SelectObject(dc, old);
        DeleteObject(pen);

        pen = CreatePen(PS_SOLID, 3, Blend(c, RGB(255, 255, 255), 0.30f * k));
        old = SelectObject(dc, pen);
        for (s = 0; s < L->npts - 1; s++) {
            MoveToEx(dc, ARENA_X + (int)L->pts[s].x, ARENA_Y + (int)L->pts[s].y, NULL);
            LineTo(dc, ARENA_X + (int)L->pts[s + 1].x, ARENA_Y + (int)L->pts[s + 1].y);
        }
        SelectObject(dc, old);
        DeleteObject(pen);

        pen = CreatePen(PS_SOLID, 1, Blend(c, C_LASER_CORE, k));
        old = SelectObject(dc, pen);
        for (s = 0; s < L->npts - 1; s++) {
            MoveToEx(dc, ARENA_X + (int)L->pts[s].x, ARENA_Y + (int)L->pts[s].y, NULL);
            LineTo(dc, ARENA_X + (int)L->pts[s + 1].x, ARENA_Y + (int)L->pts[s + 1].y);
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

/* 追踪弹：瞄准阶段画收缩准星（敌人看见就能跑），飞行阶段画绿色小球 */
static void DrawMissiles(HDC dc)
{
    int i;
    for (i = 0; i < MAX_MISSILES; i++) {
        Missile *m = &g_missiles[i];
        if (!m->active) continue;

        if (m->phase == 0) {
            int tx = ARENA_X + (int)m->lockPos.x;
            int ty = ARENA_Y + (int)m->lockPos.y;
            int rr = (int)(14.0f + 16.0f * (m->timer / MISSILE_AIM));
            HPEN pen = CreatePen(PS_DOT, 1, Blend(C_FLOOR, C_ITEM_HOMING, 0.85f));
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));

            Ellipse(dc, tx - rr, ty - rr, tx + rr, ty + rr);   /* 收缩的预警圈 */
            Ellipse(dc, tx - 7, ty - 7, tx + 7, ty + 7);       /* 中心准星 */
            MoveToEx(dc, ARENA_X + (int)m->origin.x, ARENA_Y + (int)m->origin.y, NULL);
            LineTo(dc, tx, ty);                                /* 指向目标的虚线 */

            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
        } else {
            int cx = ARENA_X + (int)m->pos.x;
            int cy = ARENA_Y + (int)m->pos.y;
            HBRUSH br;
            HGDIOBJ ob, op;

            br = CreateSolidBrush(Blend(C_FLOOR, C_ITEM_HOMING, 0.45f));
            ob = SelectObject(dc, br);
            op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, cx - 7, cy - 7, cx + 7, cy + 7);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(br);

            br = CreateSolidBrush(C_ITEM_HOMING);
            ob = SelectObject(dc, br);
            op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, cx - 4, cy - 4, cx + 5, cy + 5);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(br);
        }
    }
}

/* 死亡标记：在出生点画一个随倒计时长大的圆，告诉玩家“马上在这里复活” */
static void DrawRespawnMark(HDC dc, const Tank *t)
{
    int cx, cy, h, r;
    HBRUSH br;
    HGDIOBJ ob, op;
    if (t->alive) return;
    cx = ARENA_X + (int)t->spawn.x;
    cy = ARENA_Y + (int)t->spawn.y;
    h = (int)TANK_HALF + 4;
    r = (int)(h * (0.4f + 0.6f * (1.0f - t->respawn / RESPAWN_TIME)));

    br = CreateSolidBrush(Blend(C_FLOOR, t->color, 0.25f));
    ob = SelectObject(dc, br);
    op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
}

/* 放置预览：告诉玩家“现在按下去会放在哪”。
 *   手里有砖 → 车头前一格画一个虚线方框（那一格放不下时画成暗红）
 *   手里有雷 → 脚下一个虚线圆
 * 纯粹是画面提示，不参与任何逻辑判定。                            */
static void DrawPlaceHint(HDC dc)
{
    int idx;

    for (idx = 0; idx < 2; idx++) {
        Tank *t = &g_tanks[idx];
        if (!t->alive) continue;

        /* ---- 地雷：车体四角的角括号 ----
         * 为什么不用圆圈：重生护盾本身就是一个圆环，两个圆叠在一起玩家
         * 分不清哪个是“我按空格会埋雷”、哪个是“我现在无敌”。      */
        if (t->mines > 0) {
            int cx = ARENA_X + (int)t->pos.x;
            int cy = ARENA_Y + (int)t->pos.y;
            int r = 13, len = 5, k;
            HPEN pen = CreatePen(PS_SOLID, 1, Blend(C_FLOOR, C_ITEM_MINE, 0.95f));
            HGDIOBJ op = SelectObject(dc, pen);
            for (k = 0; k < 4; k++) {
                int sx = (k & 1) ? 1 : -1;
                int sy = (k & 2) ? 1 : -1;
                MoveToEx(dc, cx + sx * r, cy + sy * r, NULL);
                LineTo(dc, cx + sx * (r - len), cy + sy * r);
                MoveToEx(dc, cx + sx * r, cy + sy * r, NULL);
                LineTo(dc, cx + sx * r, cy + sy * (r - len));
            }
            SelectObject(dc, op);
            DeleteObject(pen);
        }

        /* ---- 砖块：车头前一格的方框 ---- */
        if (t->bricks > 0) {
            int cx, cy;
            if (BrickTargetCell(t, &cx, &cy)) {
                int px = ARENA_X + cx * TILE;
                int py = ARENA_Y + cy * TILE;
                int ok = CellFreeForBrick(cx, cy, idx);
                COLORREF hc = ok ? Blend(C_FLOOR, C_ITEM_BRICK, 0.85f)
                                 : Blend(C_FLOOR, RGB(255, 96, 96), 0.60f);
                HPEN pen = CreatePen(PS_DOT, 1, hc);
                HGDIOBJ op = SelectObject(dc, pen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, px + 1, py + 1, px + TILE - 1, py + TILE - 1);
                SelectObject(dc, op);
                SelectObject(dc, ob);
                DeleteObject(pen);
            }
        }
    }
}

/* 联机等待 / 大厅覆盖层：联机没有开始前盖住战场，给出清晰的下一步提示。 */
static void DrawNetOverlay(HDC dc)
{
    const wchar_t *title;
    wchar_t buf[128];

    if (g_appMode != APP_HOST && g_appMode != APP_CLIENT) return;
    if (g_netPhase != NET_WAIT_PEER && g_netPhase != NET_CONNECTING &&
        g_netPhase != NET_HANDSHAKE && g_netPhase != NET_LOBBY) return;

    if (g_netPhase == NET_WAIT_PEER)      title = L"等待玩家加入";
    else if (g_netPhase == NET_LOBBY)     title = L"局域网大厅";
    else                                   title = L"正在连接";

    {
        int bw = 470, bh = 230;
        int bx = (WIN_W - bw) / 2, by = (WIN_H - bh) / 2;
        HBRUSH br = CreateSolidBrush(RGB(16, 18, 27));
        HPEN pen = CreatePen(PS_SOLID, 2, C_TXT);
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, pen);
        RoundRect(dc, bx, by, bx + bw, by + bh, 16, 16);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);

        DrawStr(dc, g_fHuge, C_TXT, bx, by + 18, bw, 44, title,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawStr(dc, g_fSmall, C_TXT_DIM, bx, by + 68, bw, 20, g_net.status,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (g_appMode == APP_HOST) {
            swprintf(buf, 128, L"本机 IP：%s", g_net.localIp[0] ? g_net.localIp : L"未识别");
            DrawStr(dc, g_fMid, C_TXT, bx, by + 100, bw, 24, buf,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            swprintf(buf, 128, L"端口：%u    邀请码：%06u", (unsigned)g_net.port,
                     (unsigned)g_net.invite);
            DrawStr(dc, g_fMid, C_TXT, bx, by + 132, bw, 24, buf,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawStr(dc, g_fSmall, C_TXT_DIM, bx, by + 168, bw, 20,
                    L"把这三项告诉对方；按 R 准备，Esc 返回模式选择",
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawStr(dc, g_fSmall, C_TXT_DIM, bx, by + 104, bw, 20,
                    L"你是蓝方 P2：WASD 移动，J 开火，空格 放雷，K 砌砖",
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawStr(dc, g_fSmall, C_TXT_DIM, bx, by + 134, bw, 20,
                    L"按 R 准备，Esc 返回模式选择",
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        if (g_netPhase == NET_LOBBY) {
            swprintf(buf, 128, L"准备状态：你 %s  ·  对方 %s",
                     g_net.localReady ? L"已准备" : L"未准备",
                     g_net.remoteReady ? L"已准备" : L"未准备");
            DrawStr(dc, g_fMid, C_TXT, bx, by + 192, bw, 24, buf,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

/* 整个画面就是这张“绘制顺序表”，后面的盖住前面的：
 *   背景 → 场地 → 粒子 → 子弹 → 重生标记 → 坦克 → HUD → 联机层 → 胜利面板
 * 想让某样东西压在上面，就把它的调用往后挪。                        */
static void Render(HDC dc)
{
    wchar_t buf[64];

    FillBox(dc, 0, 0, WIN_W, WIN_H, C_BG);
    DrawArena(dc);
    DrawMines(dc);          /* 地雷在最底层：几乎不可见，被坦克压住才对 */
    DrawItems(dc);
    DrawSparks(dc);
    DrawBullets(dc);
    DrawRespawnMark(dc, &g_tanks[0]);
    DrawRespawnMark(dc, &g_tanks[1]);
    DrawTank(dc, &g_tanks[0]);
    DrawTank(dc, &g_tanks[1]);
    DrawPlaceHint(dc);      /* 放置预览压在坦克上面，才看得清对准哪一格 */
    DrawLaser(dc);          /* 光轨压在坦克上面，才像"扫过去" */
    DrawMissiles(dc);       /* 追踪弹和准星也在最上层 */
    DrawHUD(dc);
    DrawNetOverlay(dc);

    /* 结算面板：盖在最上层，边框颜色跟胜方一致，平局用白色 */
    if (g_state == 1) {
        int bw = 430, bh = 190;
        int bx = (WIN_W - bw) / 2, by = (WIN_H - bh) / 2;
        COLORREF c;
        const wchar_t *title, *sub;
        HBRUSH br;
        HPEN pen;
        HGDIOBJ ob, op;

        if (g_winner < 0)       { c = C_TXT; title = L"平  局"; }
        else if (g_winner == 0) { c = C_P1;  title = L"红方 获胜！"; }
        else                    { c = C_P2;  title = L"蓝方 获胜！"; }

        if (g_endReason == 0)  sub = L"率先达到 36 分";
        else if (g_winner < 0) sub = L"时间到 · 双方同分";
        else                   sub = L"时间到 · 分数领先";

        br = CreateSolidBrush(RGB(16, 18, 27));
        pen = CreatePen(PS_SOLID, 2, c);
        ob = SelectObject(dc, br);
        op = SelectObject(dc, pen);
        RoundRect(dc, bx, by, bx + bw, by + bh, 16, 16);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);

        DrawStr(dc, g_fHuge, c, bx, by + 18, bw, 48, title,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        swprintf(buf, 64, L"%d  :  %d", g_tanks[0].score, g_tanks[1].score);
        DrawStr(dc, g_fBig, C_TXT, bx, by + 72, bw, 34, buf,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        DrawStr(dc, g_fSmall, C_TXT_DIM, bx, by + 110, bw, 20, sub,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (g_appMode == APP_HOST || g_appMode == APP_CLIENT) {
            swprintf(buf, 64, L"联机：按 R 准备重赛（你%s，对方%s）",
                     g_net.localReady ? L"已准备" : L"未准备",
                     g_net.remoteReady ? L"已准备" : L"未准备");
            DrawStr(dc, g_fSmall, C_TXT_DIM, bx, by + 140, bw, 20, buf,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawStr(dc, g_fSmall, C_TXT_DIM, bx, by + 140, bw, 20,
                    L"按  R  再来一局", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

/* 双缓冲：所有绘制先画到内存位图，最后一次性 BitBlt 贴到窗口。
 * 如果直接画到窗口 DC，每帧都能看到“擦除 → 重画”的中间状态，就是闪烁。
 * 内存 DC 和位图只在第一次调用时创建，之后一直复用。                */
static void RenderFrame(HDC target)
{
    if (!g_memDC) {
        g_memDC  = CreateCompatibleDC(target);
        g_memBmp = CreateCompatibleBitmap(target, WIN_W, WIN_H);
        SelectObject(g_memDC, g_memBmp);
    }
    Render(g_memDC);
    BitBlt(target, 0, 0, WIN_W, WIN_H, g_memDC, 0, 0, SRCCOPY);
}

/* =============================== 窗口过程 =============================== */

/* 窗口消息处理。因为主循环里主动每帧重画，这里几乎什么都不用做：
 *   WM_PAINT  —— 窗口需要重画时补一帧（比如被别的窗口遮住后重新露出）
 *   WM_DESTROY —— 点关闭按钮，投递退出消息结束消息循环
 *   其他消息交给系统默认处理（拖动、最小化等都由它管）              */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RenderFrame(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        /* 空格现在是 P1 的埋雷键。按键本身由 GetAsyncKeyState 轮询，
         * 这里只要拦掉、不让它落到 DefWindowProc —— 否则系统会当它是
         * “未处理的按键”，每按一下就“叮”一声响。                      */
        if (wp == VK_SPACE) return 0;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 创建 5 档字体：标题 / 分数 / 小节 / 正文 / 胜利大字，全部微软雅黑。
 * 高度参数是逻辑单位，CLEARTYPE_QUALITY 让中文字形不发虚。         */
static void CreateFonts(void)
{
    g_fTitle = CreateFontW(20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"微软雅黑");
    g_fBig   = CreateFontW(26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"微软雅黑");
    g_fMid   = CreateFontW(17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"微软雅黑");
    g_fSmall = CreateFontW(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"微软雅黑");
    g_fHuge  = CreateFontW(38, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"微软雅黑");
}

/* 程序入口。做四件事：建窗口 → 初始化资源 → 跑主循环 → 清理退出。
 *
 * 主循环是“非阻塞”的：
 *   PeekMessage 把消息队列抽干（不阻塞），然后自己算 dt、更新、重画。
 *   如果改用 GetMessage，它没消息时会挂起，游戏就不动了。          */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    WNDCLASSEXW wc;
    RECT rc;
    DWORD style;
    HWND hwnd;
    HDC hdc;
    MSG msg;
    LARGE_INTEGER freq, prev, now;
    int running = 1;
    int steps;
    float dt, accumulator = 0.0f;

    (void)hPrev; (void)cmdLine;   /* 用不到这两个参数，显式忽略避免警告 */

    /* 声明进程 DPI 感知：否则高分屏上系统会把窗口按比例拉伸，画面发虚 */
    SetProcessDPIAware();

    /* 注册窗口类。hbrBackground 给 NULL：背景完全由我们自己画 */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));  /* IDC_ARROW */
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"TankBattleDemoWnd";
    RegisterClassExW(&wc);

    /* 去掉可调边框和最大化，保证客户区尺寸恒定（游戏坐标是写死的） */
    style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    /* AdjustWindowRect：把“客户区 WxH”换算成“含标题栏边框的窗口尺寸” */
    rc.left = 0; rc.top = 0; rc.right = WIN_W; rc.bottom = WIN_H;
    AdjustWindowRect(&rc, style, FALSE);

    g_hInst = hInst;
    hwnd = CreateWindowExW(0, L"TankBattleDemoWnd", L"坦克大战 · 双人对战 Demo",
                           style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g_mainWnd = hwnd;

    CreateFonts();
    ResetGame();
    if (!CreateMenuWindow()) return 1;

    /* hdc 只在开头取一次、结尾释放一次，中间每帧复用，少一次系统调用 */
    hdc = GetDC(hwnd);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    while (running) {
        /* 1) 抽干消息队列；不阻塞，所以下面能一直跑 */
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        if (g_appMode == APP_MENU) {
            QueryPerformanceCounter(&prev);
            accumulator = 0.0f;
            Sleep(10);
            continue;
        }

        if (KeyDown(VK_ESCAPE)) {
            if (g_appMode == APP_LOCAL) break;
            NetLeaveToMenu();
            QueryPerformanceCounter(&prev);
            accumulator = 0.0f;
            continue;
        }

        NetPoll();
        if (g_appMode == APP_MENU) {
            QueryPerformanceCounter(&prev);
            accumulator = 0.0f;
            continue;
        }

        /* 2) 真实时间进入累加器，游戏始终按固定 60Hz 推进。 */
        QueryPerformanceCounter(&now);
        dt = (float)((double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart);
        prev = now;
        if (dt > 0.25f) dt = 0.25f;
        accumulator += dt;

        /* 3) 一轮最多追赶 8 个 tick，防止拖动窗口后集中追帧。 */
        steps = 0;
        while (accumulator >= SIM_DT && steps < NET_MAX_TICK_CATCHUP) {
            if (g_appMode == APP_LOCAL)
                UpdateGame(SIM_DT);
            else if (g_appMode == APP_HOST && g_netPhase == NET_RUNNING)
                UpdateHostGame(SIM_DT);
            else if (g_appMode == APP_CLIENT && g_netPhase == NET_RUNNING)
                UpdateClientInput(SIM_DT);
            else
                break;
            accumulator -= SIM_DT;
            steps++;
        }
        if (steps == NET_MAX_TICK_CATCHUP && accumulator >= SIM_DT) accumulator = 0.0f;

        RenderFrame(hdc);
        Sleep(1);    /* 让出一点 CPU，别把一个核心跑满 */
    }

    /* 4) 清理：通知对方、关闭网络、释放 DC 和内存位图。字体交给系统回收。 */
    if (g_net.sock != INVALID_SOCKET) {
        NetSendSimple(NET_MSG_LEAVE);
        NetFlush();
    }
    NetResetState();
    if (g_wsaStarted) WSACleanup();
    ReleaseDC(hwnd, hdc);
    if (g_memDC) { DeleteDC(g_memDC); g_memDC = NULL; }
    if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = NULL; }
    DestroyWindow(hwnd);
    return 0;
}
