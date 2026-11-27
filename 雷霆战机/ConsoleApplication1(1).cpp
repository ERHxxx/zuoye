/* ============================================================================
 *  打飞机 · Sky Fighter  —  C 语言控制台版（Windows / Linux 均可编译）
 * ----------------------------------------------------------------------------
 *  60FPS 实时刷新：敌机不断从上方压下来，你可以走位、按住空格开火，
 *  还能捡战场里飘落的弹药箱，换用各种武器。
 *
 *  编译（Windows，MSVC 命令行）：
 *      cl /utf-8 shoot.c user32.lib
 *  编译（Windows，MinGW / gcc）：
 *      gcc shoot.c -o shoot.exe -O2
 *  编译（Linux / macOS）：
 *      gcc shoot.c -o shoot -O2
 *
 *  操作：
 *      ← → ↑ ↓   移动飞机（按住持续移动）
 *      空格       开火（按住连射）
 *      P          暂停 / 继续
 *      R          重新开始
 *      ESC        退出
 *
 *  弹药箱（带英文字母，从上方飘落，飞过去撞上就能吃）：
 *      [S] SHOTGUN   霰弹枪   一次扇形打出 5 发，近距离覆盖广，8 次
 *      [F] FLAME     喷火器   超快连喷火舌，射程短但火力猛，70 次
 *      [H] HOMING    追踪导弹 自动拐弯追最近的敌机，伤害 3，8 发
 *      [R] RAPID     速射     射速提升 2 倍多，45 次
 *      [M] MULTI     三向弹   一次并排打 3 发，14 次
 *      弹药打完自动换回默认机炮（GUN，无限弹药）
 *
 *  玩法：
 *      - 击落小飞机 +10，轰炸机 +40，俯冲机 +20
 *      - 每击落 20 架升 1 级，敌机生成更快、下落更猛
 *      - 3 条命，受伤后短暂无敌闪烁
 *
 *  实现要点：
 *      - 双缓冲：整屏先画进内存，再一次性写进控制台，绝不闪屏
 *      - delta-time 驱动，移动/子弹/生成都按秒计算，跟帧率解耦
 *      - Windows 用 GetAsyncKeyState 读"键是否按住"，手感连续
 *      - 中文提示用 WriteConsoleW 宽字符输出，不受代码页影响
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#ifdef _WIN32
#  include <windows.h>
#  include <conio.h>
#else
#  include <unistd.h>
#  include <termios.h>
#  include <fcntl.h>
#  include <sys/select.h>
#  include <locale.h>
#endif

 /* ============================ 布局参数 ================================== */
#define FIELD_W     56          /* 战场宽（格） */
#define FIELD_H     24          /* 战场高（格） */
#define FX           2          /* 战场左上角所在的屏幕列 */
#define FY           2          /* 战场左上角所在的屏幕行 */
#define SCR_W       62          /* 虚拟屏幕宽 */
#define SCR_H       27          /* 虚拟屏幕高 */

/* ============================ 数量上限 ================================== */
#define MAX_PB      90          /* 我方子弹/火舌 */
#define MAX_EB      70          /* 敌方子弹 */
#define MAX_EN      16          /* 敌机 */
#define MAX_PT     160          /* 爆炸碎片 */
#define MAX_STAR    60          /* 背景星星 */
#define MAX_CR       4          /* 弹药箱 */

/* ============================ 颜色 ====================================== */
enum {
    C_DEF = 7, C_DIM = 8, C_BLUE = 9, C_GREEN = 10, C_CYAN = 11,
    C_RED = 12, C_PURPLE = 13, C_YELLOW = 14, C_WHITE = 15
};

/* ============================ 武器定义 ================================== */
enum { W_GUN = 0, W_SHOT, W_FLAME, W_HOME, W_RAPID, W_MULTI, W_COUNT };

typedef struct {
    char        letter;         /* 弹药箱上的字母 */
    const char* name;           /* HUD 显示名（4 字符） */
    int         color;
    int         ammo;           /* -1 表示无限 */
    double      interval;       /* 射击间隔（秒） */
} WeaponDef;

static const WeaponDef WEAPONS[W_COUNT] = {
    { ' ', "GUN ", C_WHITE,   -1, 0.130 },   /* 默认机炮 */
    { 'S', "SHOT", C_YELLOW,   8, 0.420 },   /* 霰弹枪 */
    { 'F', "FLAM", C_RED,     70, 0.055 },   /* 喷火器 */
    { 'H', "HOME", C_PURPLE,   8, 0.550 },   /* 追踪导弹 */
    { 'R', "RAPD", C_CYAN,    45, 0.055 },   /* 速射 */
    { 'M', "MULT", C_GREEN,   14, 0.200 }    /* 三向弹 */
};

/* 子弹种类 */
enum { SK_BULLET = 0, SK_PELLET, SK_MISSILE, SK_FLAME };

/* ============================ 数据 ====================================== */
typedef struct {
    double x, y, vx, vy;
    double life;                /* 火焰用，<=0 消失；其它为 -1 */
    int    kind;
    int    dmg;
    int    on;
} Shot;

typedef struct {
    double x, y, vx, vy;
    int type;                   /* 0 小飞机 1 轰炸机 2 俯冲机 */
    int hp;
    double fireCd;
    int on;
} Enemy;

typedef struct {
    double x, y, vx, vy, life;
    char ch;
    int col;
    int on;
} Particle;

typedef struct { double x, y, vy; char ch; int col; } Star;

typedef struct {
    double x, y, vy;
    int kind;                   /* 授予的武器编号 */
    int on;
} Crate;

static struct {
    double x, y;
    int lives;
    double inv;                 /* 剩余无敌时间 */
} player;

static Shot      pb[MAX_PB];
static Shot      eb[MAX_EB];
static Enemy     en[MAX_EN];
static Particle  pt[MAX_PT];
static Star      st[MAX_STAR];
static Crate     cr[MAX_CR];

static int    weapon = W_GUN;
static int    ammo = -1;
static double fireTimer;

static long long score;
static long long hiScore;
static int  kills;
static int  levelNo;
static double spawnTimer;
static double spawnInterval;
static double crateTimer;
static double timeNow;

static int  gameOver;
static int  paused;
static int  quitGame;

/* ============================ 小工具 ==================================== */
static double rnd(void) { return (double)rand() / (double)RAND_MAX; }
static double rndR(double a, double b) { return a + (b - a) * rnd(); }

/* 轴对齐矩形相交；坐标统一用"格" */
static int hitBox(double ax, double ay, int aw, int ah,
    double bx, double by, int bw, int bh)
{
    int ax0 = (int)ax, ay0 = (int)ay, ax1 = (int)ax + aw, ay1 = (int)ay + ah;
    int bx0 = (int)bx, by0 = (int)by, bx1 = (int)bx + bw, by1 = (int)by + bh;
    return ax0 < bx1 && ax1 > bx0 && ay0 < by1 && ay1 > by0;
}

/* 前向声明：弹药箱拾取时要用到爆炸特效；导弹发射时要用到寻敌 */
static void boom(double x, double y, int col);
static int  nearestEnemy(double x, double y, double* ox, double* oy);

/* ============================ 虚拟屏幕 ================================== */
static char          scrCh[SCR_H][SCR_W];
static unsigned char scrCo[SCR_H][SCR_W];

static void scClear(void)
{
    int y, x;
    for (y = 0; y < SCR_H; y++)
        for (x = 0; x < SCR_W; x++) { scrCh[y][x] = ' '; scrCo[y][x] = C_DEF; }
}

static void scPut(int y, int x, char ch, int col)
{
    if (y < 0 || y >= SCR_H || x < 0 || x >= SCR_W) return;
    scrCh[y][x] = ch;
    scrCo[y][x] = (unsigned char)col;
}

static void scText(int y, int x, const char* s, int col)
{
    while (*s) scPut(y, x++, *s++, col);
}

/* 场上坐标(格) -> 屏幕坐标；越界的实体直接丢掉，避免画到边框外 */
static void fieldPut(double fx, double fy, char ch, int col)
{
    int ix = (int)(fx + 0.5);
    int iy = (int)(fy + 0.5);
    if (ix < 0 || ix >= FIELD_W || iy < 0 || iy >= FIELD_H) return;
    scPut(FY + iy, FX + ix, ch, col);
}

/* ============================ 初始化 ==================================== */
static void spawnStar(Star* s, int anywhere)
{
    s->x = rndR(0, FIELD_W - 1);
    s->y = anywhere ? rndR(0, FIELD_H - 1) : 0;
    s->vy = rndR(3.0, 9.0);
    s->ch = (rnd() < 0.25) ? '*' : '.';
    s->col = (rnd() < 0.2) ? C_DIM : C_BLUE;
}

static void resetGame(void)
{
    int i;

    memset(pb, 0, sizeof pb);
    memset(eb, 0, sizeof eb);
    memset(en, 0, sizeof en);
    memset(pt, 0, sizeof pt);
    memset(cr, 0, sizeof cr);

    player.x = FIELD_W / 2 - 1;
    player.y = FIELD_H - 4;
    player.lives = 3;
    player.inv = 1.6;

    weapon = W_GUN;
    ammo = -1;
    fireTimer = 0;

    score = 0;
    kills = 0;
    levelNo = 1;
    spawnTimer = 0.6;
    spawnInterval = 0.95;
    crateTimer = 4.0;
    timeNow = 0;
    gameOver = 0;
    paused = 0;

    for (i = 0; i < MAX_STAR; i++) spawnStar(&st[i], 1);
}

/* ============================ 弹药箱 ==================================== */
static void dropCrate(double x, double y, int forceKind)
{
    int i, slot = -1;

    for (i = 0; i < MAX_CR; i++)
        if (!cr[i].on) { slot = i; break; }
    if (slot < 0) return;

    cr[slot].on = 1;
    cr[slot].x = x;
    cr[slot].y = y;
    cr[slot].vy = rndR(2.6, 3.6);
    cr[slot].kind = forceKind ? forceKind : (1 + rand() % (W_COUNT - 1));
}

static void spawnCrate(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_CR; i++) if (cr[i].on) n++;
    if (n >= 2) return;                     /* 场上最多同时 2 个 */
    dropCrate(rndR(2, FIELD_W - 5), -0.5, 0);
}

static void pickupCrate(int kind)
{
    weapon = kind;
    ammo = WEAPONS[kind].ammo;
    fireTimer = 0;                          /* 立刻能开火 */
    boom(player.x + 1, player.y, WEAPONS[kind].color);
}

/* ============================ 生成敌机 ================================== */
static void spawnEnemy(void)
{
    int i, slot = -1;
    double spd = 1.0 + (levelNo - 1) * 0.12;

    for (i = 0; i < MAX_EN; i++)
        if (!en[i].on) { slot = i; break; }
    if (slot < 0) return;

    en[slot].on = 1;
    en[slot].fireCd = rndR(0.6, 1.8);

    {
        double r = rnd();
        if (r < 0.66) {                     /* 小飞机：直下，快 */
            en[slot].type = 0;
            en[slot].x = rndR(0, FIELD_W - 1);
            en[slot].vy = rndR(9, 14) * spd;
            en[slot].vx = 0;
            en[slot].hp = 1;
        }
        else if (r < 0.87) {              /* 轰炸机：慢，血厚，会开火 */
            en[slot].type = 1;
            en[slot].x = rndR(1, FIELD_W - 4);
            en[slot].vy = rndR(4, 6) * spd;
            en[slot].vx = rndR(-2, 2);
            en[slot].hp = 3;
        }
        else {                            /* 俯冲机：斜着冲下来 */
            en[slot].type = 2;
            if (rnd() < 0.5) {
                en[slot].x = 0;
                en[slot].vx = rndR(6, 11) * spd;
            }
            else {
                en[slot].x = FIELD_W - 1;
                en[slot].vx = -rndR(6, 11) * spd;
            }
            en[slot].vy = rndR(7, 10) * spd;
            en[slot].hp = 1;
        }
        en[slot].y = 0;
    }
}

/* 加一发我方弹药 */
static void addShot(double x, double y, double vx, double vy,
    int kind, int dmg, double life)
{
    int i;
    for (i = 0; i < MAX_PB; i++) {
        if (!pb[i].on) {
            pb[i].on = 1;
            pb[i].x = x;
            pb[i].y = y;
            pb[i].vx = vx;
            pb[i].vy = vy;
            pb[i].kind = kind;
            pb[i].dmg = dmg;
            pb[i].life = life;
            return;
        }
    }
}

static void shootPlayer(void)
{
    double y = player.y - 0.4;

    switch (weapon) {
    case W_SHOT: {                          /* 霰弹：5 发扇形 */
        static const double fan[5] = { -26, -13, 0, 13, 26 };
        int i;
        for (i = 0; i < 5; i++)
            addShot(player.x + 1, y, fan[i], -52, SK_PELLET, 1, -1);
        break;
    }
    case W_FLAME:                           /* 喷火：短命火舌 */
        addShot(player.x + 1 + rndR(-0.6, 0.6), y,
            rndR(-8, 8), rndR(-40, -34), SK_FLAME, 1, rndR(0.45, 0.60));
        break;
    case W_HOME: {                          /* 追踪导弹：发射时先锁定最近的敌机 */
        double px = player.x + 1;
        double tx, ty, vx = 0, vy = -30;

        if (nearestEnemy(px, y, &tx, &ty)) {
            double dx = tx - px, dy = ty - y;
            double m = (dx < 0 ? -dx : dx);
            if ((dy < 0 ? -dy : dy) > m) m = (dy < 0 ? -dy : dy);
            if (m < 1.0) m = 1.0;
            vx = dx / m * 30.0;
            vy = dy / m * 30.0;
            if (vy > -6.0) vy = -6.0;       /* 别掉头往下飞 */
        }
        addShot(px, y, vx, vy, SK_MISSILE, 3, -1);
        break;
    }
    case W_RAPID:                           /* 速射，单发但极快 */
        addShot(player.x + 1, y, 0, -64, SK_BULLET, 1, -1);
        break;
    case W_MULTI: {                         /* 三向弹：并排 3 发 */
        int i;
        for (i = 0; i < 3; i++)
            addShot(player.x + i, y, 0, -56, SK_BULLET, 1, -1);
        break;
    }
    default:                                /* 默认机炮 */
        addShot(player.x + 1, y, 0, -58, SK_BULLET, 1, -1);
        break;
    }

    /* 扣弹药，打完自动换回机炮 */
    if (ammo > 0) {
        ammo--;
        if (ammo <= 0) { weapon = W_GUN; ammo = -1; }
    }
}

static void shootEnemy(double x, double y)
{
    int i;
    for (i = 0; i < MAX_EB; i++) {
        if (!eb[i].on) {
            eb[i].on = 1;
            eb[i].x = x;
            eb[i].y = y + 0.5;
            return;
        }
    }
}

/* ============================ 特效 ====================================== */
static void boom(double x, double y, int col)
{
    static const char pat[] = "*+.o";
    int i, n = 12;

    for (i = 0; i < MAX_PT && n > 0; i++) {
        if (pt[i].on) continue;
        pt[i].on = 1;
        pt[i].x = x;
        pt[i].y = y;
        pt[i].vx = rndR(-14, 14);
        pt[i].vy = rndR(-14, 14);
        pt[i].life = rndR(0.25, 0.55);
        pt[i].ch = pat[rand() % 4];
        pt[i].col = col;
        n--;
    }
}

/* ============================ 玩家受伤 ================================== */
static void playerHit(void)
{
    int i;

    if (player.inv > 0 || gameOver) return;

    boom(player.x + 1, player.y + 1, C_YELLOW);
    player.lives--;
    player.inv = 1.8;

    /* 清掉身边一圈敌弹，避免复活即死 */
    for (i = 0; i < MAX_EB; i++)
        if (eb[i].on && hitBox(eb[i].x, eb[i].y, 1, 1,
            player.x - 3, player.y - 3, 9, 9))
            eb[i].on = 0;

    if (player.lives <= 0) {
        player.lives = 0;
        gameOver = 1;
        if (score > hiScore) hiScore = score;
    }
}

/* 导弹寻找最近的敌机 */
static int nearestEnemy(double x, double y, double* ox, double* oy)
{
    int i, best = -1;
    double bestD = 1e30;

    for (i = 0; i < MAX_EN; i++) {
        double dx, dy, d;
        if (!en[i].on) continue;
        dx = en[i].x - x;
        dy = en[i].y - y;
        d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return 0;
    *ox = en[best].x;
    *oy = en[best].y;
    return 1;
}

/* ============================ 更新：我方弹药 ============================ */
static void updateShots(double dt)
{
    int i, j;

    for (i = 0; i < MAX_PB; i++) {
        Shot* s = &pb[i];
        if (!s->on) continue;

        /* ---- 运动 ---- */
        if (s->kind == SK_MISSILE) {
            double tx, ty;
            if (nearestEnemy(s->x, s->y, &tx, &ty)) {
                /* 朝目标方向加速（用较大的分量做归一化，避免开方依赖） */
                double dx = tx - s->x, dy = ty - s->y;
                double m = (dx < 0 ? -dx : dx);
                if ((dy < 0 ? -dy : dy) > m) m = (dy < 0 ? -dy : dy);
                if (m < 1.0) m = 1.0;
                s->vx += dx / m * 95.0 * dt;
                s->vy += dy / m * 95.0 * dt;
            }
            else {
                s->vx *= 0.98;              /* 没目标就直着飞 */
            }
            /* 限速要按方向等比缩放，单独截断某个分量会把弹道掰弯 */
            {
                double m = (s->vx < 0 ? -s->vx : s->vx);
                double n = (s->vy < 0 ? -s->vy : s->vy);
                if (n > m) m = n;
                if (m > 40.0) {
                    s->vx *= 40.0 / m;
                    s->vy *= 40.0 / m;
                }
            }
            s->x += s->vx * dt;
            s->y += s->vy * dt;
        }
        else if (s->kind == SK_FLAME) {
            s->life -= dt;
            if (s->life <= 0) { s->on = 0; continue; }
            s->x += s->vx * dt;
            s->y += s->vy * dt;
            s->vx *= (1.0 - 1.8 * dt);      /* 火舌越飞越慢，形成火柱 */
            s->vy *= (1.0 - 1.8 * dt);
        }
        else {
            s->x += s->vx * dt;
            s->y += s->vy * dt;
        }

        if (s->y < -1 || s->y > FIELD_H + 1 || s->x < -1 || s->x > FIELD_W + 1) {
            s->on = 0;
            continue;
        }

        /* ---- 打敌机 ---- */
        for (j = 0; j < MAX_EN; j++) {
            int ew;
            if (!en[j].on) continue;
            ew = (en[j].type == 1) ? 3 : 1;
            if (!hitBox(s->x, s->y, 1, 1, en[j].x, en[j].y, ew, 1)) continue;

            en[j].hp -= s->dmg;
            if (s->kind != SK_FLAME) s->on = 0;  /* 火舌能继续烧后面的敌机 */

            if (en[j].hp <= 0) {
                int type = en[j].type;
                en[j].on = 0;
                kills++;
                score += (type == 1) ? 40 : (type == 2 ? 20 : 10);
                boom(en[j].x + (ew - 1) / 2.0, en[j].y, C_YELLOW);
                if (type == 1 && rnd() < 0.35)   /* 轰炸机有概率掉弹药箱 */
                    dropCrate(en[j].x + 1, en[j].y, 0);
                if (kills % 20 == 0) {
                    levelNo++;
                    spawnInterval = 0.95 - (levelNo - 1) * 0.08;
                    if (spawnInterval < 0.30) spawnInterval = 0.30;
                }
            }
            break;
        }
    }
}

/* ============================ 更新：世界 ================================ */
static void updateWorld(double dt)
{
    int i;

    /* ---- 敌机 ---- */
    for (i = 0; i < MAX_EN; i++) {
        int ew;
        if (!en[i].on) continue;

        en[i].x += en[i].vx * dt;
        en[i].y += en[i].vy * dt;
        ew = (en[i].type == 1) ? 3 : 1;

        if (en[i].x < 0) { en[i].x = 0; en[i].vx = -en[i].vx; }
        if (en[i].x > FIELD_W - ew) { en[i].x = FIELD_W - ew; en[i].vx = -en[i].vx; }
        if (en[i].y > FIELD_H) { en[i].on = 0; continue; }

        if (en[i].type == 1) {
            en[i].fireCd -= dt;
            if (en[i].fireCd <= 0) {
                en[i].fireCd = rndR(1.4, 2.6) / (1.0 + (levelNo - 1) * 0.1);
                shootEnemy(en[i].x + 1, en[i].y + 1);
            }
        }

        if (hitBox(en[i].x, en[i].y, ew, 1, player.x, player.y, 3, 2)) {
            en[i].on = 0;
            boom(en[i].x, en[i].y, C_RED);
            playerHit();
        }
    }

    /* ---- 敌方子弹 ---- */
    for (i = 0; i < MAX_EB; i++) {
        if (!eb[i].on) continue;
        eb[i].y += 26.0 * dt;
        if (eb[i].y > FIELD_H) { eb[i].on = 0; continue; }

        if (player.inv <= 0 &&
            hitBox(eb[i].x, eb[i].y, 1, 1, player.x, player.y, 3, 2)) {
            eb[i].on = 0;
            playerHit();
        }
    }

    /* ---- 弹药箱：下落 + 拾取 ---- */
    for (i = 0; i < MAX_CR; i++) {
        if (!cr[i].on) continue;
        cr[i].y += cr[i].vy * dt;
        if (cr[i].y > FIELD_H) { cr[i].on = 0; continue; }

        if (hitBox(cr[i].x, cr[i].y, 3, 1, player.x, player.y, 3, 2)) {
            cr[i].on = 0;
            pickupCrate(cr[i].kind);
        }
    }

    /* ---- 碎片 ---- */
    for (i = 0; i < MAX_PT; i++) {
        if (!pt[i].on) continue;
        pt[i].life -= dt;
        if (pt[i].life <= 0) { pt[i].on = 0; continue; }
        pt[i].x += pt[i].vx * dt;
        pt[i].y += pt[i].vy * dt;
        pt[i].vx *= 0.94;
        pt[i].vy *= 0.94;
    }

    /* ---- 计时器 ---- */
    if (fireTimer > 0) fireTimer -= dt;

    spawnTimer -= dt;
    if (spawnTimer <= 0) {
        spawnEnemy();
        spawnTimer = spawnInterval * rndR(0.7, 1.2);
    }

    crateTimer -= dt;
    if (crateTimer <= 0) {
        spawnCrate();
        crateTimer = rndR(6.0, 10.0) - levelNo * 0.25;
        if (crateTimer < 3.5) crateTimer = 3.5;
    }
}

static void update(double dt)
{
    int i;

    if (gameOver || paused) {
        /* 结束时仍然让星星和碎片动，画面不死 */
        for (i = 0; i < MAX_STAR; i++) {
            st[i].y += st[i].vy * dt;
            if (st[i].y >= FIELD_H) spawnStar(&st[i], 0);
        }
        for (i = 0; i < MAX_PT; i++) {
            if (!pt[i].on) continue;
            pt[i].life -= dt;
            if (pt[i].life <= 0) { pt[i].on = 0; continue; }
            pt[i].x += pt[i].vx * dt;
            pt[i].y += pt[i].vy * dt;
            pt[i].vx *= 0.94;
            pt[i].vy *= 0.94;
        }
        return;
    }

    timeNow += dt;
    if (player.inv > 0) player.inv -= dt;

    for (i = 0; i < MAX_STAR; i++) {
        st[i].y += st[i].vy * dt * (1.0 + levelNo * 0.05);
        if (st[i].y >= FIELD_H) spawnStar(&st[i], 0);
    }

    updateShots(dt);
    updateWorld(dt);
}

/* ============================ 绘制 ====================================== */
static void drawHUD(void)
{
    char buf[80];
    int i;

    snprintf(buf, sizeof buf, "SCORE %06lld", score);
    scText(0, FX, buf, C_WHITE);

    snprintf(buf, sizeof buf, "HI %06lld", hiScore > score ? hiScore : score);
    scText(0, FX + 14, buf, C_DIM);

    snprintf(buf, sizeof buf, "LV %d", levelNo);
    scText(0, FX + 24, buf, C_YELLOW);

    scText(0, FX + 31, "LIVES", C_DIM);
    for (i = 0; i < 3; i++)
        scPut(0, FX + 37 + i * 2, i < player.lives ? 'A' : '-',
            i < player.lives ? C_GREEN : C_DIM);

    /* 当前武器 + 剩余弹药 */
    if (weapon == W_GUN) {
        scText(0, FX + 44, "GUN  --", C_DIM);
    }
    else {
        snprintf(buf, sizeof buf, "%s %02d", WEAPONS[weapon].name, ammo);
        scText(0, FX + 44, buf, WEAPONS[weapon].color);
    }
}

static void drawBorder(void)
{
    int y, x;
    int right = FX + FIELD_W;

    scPut(FY - 1, FX - 1, '+', C_DIM);
    scPut(FY + FIELD_H, FX - 1, '+', C_DIM);
    scPut(FY - 1, right, '+', C_DIM);
    scPut(FY + FIELD_H, right, '+', C_DIM);

    for (x = 0; x < FIELD_W; x++) {
        scPut(FY - 1, FX + x, '-', C_DIM);
        scPut(FY + FIELD_H, FX + x, '-', C_DIM);
    }
    for (y = 0; y < FIELD_H; y++) {
        scPut(FY + y, FX - 1, '|', C_DIM);
        scPut(FY + y, right, '|', C_DIM);
    }
}

static void drawStars(void)
{
    int i;
    for (i = 0; i < MAX_STAR; i++)
        fieldPut(st[i].x, st[i].y, st[i].ch, st[i].col);
}

/* 弹药箱：[S] [F] [H] [R] [M]，字母用武器自己的颜色 */
static void drawCrates(void)
{
    int i;
    for (i = 0; i < MAX_CR; i++) {
        const WeaponDef* w;
        if (!cr[i].on) continue;
        w = &WEAPONS[cr[i].kind];
        fieldPut(cr[i].x, cr[i].y, '[', C_DIM);
        fieldPut(cr[i].x + 1, cr[i].y, w->letter, w->color);
        fieldPut(cr[i].x + 2, cr[i].y, ']', C_DIM);
    }
}

static void drawEnemies(void)
{
    int i;
    for (i = 0; i < MAX_EN; i++) {
        if (!en[i].on) continue;
        switch (en[i].type) {
        case 0:
            fieldPut(en[i].x, en[i].y, 'v', C_GREEN);
            break;
        case 1:
            fieldPut(en[i].x, en[i].y, '\\', C_RED);
            fieldPut(en[i].x + 1, en[i].y, 'o', C_RED);
            fieldPut(en[i].x + 2, en[i].y, '/', C_RED);
            break;
        default:
            fieldPut(en[i].x, en[i].y, en[i].vx > 0 ? '>' : '<', C_YELLOW);
            break;
        }
    }
}

static void drawPlayer(void)
{
    /* 无敌时间闪烁 */
    if (player.inv > 0 && ((int)(player.inv * 10) & 1)) return;

    fieldPut(player.x, player.y, ' ', C_CYAN);
    fieldPut(player.x + 2, player.y, ' ', C_CYAN);
    fieldPut(player.x + 1, player.y, '^', C_WHITE);
    fieldPut(player.x, player.y + 1, '/', C_CYAN);
    fieldPut(player.x + 1, player.y + 1, 'A', C_CYAN);
    fieldPut(player.x + 2, player.y + 1, '\\', C_CYAN);
}

static void drawShots(void)
{
    int i;
    for (i = 0; i < MAX_PB; i++) {
        Shot* s = &pb[i];
        if (!s->on) continue;
        switch (s->kind) {
        case SK_PELLET:
            fieldPut(s->x, s->y, '\'', C_WHITE);
            break;
        case SK_MISSILE:
            fieldPut(s->x, s->y, '^', C_YELLOW);
            break;
        case SK_FLAME:
            fieldPut(s->x, s->y,
                s->life > 0.2 ? '~' : '*',
                s->life > 0.2 ? C_YELLOW : C_RED);
            break;
        default:
            fieldPut(s->x, s->y, '|', C_CYAN);
            break;
        }
    }

    for (i = 0; i < MAX_EB; i++)
        if (eb[i].on) fieldPut(eb[i].x, eb[i].y, '*', C_RED);
}

static void drawParticles(void)
{
    int i;
    for (i = 0; i < MAX_PT; i++) {
        if (!pt[i].on) continue;
        fieldPut(pt[i].x, pt[i].y,
            pt[i].life > 0.18 ? pt[i].ch : '.',
            pt[i].life > 0.18 ? pt[i].col : C_DIM);
    }
}

/* 战场中央的提示框：三行文字，逐行居中 */
static void drawBox(int top, const char* l1, const char* l2, const char* l3, int col)
{
    int x, y, w = 32;
    int left = FX + (FIELD_W - w) / 2;

    for (y = 0; y <= 5; y++)
        for (x = 0; x < w; x++) scPut(top + y, left + x, ' ', C_DEF);

    for (x = 0; x < w; x++) {
        scPut(top, left + x, '-', C_DIM);
        scPut(top + 5, left + x, '-', C_DIM);
    }
    for (y = 0; y <= 5; y++) {
        scPut(top + y, left, '|', C_DIM);
        scPut(top + y, left + w - 1, '|', C_DIM);
    }
    scPut(top, left, '+', C_DIM);
    scPut(top, left + w - 1, '+', C_DIM);
    scPut(top + 5, left, '+', C_DIM);
    scPut(top + 5, left + w - 1, '+', C_DIM);

    if (l1 && *l1) scText(top + 1, left + (w - (int)strlen(l1)) / 2, l1, col);
    if (l2 && *l2) scText(top + 2, left + (w - (int)strlen(l2)) / 2, l2, C_WHITE);
    if (l3 && *l3) scText(top + 3, left + (w - (int)strlen(l3)) / 2, l3, C_DIM);
}

static void drawOverlay(void)
{
    char buf[48];

    if (paused && !gameOver) {
        drawBox(8, "P A U S E D", "", "press P to resume", C_YELLOW);
    }
    else if (gameOver) {
        snprintf(buf, sizeof buf, "SCORE  %lld", score);
        drawBox(8, "G A M E  O V E R", buf, "press R to restart", C_RED);
    }
}

/* ============================ 平台层 ==================================== */
#ifdef _WIN32

static HANDLE hOut;

static double nowSec(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static void sleepMs(int ms) { Sleep((DWORD)(ms > 0 ? ms : 0)); }

static int keyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void pollTapKeys(void)
{
    int c;
    while (_kbhit()) {
        c = _getch();
        if (c == 0 || c == 224) { _getch(); continue; }   /* 吞掉方向键 */
        if (c == 27) { quitGame = 1; return; }
        if (c == 'p' || c == 'P') {
            if (!gameOver) paused = !paused;
        }
        else if (c == 'r' || c == 'R') {
            if (gameOver) resetGame();
        }
    }
}

static void handleInput(double dt)
{
    const double spd = 34.0;
    double dx = 0, dy = 0;

    if (keyHeld(VK_LEFT))  dx -= spd * dt;
    if (keyHeld(VK_RIGHT)) dx += spd * dt;
    if (keyHeld(VK_UP))    dy -= spd * dt;
    if (keyHeld(VK_DOWN))  dy += spd * dt;

    player.x += dx;
    player.y += dy;

    if (player.x < 0)           player.x = 0;
    if (player.x > FIELD_W - 3) player.x = FIELD_W - 3;
    if (player.y < 1)           player.y = 1;
    if (player.y > FIELD_H - 2) player.y = FIELD_H - 2;

    if (keyHeld(VK_SPACE) && fireTimer <= 0) {
        shootPlayer();
        fireTimer = WEAPONS[weapon].interval;
    }
}

static void present(void)
{
    static CHAR_INFO buf[SCR_H][SCR_W];
    static SMALL_RECT region = { 0, 0, SCR_W - 1, SCR_H - 1 };
    static COORD      size = { SCR_W, SCR_H };
    static COORD      origin = { 0, 0 };
    int y, x;

    for (y = 0; y < SCR_H; y++) {
        for (x = 0; x < SCR_W; x++) {
            buf[y][x].Char.AsciiChar = scrCh[y][x];
            buf[y][x].Attributes = scrCo[y][x];
        }
    }
    WriteConsoleOutputA(hOut, &buf[0][0], size, origin, &region);
}

static void printLineW(int row, const wchar_t* s)
{
    COORD p;
    DWORD n;
    p.X = 0;
    p.Y = (SHORT)row;
    SetConsoleCursorPosition(hOut, p);
    WriteConsoleW(hOut, s, (DWORD)wcslen(s), &n, NULL);
}

static void setupConsole(void)
{
    CONSOLE_CURSOR_INFO ci;
    COORD bs;
    SMALL_RECT win;

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    win.Left = 0; win.Top = 0; win.Right = 1; win.Bottom = 1;
    SetConsoleWindowInfo(hOut, TRUE, &win);

    bs.X = SCR_W;
    bs.Y = SCR_H + 4;
    SetConsoleScreenBufferSize(hOut, bs);

    win.Left = 0; win.Top = 0;
    win.Right = (SHORT)(SCR_W - 1);
    win.Bottom = (SHORT)(SCR_H + 3);
    SetConsoleWindowInfo(hOut, TRUE, &win);

    SetConsoleTitleA("Sky Fighter - C Console Edition");

    ci.dwSize = 1;
    ci.bVisible = FALSE;
    SetConsoleCursorInfo(hOut, &ci);
}

static void restoreConsole(void)
{
    CONSOLE_CURSOR_INFO ci;
    ci.dwSize = 25;
    ci.bVisible = TRUE;
    SetConsoleCursorInfo(hOut, &ci);
    SetConsoleTextAttribute(hOut, C_DEF);
}

#else   /* ---------------------------- POSIX --------------------------------- */

static struct termios gOrig;
static int gSaved = 0;
static int gFireHold = 0;

static double nowSec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleepMs(int ms) { if (ms > 0) usleep((useconds_t)ms * 1000); }

static void pollTapKeys(void)
{
    unsigned char b;
    if (read(STDIN_FILENO, &b, 1) != 1) return;

    if (b == 27) {
        unsigned char s[2];
        fd_set rs;
        struct timeval tv = { 0, 20000 };
        FD_ZERO(&rs);
        FD_SET(STDIN_FILENO, &rs);
        if (select(STDIN_FILENO + 1, &rs, NULL, NULL, &tv) <= 0) { quitGame = 1; return; }
        if (read(STDIN_FILENO, &s[0], 1) != 1) { quitGame = 1; return; }
        if (s[0] == '[' && read(STDIN_FILENO, &s[1], 1) == 1) {
            if (!gameOver && !paused) {
                switch (s[1]) {
                case 'A': player.y -= 2; break;
                case 'B': player.y += 2; break;
                case 'C': player.x += 3; break;
                case 'D': player.x -= 3; break;
                }
                if (player.x < 0)           player.x = 0;
                if (player.x > FIELD_W - 3) player.x = FIELD_W - 3;
                if (player.y < 1)           player.y = 1;
                if (player.y > FIELD_H - 2) player.y = FIELD_H - 2;
            }
        }
        return;
    }

    if (b == 'p' || b == 'P') { if (!gameOver) paused = !paused; }
    else if (b == 'r' || b == 'R') { if (gameOver) resetGame(); }
    else if (b == ' ') { gFireHold = !gFireHold; }   /* 空格切换连射 */
}

static void handleInput(double dt)
{
    (void)dt;
    if (gFireHold && fireTimer <= 0) {
        shootPlayer();
        fireTimer = WEAPONS[weapon].interval;
    }
}

static const char* ansiOf(int c)
{
    switch (c) {
    case 8:  return "\033[90m";
    case 9:  return "\033[94m";
    case 10: return "\033[92m";
    case 11: return "\033[96m";
    case 12: return "\033[91m";
    case 13: return "\033[95m";
    case 14: return "\033[93m";
    case 15: return "\033[97m";
    default: return "\033[37m";
    }
}

static void present(void)
{
    int y, x, last = -1;
    fputs("\033[H", stdout);
    for (y = 0; y < SCR_H; y++) {
        for (x = 0; x < SCR_W; x++) {
            if (scrCo[y][x] != last) {
                last = scrCo[y][x];
                fputs(ansiOf(last), stdout);
            }
            putchar(scrCh[y][x]);
        }
        putchar('\n');
    }
    fputs("\033[0m", stdout);
    fflush(stdout);
}

static void printLineW(int row, const wchar_t* s)
{
    char buf[512];
    int n = (int)wcstombs(buf, s, sizeof buf - 1);
    if (n < 0) return;
    buf[n] = 0;
    printf("\033[%d;1H%s", row + 1, buf);
    fflush(stdout);
}

static void setupConsole(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &gOrig) == 0) {
        gSaved = 1;
        t = gOrig;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    fputs("\033[?25l\033[2J", stdout);
    setlocale(LC_ALL, "");
}

static void restoreConsole(void)
{
    if (gSaved) tcsetattr(STDIN_FILENO, TCSANOW, &gOrig);
    fputs("\033[?25h\033[0m\n", stdout);
}

#endif

/* ============================ 说明文字 ================================== */
static void printHelp(void)
{
    static const wchar_t h1[] =
        L"← → ↑ ↓ 移动    空格 开火（按住连射）    P 暂停    R 重开    ESC 退出";
    static const wchar_t h2[] =
        L"弹药箱（飘在战局里，撞上去拾取）：[S]霰弹枪  [F]喷火器  [H]追踪导弹  [R]速射  [M]三向弹";
    static const wchar_t h3[] =
        L"击落：小飞机+10  俯冲机+20  轰炸机+40（有概率掉弹药箱）· 每 20 架升一级";

    printLineW(SCR_H, h1);
    printLineW(SCR_H + 1, h2);
    printLineW(SCR_H + 2, h3);
}

/* ============================ 主循环 ==================================== */
static void render(void)
{
    scClear();
    drawHUD();
    drawBorder();
    drawStars();
    drawCrates();
    drawEnemies();
    drawShots();
    if (!gameOver) drawPlayer();
    drawParticles();
    drawOverlay();
    present();
}

int main(void)
{
    double last, now, dt;

    srand((unsigned)time(NULL));
    setupConsole();
    resetGame();

    render();
    printHelp();

    last = nowSec();

    while (!quitGame) {
        now = nowSec();
        dt = now - last;
        last = now;
        if (dt > 0.1) dt = 0.1;         /* 卡顿保护 */

        pollTapKeys();
        if (quitGame) break;

        if (!paused && !gameOver) handleInput(dt);

        update(dt);
        render();

        sleepMs(16);
    }

    restoreConsole();
    {
        wchar_t buf[160];
        swprintf(buf, 160, L"游戏结束。本局得分：%lld   击落：%d 架   最高：%lld\n",
            score, kills, hiScore);
        printLineW(SCR_H + 3, buf);
    }
    return 0;
}
