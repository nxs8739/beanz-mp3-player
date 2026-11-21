/* BEANZ MP3 Player - milestone 0.11: MP3 decode and ASND playback.
 * MIT license; bundled minimp3 decoder is CC0.
 * Target: devkitPPC + libogc 3.2.1 + libfat.
 */
#include <gccore.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mp3_audio.h"

#define MAX_ITEMS 128
#define NAME_SIZE 256
#define VISIBLE 7

typedef struct { char name[NAME_SIZE]; int directory; } Item;
static Item items[MAX_ITEMS];
static int count=0, selected=0, top=0;
static char cwd[512] = "sd:/";
/* Never navigate above the volume selected at startup. */
static char mount_root[16] = "sd:/";
static char status[128] = "Choose a folder or MP3 file";
static int showing_player=0;
static char chosen_mp3[NAME_SIZE] = "";
static int chosen_track = 0;
static unsigned int marquee_frames = 0;
static int marquee_offset = 0;
static void *xfb;
static GXRModeObj *vmode;
extern const unsigned char beanz_wallpaper[640*480*2];
extern const unsigned char beanz_browser_wallpaper[640*480*2];

/* Genesis Plus GX-inspired approach: GX textured background and IPL glyphs.
 * Original implementation; no Genesis Plus GX source is copied.
 * The existing browser coordinates and seven-row layout remain unchanged. */
#define LIST_X 30
#define LIST_Y 185
#define LIST_WIDTH 337
#define LIST_ROW_HEIGHT 24

static u8 gx_fifo[GX_FIFO_MINSIZE] ATTRIBUTE_ALIGN(32);
static u16 *browser_texture = NULL;
static u16 *player_texture = NULL;
static GXTexObj browser_texobj, player_texobj, glyph_texobj;
static sys_fontheader *ipl_header = NULL;
static u8 *glyph_texture = NULL;
static u8 *glyph_bitmap = NULL;
static int glyph_tw = 0, glyph_th = 0;
static int font_ready = 0;

/* Convert the existing 640x480 YUYV wallpaper into GX's 4x4 tiled RGB565.
 * This is done once at startup, not during file navigation. */
static u16 *make_wallpaper_texture(const u8 *yuyv) {
    u16 *dst = memalign(32, 640 * 480 * 2);
    if (!dst) return NULL;
    for (int ty = 0; ty < 480; ty += 4)
        for (int tx = 0; tx < 640; tx += 4)
            for (int py = 0; py < 4; ++py)
                for (int px = 0; px < 4; ++px) {
                    int x = tx + px, y = ty + py;
                    const u8 *pair = yuyv + (y * 640 + (x & ~1)) * 2;
                    int Y = pair[(x & 1) ? 2 : 0];
                    int U = (int)pair[1] - 128, V = (int)pair[3] - 128;
                    int c = Y - 16;
                    if (c < 0) c = 0;
                    int r = (298*c + 409*V + 128) >> 8;
                    int g = (298*c - 100*U - 208*V + 128) >> 8;
                    int b = (298*c + 516*U + 128) >> 8;
                    if (r < 0) r = 0;
                    if (r > 255) r = 255;
                    if (g < 0) g = 0;
                    if (g > 255) g = 255;
                    if (b < 0) b = 0;
                    if (b > 255) b = 255;
                    int tile = ((ty / 4) * (640 / 4) + tx / 4) * 16;
                    dst[tile + py * 4 + px] = ((r & 0xf8) << 8) |
                                                ((g & 0xfc) << 3) | (b >> 3);
                }
    DCFlushRange(dst, 640 * 480 * 2);
    return dst;
}

static void gx_quad(int x0, int y0, int x1, int y1, GXColor color) {
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2s16(x0, y0); GX_Color4u8(color.r,color.g,color.b,color.a); GX_TexCoord2f32(0,0);
    GX_Position2s16(x1, y0); GX_Color4u8(color.r,color.g,color.b,color.a); GX_TexCoord2f32(1,0);
    GX_Position2s16(x1, y1); GX_Color4u8(color.r,color.g,color.b,color.a); GX_TexCoord2f32(1,1);
    GX_Position2s16(x0, y1); GX_Color4u8(color.r,color.g,color.b,color.a); GX_TexCoord2f32(0,1);
    GX_End();
}

static int init_gx_browser(void) {
    browser_texture = make_wallpaper_texture(beanz_browser_wallpaper);
    player_texture = make_wallpaper_texture(beanz_wallpaper);
    if (!browser_texture || !player_texture) return 0;

    GX_Init(gx_fifo, sizeof(gx_fifo));
    GX_SetViewport(0,0,vmode->fbWidth,vmode->efbHeight,0,1);
    GX_SetScissor(0,0,vmode->fbWidth,vmode->efbHeight);
    GX_SetDispCopySrc(0,0,vmode->fbWidth,vmode->efbHeight);
    GX_SetDispCopyDst(vmode->fbWidth,vmode->xfbHeight);
    GX_SetDispCopyYScale(GX_GetYScaleFactor(vmode->efbHeight,vmode->xfbHeight));
    GX_SetCopyFilter(vmode->aa, vmode->sample_pattern, GX_FALSE, vmode->vfilter);
    GX_SetFieldMode(vmode->field_rendering,
        vmode->viHeight == 2 * vmode->xfbHeight ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(GX_PF_RGB8_Z24,GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE,GX_ALWAYS,GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND,GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_LO_CLEAR);
    GX_SetCopyClear((GXColor){0,0,0,255},GX_MAX_Z24);
    Mtx44 proj;
    guOrtho(proj,0,480,0,640,0,1);
    GX_LoadProjectionMtx(proj,GX_ORTHOGRAPHIC);
    Mtx identity;
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity,GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0,GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XY,GX_S16,0);
    GX_SetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
    GX_SetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0,GX_DISABLE,GX_SRC_REG,GX_SRC_VTX,GX_LIGHTNULL,GX_DF_NONE,GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0,GX_MODULATE);
    GX_InitTexObj(&browser_texobj,browser_texture,640,480,GX_TF_RGB565,GX_CLAMP,GX_CLAMP,GX_FALSE);
    GX_InitTexObj(&player_texobj,player_texture,640,480,GX_TF_RGB565,GX_CLAMP,GX_CLAMP,GX_FALSE);
    GX_InitTexObjLOD(&browser_texobj,GX_NEAR,GX_NEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);
    GX_InitTexObjLOD(&player_texobj,GX_NEAR,GX_NEAR,0,0,0,GX_FALSE,GX_FALSE,GX_ANISO_1);

    /* IPL glyphs are intensity-only. Convert their 4-bit coverage into
     * GX IA4: white intensity + coverage as alpha, in GX 8x4 tile order.
     * This preserves the wallpaper behind the letters. */
    ipl_header = memalign(32, SYS_FONTSIZE_ANSI);
    if (ipl_header && SYS_InitFont(ipl_header)) {
        int cw = ipl_header->cell_width, ch = ipl_header->cell_height;
        if (cw > 0 && ch > 0 && cw <= 64 && ch <= 64) {
            glyph_tw = (cw + 7) & ~7;
            glyph_th = (ch + 3) & ~3;
            /* SYS_GetFontTexel writes I4 in 8x8 GX tiles, not linear rows. */
            glyph_bitmap = memalign(32, glyph_tw * ((ch + 7) & ~7) / 2);
            glyph_texture = memalign(32, glyph_tw * glyph_th);
            if (glyph_bitmap && glyph_texture) {
                memset(glyph_texture, 0, glyph_tw * glyph_th);
                GX_InitTexObj(&glyph_texobj,glyph_texture,glyph_tw,glyph_th,
                              GX_TF_IA4,GX_CLAMP,GX_CLAMP,GX_FALSE);
                GX_InitTexObjLOD(&glyph_texobj,GX_NEAR,GX_NEAR,0,0,0,
                                 GX_FALSE,GX_FALSE,GX_ANISO_1);
                font_ready = 1;
            }
        }
    }
    return 1;
}

static void bounded_text(int x, int y, int limit, const char *str) {
    if (!font_ready || !str) return;
    int cw = ipl_header->cell_width, ch = ipl_header->cell_height;
    int stride = glyph_tw / 2;
    int source_bytes = glyph_tw * ((ch + 7) & ~7) / 2;
    const int height = 18;
    while (*str && x < limit) {
        int width = 0;
        u8 c = (u8)*str++;
        if (c < 32 || c > 126) c = '?';
        memset(glyph_bitmap,0,source_bytes);
        SYS_GetFontTexel(c,glyph_bitmap,0,stride,&width);
        int advance = (width * height + ch / 2) / ch;
        if (advance < 5) advance = 5;
        if (x + advance > limit) break;

        /* Each source nibble is coverage; IA4 high nibble = alpha,
         * low nibble = intensity (white). GX IA4 tiles are 8x4. */
        memset(glyph_texture,0,glyph_tw * glyph_th);
        for (int gy = 0; gy < ch; ++gy)
            for (int gx = 0; gx < cw; ++gx) {
                /* I4: 8x8 tiles, 32 bytes each, two pixels per byte. */
                int source_tile = ((gy / 8) * (glyph_tw / 8) + gx / 8) * 32;
                u8 packed = glyph_bitmap[source_tile + (gy % 8) * 4 + (gx % 8) / 2];
                u8 alpha = (gx & 1) ? (packed & 15) : (packed >> 4);
                int tile = ((gy / 4) * (glyph_tw / 8) + gx / 8) * 32;
                glyph_texture[tile + (gy % 4) * 8 + gx % 8] =
                    (alpha << 4) | 15;
            }
        DCFlushRange(glyph_texture,glyph_tw * glyph_th);
        GX_InvalidateTexAll();
        GX_LoadTexObj(&glyph_texobj,GX_TEXMAP0);
        /* Sample the glyph cell without stretching the padded edges. */
        GX_Begin(GX_QUADS,GX_VTXFMT0,4);
        f32 u = (f32)cw / glyph_tw, v = (f32)ch / glyph_th;
        GX_Position2s16(x,y); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(0,0);
        GX_Position2s16(x+advance,y); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u,0);
        GX_Position2s16(x+advance,y+height); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(u,v);
        GX_Position2s16(x,y+height); GX_Color4u8(255,255,255,255); GX_TexCoord2f32(0,v);
        GX_End();
        GX_DrawDone(); /* finish before reusing this texture buffer */
        x += advance;
    }
}


/* The proven IPL/GX renderer is unchanged; only its right-hand text
 * boundary is made selectable so it can serve both screens. */
static void browser_text(int x, int y, const char *str) {
    bounded_text(x, y, LIST_X + LIST_WIDTH, str);
}

/* Playback text stays within the open space left of the GameCube artwork.
 * Scroll by whole glyphs, never allowing a glyph past the right boundary. */
#define PLAYER_TEXT_X 27
#define PLAYER_TEXT_RIGHT 294
#define PLAYER_NAME_Y 270
#define PLAYER_TRACK_Y 315
#define PLAYER_TIME_Y 347

static void draw_player_info(void) {
    char line[48];
    const char *name = chosen_mp3;
    int length = (int)strlen(name);
    if (length > 0 && marquee_offset < length)
        name += marquee_offset;
    bounded_text(PLAYER_TEXT_X, PLAYER_NAME_Y, PLAYER_TEXT_RIGHT, name);
    snprintf(line, sizeof(line), "Track %d", chosen_track);
    bounded_text(PLAYER_TEXT_X, PLAYER_TRACK_Y, PLAYER_TEXT_RIGHT, line);
    unsigned seconds = mp3_audio_elapsed_seconds();
    unsigned total = mp3_audio_total_seconds();
    if (total)
        snprintf(line, sizeof(line), "%02u:%02u / %02u:%02u", seconds / 60, seconds % 60, total / 60, total % 60);
    else
        snprintf(line, sizeof(line), "%02u:%02u / --:--", seconds / 60, seconds % 60);
    bounded_text(PLAYER_TEXT_X, PLAYER_TIME_Y, PLAYER_TEXT_RIGHT, line);
}

static void advance_player_marquee(void) {
    int length = (int)strlen(chosen_mp3);
    /* Short filenames already fit: do not scroll them. */
    if (length <= 22) return;
    /* Approximately one character every 0.4 seconds at 60 Hz. */
    if (++marquee_frames < 24) return;
    marquee_frames = 0;
    if (++marquee_offset >= length) marquee_offset = 0;
}

static void draw_browser_list(void) {
    if (count == 0) {
        browser_text(LIST_X,LIST_Y + 4,status);
        return;
    }
    for (int row = 0; row < VISIBLE && top + row < count; ++row) {
        int i = top + row;
        int y = LIST_Y + row * LIST_ROW_HEIGHT + 4;
        browser_text(LIST_X,y,i == selected ? ">" : " ");
        browser_text(LIST_X + 16,y,items[i].directory ? "[DIR]" : "[MP3]");
        browser_text(LIST_X + 72,y,items[i].name);
    }
}

static int is_mp3(const char *s) {
    size_t n=strlen(s);
    return n>=4 && !strcasecmp(s+n-4,".mp3");
}
static int compare_items(const void *aa,const void *bb) {
    const Item *a=(const Item*)aa,*b=(const Item*)bb;
    if(a->directory != b->directory) return b->directory-a->directory;
    return strcasecmp(a->name,b->name);
}
static void scan_dir(void) {
    DIR *dir; struct dirent *entry;
    count=selected=top=0;
    dir=opendir(cwd);
    if(!dir) { snprintf(status,sizeof(status),"Cannot open: %.90s",cwd); return; }
    while((entry=readdir(dir)) && count<MAX_ITEMS) {
        char full[768]; struct stat st;
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        snprintf(full,sizeof(full),"%s%s%s",cwd,
                 cwd[strlen(cwd)-1]=='/'?"":"/",entry->d_name);
        if(stat(full,&st)!=0) continue;
        if(!S_ISDIR(st.st_mode) && !is_mp3(entry->d_name)) continue;
        snprintf(items[count].name,NAME_SIZE,"%s",entry->d_name);
        items[count].directory=S_ISDIR(st.st_mode);
        count++;
    }
    closedir(dir);
    qsort(items,count,sizeof(Item),compare_items);
    snprintf(status,sizeof(status),"%d folders / MP3 entries",count);
}
static void parent_dir(void) {
    size_t root_len = strlen(mount_root);
    size_t n = strlen(cwd);
    /* B at the mounted volume root must never change the path. */
    if (n == root_len && strcmp(cwd, mount_root) == 0) return;
    /* Also recover safely if a path is shorter than or outside our root. */
    if (n <= root_len || strncmp(cwd, mount_root, root_len) != 0) {
        strcpy(cwd, mount_root);
        scan_dir();
        return;
    }
    if (cwd[n-1] == '/') cwd[--n] = '\0';
    char *p = strrchr(cwd, '/');
    if (p && (size_t)(p + 1 - cwd) >= root_len) *(p + 1) = '\0';
    else strcpy(cwd, mount_root);
    scan_dir();
}
static void enter_selected(void) {
    if(count==0) return;
    if(items[selected].directory) {
        char next[512];
        int n=snprintf(next,sizeof(next),"%s%s%s",cwd,
            cwd[strlen(cwd)-1]=='/'?"":"/",items[selected].name);
        if(n<0 || n>=(int)sizeof(next)) {
            strcpy(status,"Path is too long"); return;
        }
        strcpy(cwd,next); scan_dir();
    } else {
        snprintf(chosen_mp3,sizeof(chosen_mp3),"%s",items[selected].name);
        chosen_track = 0;
        for (int i = 0; i <= selected; ++i)
            if (!items[i].directory) ++chosen_track;
        marquee_frames = 0;
        marquee_offset = 0;
        char path[768];
        int n = snprintf(path, sizeof(path), "%s%s%s", cwd,
            cwd[strlen(cwd)-1]=='/' ? "" : "/", items[selected].name);
        if (n < 0 || n >= (int)sizeof(path) || !mp3_audio_open(path)) {
            snprintf(status, sizeof(status), "Audio: %.100s", mp3_audio_error());
            return;
        }
        showing_player=1;
    }
}
static void draw_ui(void) {
    GX_LoadTexObj(showing_player ? &player_texobj : &browser_texobj,GX_TEXMAP0);
    gx_quad(0,0,640,480,(GXColor){255,255,255,255});
    GX_DrawDone();
    if (showing_player) draw_player_info();
    else draw_browser_list();
    GX_DrawDone();
    GX_CopyDisp(xfb,GX_TRUE);
    GX_DrawDone();
    VIDEO_Flush();
}

int main(void) {
    VIDEO_Init(); PAD_Init();
    vmode=VIDEO_GetPreferredMode(NULL);
    xfb=MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
    if(!xfb) return 1;
    VIDEO_Configure(vmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush(); VIDEO_WaitVSync();
    if (!init_gx_browser()) return 1;
    /* Give the SD adapter time to settle after launch from Swiss. */
    usleep(400000);
    if (!fatInitDefault()) {
        strcpy(status, "FAT mount failed - check SD adapter");
    } else {
        /* SD2SP2, SD Gecko Slot B, and SD Gecko Slot A. */
        const char *paths[] = {"sd:/", "cardb:/", "carda:/"};
        int found = 0;
        for (unsigned int i = 0; i < sizeof(paths)/sizeof(paths[0]); ++i) {
            DIR *probe = opendir(paths[i]);
            if (probe) {
                closedir(probe);
                strcpy(cwd, paths[i]);
                strcpy(mount_root, paths[i]);
                found = 1;
                break;
            }
        }
        if (found) scan_dir();
        else strcpy(status, "No mounted SD volume found");
    }
    if (!mp3_audio_init())
        snprintf(status, sizeof(status), "Audio: %.100s", mp3_audio_error());
    draw_ui();
    unsigned last_second = 0;
    while(1) {
        VIDEO_WaitVSync();
        mp3_audio_pump();
        PAD_ScanPads();
        u32 pressed=PAD_ButtonsDown(0);
        if (showing_player && !mp3_audio_active()) {
            showing_player = 0;
            if (mp3_audio_error()[0])
                snprintf(status, sizeof(status), "Audio: %.100s", mp3_audio_error());
            draw_ui();
        }
        if (!pressed) {
            if (showing_player) {
                int before = marquee_offset;
                unsigned second = mp3_audio_elapsed_seconds();
                advance_player_marquee();
                if (marquee_offset != before || second != last_second) {
                    last_second = second;
                    draw_ui();
                }
            }
            continue;
        }
        if(pressed&PAD_BUTTON_START) break;
        if(showing_player) {
            if(pressed&PAD_BUTTON_B) { mp3_audio_stop(); showing_player=0; }
            else if (pressed&PAD_BUTTON_A) mp3_audio_pause_toggle();
        } else {
            if(pressed&PAD_BUTTON_B) parent_dir();
            if(pressed&PAD_BUTTON_A) enter_selected();
            if((pressed&PAD_BUTTON_DOWN) && selected+1<count) selected++;
            if((pressed&PAD_BUTTON_UP) && selected>0) selected--;
            if(selected<top) top=selected;
            if(selected>=top+VISIBLE) top=selected-VISIBLE+1;
        }
        draw_ui();
    }
    mp3_audio_shutdown();
    return 0;
}
