/* Bootable graphics "hello world" kernel for x86 PCs (QEMU/KVM).
 * Loaded via Multiboot (qemu -kernel). Switches the Bochs/QEMU VBE display
 * adapter to 1024x768x32, draws into its linear framebuffer, and also logs
 * to COM1. */
#include <stdint.h>
#include "font.h"

/* ---- Multiboot v1 header ---- */
#define MB_MAGIC 0x1BADB002
#define MB_FLAGS 0x0
__attribute__((section(".multiboot"), used, aligned(4)))
static const uint32_t multiboot_header[3] = {
    MB_MAGIC, MB_FLAGS, (uint32_t)-(MB_MAGIC + MB_FLAGS)
};

/* ---- Entry point ---- */
uint8_t stack[16384] __attribute__((aligned(16), used));
void kmain(void);
__asm__(
    ".global _start\n"
    "_start:\n"
    "    cli\n"
    "    lea stack+16384, %esp\n"
    "    call kmain\n"
    "1:  hlt\n"
    "    jmp 1b\n");

/* ---- Port I/O ---- */
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

/* ---- Serial (COM1) ---- */
#define COM1 0x3F8
static void serial_init(void) {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7);
}
static void serial_puts(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') { while (!(inb(COM1 + 5) & 0x20)) {} outb(COM1, '\r'); }
        while (!(inb(COM1 + 5) & 0x20)) {}
        outb(COM1, (uint8_t)*s);
    }
}

/* ---- PCI: find the VBE adapter's framebuffer (BAR0) ---- */
static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static uint32_t find_framebuffer(void) {
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read(bus, dev, 0, 0);
            if (id == 0x11111234)               /* vendor 1234, device 1111: QEMU/Bochs std VGA */
                return pci_read(bus, dev, 0, 0x10) & 0xFFFFFFF0;
        }
    return 0;
}

/* ---- Bochs VBE (BGA) mode set ---- */
#define VBE_INDEX 0x01CE
#define VBE_DATA  0x01CF
static void vbe_write(uint16_t idx, uint16_t val) { outw(VBE_INDEX, idx); outw(VBE_DATA, val); }
static uint16_t vbe_read(uint16_t idx) { outw(VBE_INDEX, idx); return inw(VBE_DATA); }

#define W 1024
#define H 768
static volatile uint32_t *fb;

static int vbe_set_mode(void) {
    if (vbe_read(0) < 0xB0C0) return 0;     /* adapter not present */
    vbe_write(4, 0);                        /* disable */
    vbe_write(1, W);                        /* xres */
    vbe_write(2, H);                        /* yres */
    vbe_write(3, 32);                       /* bpp */
    vbe_write(4, 0x01 | 0x40);              /* enable + linear framebuffer */
    return 1;
}

/* ---- Drawing ---- */
static inline uint32_t rgb(int r, int g, int b) { return (uint32_t)(r << 16 | g << 8 | b); }
static inline void px(int x, int y, uint32_t c) {
    if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = c;
}
static uint32_t blend(uint32_t a, uint32_t b, int t) {   /* t in 0..255 */
    int r = (((a >> 16) & 255) * (255 - t) + ((b >> 16) & 255) * t) / 255;
    int g = (((a >> 8) & 255) * (255 - t) + ((b >> 8) & 255) * t) / 255;
    int bl = ((a & 255) * (255 - t) + (b & 255) * t) / 255;
    return rgb(r, g, bl);
}
static void gradient(uint32_t top, uint32_t bottom) {
    for (int y = 0; y < H; y++) {
        uint32_t c = blend(top, bottom, y * 255 / (H - 1));
        for (int x = 0; x < W; x++) fb[y * W + x] = c;
    }
}
static void rect(int x0, int y0, int w, int h, uint32_t c) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) px(x, y, c);
}
static void round_rect(int x0, int y0, int w, int h, int r, uint32_t c) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int dx = x < r ? r - x : (x >= w - r ? x - (w - r - 1) : 0);
            int dy = y < r ? r - y : (y >= h - r ? y - (h - r - 1) : 0);
            if (dx * dx + dy * dy <= r * r) px(x0 + x, y0 + y, c);
        }
}
static void circle(int cx, int cy, int r, uint32_t c) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) px(cx + x, cy + y, c);
}
static void draw_char(int x, int y, char ch, int s, uint32_t c) {
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = font8x16[ch - 32];
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (0x80 >> col)) rect(x + col * s, y + row * s, s, s, c);
}
static int text_width(const char *s, int scale) { int n = 0; while (s[n]) n++; return n * 8 * scale; }
static void draw_text(int x, int y, const char *s, int scale, uint32_t c) {
    for (; *s; s++, x += 8 * scale) draw_char(x, y, *s, scale, c);
}
static void draw_text_shadow(int x, int y, const char *s, int scale, uint32_t c) {
    draw_text(x + scale, y + scale, s, scale, rgb(0, 0, 0));
    draw_text(x, y, s, scale, c);
}

void kmain(void) {
    serial_init();
    uint32_t base = find_framebuffer();
    if (!base || !vbe_set_mode()) {
        serial_puts("No Bochs/QEMU VBE adapter found (use -vga std).\n");
        return;
    }
    fb = (volatile uint32_t *)base;

    /* background */
    gradient(rgb(20, 24, 60), rgb(120, 40, 110));

    /* decorative circles */
    circle(150, 140, 90, rgb(255, 170, 60));
    circle(880, 620, 130, rgb(60, 200, 220));
    circle(900, 150, 50, rgb(250, 90, 120));

    /* central panel */
    int pw = 720, ph = 300, pxo = (W - pw) / 2, pyo = (H - ph) / 2;
    round_rect(pxo + 8, pyo + 8, pw, ph, 24, rgb(10, 10, 25));    /* shadow */
    round_rect(pxo, pyo, pw, ph, 24, rgb(245, 245, 250));

    const char *title = "Hello, world!";
    draw_text(pxo + (pw - text_width(title, 5)) / 2, pyo + 50, title, 5, rgb(40, 40, 90));

    const char *sub = "A bare-metal C kernel in 1024x768x32";
    draw_text(pxo + (pw - text_width(sub, 2)) / 2, pyo + 160, sub, 2, rgb(90, 90, 110));

    /* color bars */
    uint32_t bars[] = { rgb(230, 60, 60), rgb(240, 160, 40), rgb(240, 220, 60),
                        rgb(70, 190, 90), rgb(60, 140, 230), rgb(140, 80, 210) };
    int bw = 90, gap = 16, total = 6 * bw + 5 * gap;
    for (int i = 0; i < 6; i++)
        round_rect(pxo + (pw - total) / 2 + i * (bw + gap), pyo + 220, bw, 40, 10, bars[i]);

    draw_text_shadow(20, H - 36, "No OS, no libc, just pixels.", 2, rgb(255, 255, 255));

    serial_puts("Hello, world! Graphics mode 1024x768x32 set.\n");
}
