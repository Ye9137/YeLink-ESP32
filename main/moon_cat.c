// Copyright (c) 2026 Ye
// 按用户参考图重绘：米棕色宽脸、白色肚皮、深棕描边、蓝色竖眼及弯曲举爪。
// 使用原生路径绘图，无位图素材和额外运行库，可在 ESP32 上连续变形。
#include "moon_cat.h"
#include "display.h"
#include <math.h>
#include <stddef.h>

typedef struct { float x, y; } point_t;
static uint8_t *canvas;
static int shift_x, shift_y;
static int custom_open=100, custom_sway=1, custom_paw=0;
static uint16_t custom_eye;
static bool custom_style;
void moon_cat_set_style(int eye_open,int sway,int paw,uint16_t eye_color)
{
    custom_open=eye_open; custom_sway=sway; custom_paw=paw;
    custom_eye=eye_color; custom_style=true;
}
#define RGB565(r,g,b) ((((r)&248)<<8) | (((g)&252)<<3) | ((b)>>3))
static const uint16_t BACKGROUND=RGB565(255,253,249);
static const uint16_t OUTLINE=RGB565(106,67,52);
static const uint16_t FUR=RGB565(207,181,149);
static const uint16_t WHITE=RGB565(255,255,255);
static const uint16_t BLUSH=RGB565(237,167,153);

// 绘图限制在动画区域内，保证局部刷新不会遗留区域外的旧像素。
static void dot(int x,int y,uint16_t color)
{
    x+=shift_x; y+=shift_y;
    if (x<0 || x>=SCREEN_WIDTH || y<16 || y>=120) return;
    size_t offset=(y*SCREEN_WIDTH+x)*2;
    canvas[offset]=color>>8; canvas[offset+1]=color&255;
}
static void ellipse(int cx,int cy,int rx,int ry,uint16_t color)
{
    for (int y=-ry;y<=ry;y++) for (int x=-rx;x<=rx;x++)
        if (x*x*ry*ry+y*y*rx*rx<=rx*rx*ry*ry) dot(cx+x,cy+y,color);
}
// 圆头线段保持参考图的粗线手绘感，短线也不会出现尖锐端点。
static void line(point_t a,point_t b,int radius,uint16_t color)
{
    int steps=(int)ceilf(fmaxf(fabsf(b.x-a.x),fabsf(b.y-a.y)));
    if (steps<1) steps=1;
    for (int i=0;i<=steps;i++) {
        float t=(float)i/steps;
        ellipse(lroundf(a.x+(b.x-a.x)*t),lroundf(a.y+(b.y-a.y)*t),radius,radius,color);
    }
}
static void stroke(const point_t *points,int count,int radius,uint16_t color,bool closed)
{
    for (int i=1;i<count;i++) line(points[i-1],points[i],radius,color);
    if (closed) line(points[count-1],points[0],radius,color);
}
// 扫描线填充支持白色额纹等非凸轮廓；不需要整屏双倍分辨率缓冲。
static void fill(const point_t *points,int count,uint16_t color)
{
    for (int y=16-shift_y;y<120-shift_y;y++) {
        float intersections[64]; int n=0;
        for (int i=0,j=count-1;i<count;j=i++) {
            point_t a=points[j],b=points[i]; float scan=y+0.5f;
            if ((a.y<=scan && b.y>scan) || (b.y<=scan && a.y>scan))
                intersections[n++]=a.x+(scan-a.y)*(b.x-a.x)/(b.y-a.y);
        }
        for (int i=1;i<n;i++) {
            float v=intersections[i]; int j=i-1;
            while (j>=0 && intersections[j]>v) { intersections[j+1]=intersections[j]; j--; }
            intersections[j+1]=v;
        }
        for (int i=0;i+1<n;i+=2)
            for (int x=(int)ceilf(intersections[i]);x<=(int)floorf(intersections[i+1]);x++) dot(x,y,color);
    }
}
// 一段二次贝塞尔曲线采样为 17 点；先画粗描边，再画较细填色。
static void curve(point_t a,point_t control,point_t b,int radius,uint16_t color)
{
    point_t points[17];
    for (int i=0;i<=16;i++) {
        float t=i/16.0f,u=1-t;
        points[i]=(point_t){u*u*a.x+2*u*t*control.x+t*t*b.x,u*u*a.y+2*u*t*control.y+t*t*b.y};
    }
    stroke(points,17,radius,color,false);
}
static void paw(bool right,float lift)
{
    // 用连贯的三段曲线形成弯着的小手，抬爪时只改变控制点。
    point_t a=right?(point_t){91,99}:(point_t){42,90};
    point_t b=right?(point_t){87,76-lift}:(point_t){58,70-lift};
    point_t c=right?(point_t){78,74-lift}:(point_t){66,72-lift};
    point_t d=right?(point_t){80,82-lift}:(point_t){53,91-lift};
    for (int pass=0;pass<2;pass++) {
        int radius=pass?3:5; uint16_t color=pass?WHITE:OUTLINE;
        curve(a,right?(point_t){94,84-lift}:(point_t){47,75-lift},b,radius,color);
        curve(b,right?(point_t){78,69-lift}:(point_t){65,66-lift},c,radius,color);
        curve(c,right?(point_t){76,79-lift}:(point_t){55,77-lift},d,radius,color);
    }
}
static void heart(int cx,int cy,int size,uint16_t color)
{
    ellipse(cx-size/2,cy,size/2,size/2,color);
    ellipse(cx+size/2,cy,size/2,size/2,color);
    point_t triangle[]={{cx-size,cy},{cx+size,cy},{cx,cy+size+1}};
    fill(triangle,3,color);
}

void moon_cat_render(uint8_t *pixels,int64_t now_us,int expression,int look,int theme,bool blink_requested)
{
    static int64_t previous_us,next_blink,blink_start=-1000000;
    static float gaze_x,gaze_y;
    static const uint16_t eyes[]={RGB565(58,98,163),RGB565(184,89,128),RGB565(181,122,48),
        RGB565(61,133,113),RGB565(132,96,165)};
    canvas=pixels;
    // 从 GIF 切回互动模式会重置动画时钟，旧眨眼时刻和视线插值需同步复位。
    if (now_us<previous_us) { previous_us=0; next_blink=0; blink_start=-1000000; gaze_x=0; gaze_y=0; }
    for (int i=0;i<SCREEN_BYTES;i+=2) { canvas[i]=BACKGROUND>>8; canvas[i+1]=BACKGROUND&255; }
    float seconds=(now_us%600000000)/1000000.0f;
    float dt=previous_us?fminf(0.1f,(now_us-previous_us)/1000000.0f):0.016f;
    previous_us=now_us;
    shift_x=lroundf(sinf(seconds*1.5f)*custom_sway);
    shift_y=lroundf(sinf(seconds*(expression==1?4.2f:2.0f))*(expression==1?2:1)*custom_sway);
    float tx=look==2?-3:look==3?3:look==0?2*sinf(seconds*0.8f):0;
    float ty=look==4?-2:look==5?2:0;
    gaze_x+=(tx-gaze_x)*fminf(1,dt*8); gaze_y+=(ty-gaze_y)*fminf(1,dt*8);
    if (!next_blink) next_blink=now_us+2300000;
    if (blink_requested || now_us>=next_blink) {
        blink_start=now_us;
        next_blink=now_us+3300000+(int64_t)(700000*(1+sinf(seconds)));
    }
    int64_t elapsed=now_us-blink_start;
    float open=elapsed>=0 && elapsed<100000?1-elapsed/100000.0f:
        elapsed>=100000 && elapsed<140000?0:elapsed>=140000 && elapsed<260000?(elapsed-140000)/120000.0f:1;

    // 尾巴先画在身后，米棕填色和两道棕纹随节拍轻轻摆动。
    float wag=3*sinf(seconds*(expression==1?5:2));
    point_t tail_a={46,101},tail_c={34,113+wag/2},tail_b={27,109+wag};
    curve(tail_a,tail_c,tail_b,6,OUTLINE); curve(tail_a,tail_c,tail_b,3,FUR);
    line((point_t){32,104+wag},(point_t){29,109+wag},1,OUTLINE);
    line((point_t){39,103},(point_t){37,109},1,OUTLINE);

    // 白肚皮收窄到身体，底部轻微凹起，避免把角色画成只有一张脸。
    point_t body[]={{40,65},{86,62},{94,77},{94,98},{91,113},{79,107},{66,105},
        {59,106},{52,111},{47,111},{43,108},{41,96}};
    fill(body,12,WHITE); stroke(body,12,2,OUTLINE,true);
    // 不对称尖耳与宽脸是参考图的重要轮廓，额头不画成圆脑袋。
    point_t head[]={{16,69},{28,49},{28,32},{32,29},{46,37},{76,35},{82,25},
        {86,25},{96,41},{111,58},{113,65},{107,78},{92,79},{79,73},{48,79},{25,80}};
    fill(head,16,FUR);
    stroke(head,10,2,OUTLINE,false);
    // 保留两侧蓬松毛边，中间与白肚皮连成一体。
    line((point_t){17,72},(point_t){15,75},2,OUTLINE);
    line((point_t){23,76},(point_t){22,81},2,OUTLINE);
    line((point_t){22,81},(point_t){39,81},2,OUTLINE);
    line((point_t){111,67},(point_t){114,71},2,OUTLINE);
    line((point_t){107,75},(point_t){109,79},2,OUTLINE);
    line((point_t){109,79},(point_t){94,81},2,OUTLINE);
    // 从额头延伸到胸口的白色倒 V 花纹。
    point_t blaze[]={{43,80},{48,65},{54,49},{59,44},{63,43},{67,46},{74,62},{82,78},{85,98},{46,99}};
    fill(blaze,10,WHITE);

    uint16_t eye=custom_style?custom_eye:eyes[theme>=0 && theme<5?theme:0];
    for (int side=0;side<2;side++) {
        int cx=50+side*29+lroundf(gaze_x),cy=60+lroundf(gaze_y);
        int h=(int)((expression==2?4:expression==3?10:8)*open*custom_open/100);
        if (h<=1) line((point_t){cx-3,cy},(point_t){cx+3,cy},1,eye);
        else if (expression==1) curve((point_t){cx-4,cy+2},(point_t){cx,cy-5},(point_t){cx+4,cy+2},1,eye);
        else {
            // 圆头竖条更贴近参考图的蓝眼睛，困困状态压扁成短线。
            int radius=expression==3?4:expression==2?1:3;
            if (h<radius) radius=h;
            line((point_t){cx,cy-h+radius},(point_t){cx,cy+h-radius},radius,eye);
        }
    }
    if (expression==1) {
        ellipse(35,71,5,2,BLUSH); ellipse(96,70,5,2,BLUSH);
        heart(13,43+(int)(2*sinf(seconds*3)),4,BLUSH);
        heart(114,31+(int)(2*cosf(seconds*3)),3,BLUSH);
    }
    if (expression==3) { ellipse(65,76,3,4,OUTLINE); ellipse(65,76,1,2,WHITE); }
    // 原图的双爪贴脸动作作为默认姿态；其他表情改变抬爪节奏与幅度。
    float lift=expression==1?5+3*sinf(seconds*5):expression==2?-5:expression==3?5:1.5f*sinf(seconds*2.6f);
    paw(false,lift+custom_paw); paw(true,(expression==1?5-3*sinf(seconds*5):lift*0.7f)+custom_paw);
    if (expression==2) {
        point_t z[]={{99,39},{105,39},{99,45},{105,45}};
        stroke(z,4,1,eye,false);
    }
}
