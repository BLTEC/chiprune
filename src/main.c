#include <gfx.h>
#include <mem/mc.h>
#include <mem/sdram.h>
#include <mem/emc.h>
#include <soc/hw_init.h>
#include <storage/emmc.h>
#include <storage/sd.h>
#include <mem/heap.h>
#include <memory_map.h>
#include <storage/sdmmc.h>
#include <storage/mbr_gpt.h>
#include <string.h>
#include <soc/timer.h>
#include <usb/usbd.h>
#include <utils/btn.h>
#include <utils/types.h>
#include <utils/util.h>
#include <utils/sprintf.h>
#include <soc/t210.h>

#include <display/di.h>
#include <gfx_utils.h>
#include <tui.h>

// Skip the first 16MB of DRAM (convention used by Nvidia/CTCaer bootloaders
// to reserve space near DRAM_START for secure/TZ firmware).
#define DRAM_TEST_SKIP   SZ_16M
// How much DRAM to pattern-test. Reduce this if the console has less RAM
// or if you want a quicker (less thorough) pass.
#define DRAM_TEST_SIZE   SZ_1G

// Small-palette color indices (see bdk/display/di.inl _di_winA_pitch_small_palette).
#define COL_BLACK   0
#define COL_WHITE   1
#define COL_GREY    2
#define COL_RED     3
#define COL_GREEN   4
#define COL_YELLOW  5
#define COL_BLUE    6

#define BIT_LANES     32
#define TEST_CHUNKS   16

static void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color){
	for(u32 j = 0; j < h; j++)
		for(u32 i = 0; i < w; i++)
			gfx_set_pixel(x + i, y + j, color);
}

// 3-dot "loading" indicator: one dot lights up at a time, cycling.
static void draw_progress_dots(u32 x, u32 y, u32 step){
	for(u32 d = 0; d < 3; d++){
		u32 col = (d == (step % 3)) ? COL_YELLOW : COL_GREY;
		gfx_fill_circle(x + d * 12, y, 3, col);
	}
}

// Chip-footprint style diagram: a package outline with one dot per DQ
// Real physical BGA-200 footprint of the RAM package (extracted from the
// board's actual net/ball mapping - both modules, N14856 and N14857, are
// the identical part: Hynix H9HCNNNCPMMLXR-NEI, LPDDR4X, so they share
// the same ball layout). Grid is a full 10 columns x 20 rows
// (columns 1,2,3,4,5,8,9,10,11,12 and rows A,B,C,D,E,F,G,H,J,K,N,P,R,T,U,V,W,Y,AA,AB).
// col[b]/row[b] give the real grid position of DQ<b>'s ball for bit b of
// the 32-bit word (DQ0-15 = channel A, DQ16-31 = channel B).
static const u8 dq_col[BIT_LANES] = {
	1,1,1,1,3,3,3,3,8,8,8,8,6,6,6,6,
	1,1,1,1,3,3,3,3,8,8,8,8,6,6,6,6
};
static const u8 dq_row[BIT_LANES] = {
	1,2,4,5,5,4,2,1,1,2,4,5,5,4,2,1,
	18,17,15,14,14,15,17,18,18,17,15,14,14,15,17,18
};
#define BGA_COLS 10
#define BGA_ROWS 20
#define MOD_PITCH_X 7
#define MOD_PITCH_Y 5
#define MOD_W (BGA_COLS * MOD_PITCH_X)
#define MOD_H ((BGA_ROWS - 1) * MOD_PITCH_Y)
#define MOD_GAP 6

// Reverse lookup: grid index -> real silkscreen ball name (e.g. "F9", "U9").
static const u8 bga_col_num[BGA_COLS] = {1,2,3,4,5,8,9,10,11,12};
static const char *bga_row_name[BGA_ROWS] = {
	"A","B","C","D","E","F","G","H","J","K",
	"N","P","R","T","U","V","W","Y","AA","AB"
};

// Draws ONE memory module's real BGA-200 footprint, to scale: every pin
// (DQ or not) is the same tiny single-pixel dot, distinguished only by
// color (grey = non-data pin, green = DQ pin, ok). The ONLY pin that gets
// drawn bigger is one with a detected error, in a distinct highlight
// color, so it's immediately findable regardless of category/channel.
static void draw_module(u32 x0, u32 y0, const char *title, u32 *bit_errors){
	gfx_puts_small(x0, y0, title, COL_WHITE);
	y0 += 8;

	// Package outline.
	for(u32 i = 0; i <= MOD_W; i++){
		gfx_set_pixel(x0 + i, y0, COL_GREY);
		gfx_set_pixel(x0 + i, y0 + MOD_H, COL_GREY);
	}
	for(u32 j = 0; j <= MOD_H; j++){
		gfx_set_pixel(x0, y0 + j, COL_GREY);
		gfx_set_pixel(x0 + MOD_W, y0 + j, COL_GREY);
	}

	// All 200 balls, same tiny size, colored by category only.
	for(u32 r = 0; r < BGA_ROWS; r++){
		for(u32 c = 0; c < BGA_COLS; c++){
			u32 cx = x0 + c * MOD_PITCH_X + MOD_PITCH_X / 2;
			u32 cy = y0 + r * MOD_PITCH_Y;
			gfx_set_pixel(cx, cy, COL_GREY);
		}
	}
	for(u32 b = 0; b < BIT_LANES; b++){
		u32 cx = x0 + dq_col[b] * MOD_PITCH_X + MOD_PITCH_X / 2;
		u32 cy = y0 + dq_row[b] * MOD_PITCH_Y;
		gfx_set_pixel(cx, cy, COL_GREEN);
	}

	// Error pin(s): the only ones enlarged, in a highlight color.
	for(u32 b = 0; b < BIT_LANES; b++){
		if(!bit_errors[b])
			continue;
		u32 cx = x0 + dq_col[b] * MOD_PITCH_X + MOD_PITCH_X / 2;
		u32 cy = y0 + dq_row[b] * MOD_PITCH_Y;
		gfx_fill_circle(cx, cy, 3, COL_YELLOW);
	}
}

// Which physical module corresponds to the low 32 bits of each 64-bit
// word vs the high 32 bits is NOT confirmed by software alone (needs
// physical validation - see chat). Flip this if/when confirmed.
typedef enum { ORDER_N14856_LOW_N14857_HIGH, ORDER_N14857_LOW_N14856_HIGH } module_order_t;
static const module_order_t g_module_order = ORDER_N14856_LOW_N14857_HIGH;
static const bool g_module_order_confirmed = false;

// Prints the text result: either a clear "no errors" message, or one line
// per module per defective DQ line, naming the exact signal and ball.
// bit_errors_lo/hi are the low/high 32-bit halves of the 64-bit DRAM
// word (word-pair interleave), NOT an address-range split.
static u32 draw_module_report(u32 y0, u32 *bit_errors_lo, u32 *bit_errors_hi){
	u32 *n14856 = (g_module_order == ORDER_N14856_LOW_N14857_HIGH) ? bit_errors_lo : bit_errors_hi;
	u32 *n14857 = (g_module_order == ORDER_N14856_LOW_N14857_HIGH) ? bit_errors_hi : bit_errors_lo;
	static const char *mod_name[2] = {"N14856", "N14857"};
	u32 *arrs[2] = {n14856, n14857};
	bool any = false;

	for(u32 m = 0; m < 2; m++){
		for(u32 b = 0; b < BIT_LANES; b++){
			if(!arrs[m][b])
				continue;
			any = true;
			char sig[8], ball[8], line[40];
			s_printf(sig, "DQ%d_%c", b % 16, (b < 16) ? 'A' : 'B');
			s_printf(ball, "%s%d", bga_row_name[dq_row[b]], bga_col_num[dq_col[b]]);
			s_printf(line, "Mod %d (%s): pino %s (%s)", m + 1, mod_name[m], sig, ball);
			gfx_puts_small(2, y0, line, COL_YELLOW);
			y0 += 8;
		}
	}

	if(!any){
		gfx_puts_small(2, y0, "Nenhum erro encontrado nos", COL_GREEN);
		y0 += 8;
		gfx_puts_small(2, y0, "modulos de memoria.", COL_GREEN);
		y0 += 8;
	}
	if(!g_module_order_confirmed){
		gfx_puts_small(2, y0, "(ordem N14856/57 nao confirmada,", COL_GREY);
		y0 += 8;
		gfx_puts_small(2, y0, "so o par baixo/alto e real)", COL_GREY);
		y0 += 8;
	}
	return y0 + 2;
}

// Test patterns: some XORed with the word index (catches address-decode
// faults too), some fixed (classic stuck-at-0/1 coverage). Kept modest in
// count to keep test time reasonable; walking-ones/zeros would be a good
// further addition if this needs to be even more thorough.
typedef struct { u32 value; bool xor_index; } test_pattern_t;
static const test_pattern_t g_patterns[] = {
	{0xAA55AA55, true},
	{0x55AA55AA, true},
	{0x00000000, false},
	{0xFFFFFFFF, false},
};
#define NUM_PATTERNS (sizeof(g_patterns) / sizeof(g_patterns[0]))

static void draw_stage_box(u32 y0, const char *title, u32 color){
	const u32 w = 176, h = 10;
	for(u32 i = 0; i <= w; i++){
		gfx_set_pixel(2 + i, y0, color);
		gfx_set_pixel(2 + i, y0 + h, color);
	}
	for(u32 j = 0; j <= h; j++){
		gfx_set_pixel(2, y0 + j, color);
		gfx_set_pixel(2 + w, y0 + j, color);
	}
	gfx_puts_small(6, y0 + 2, title, color);
}

static void dram_test(){
	u32 y = 2;

	// ETAPA 1: se voce esta vendo esta caixa na tela, o SoC (CPU/BPMP),
	// o boot via IRAM e o driver de video ja provaram que funcionam -
	// nenhum deles depende da DRAM. Se o console nunca chega a mostrar
	// isso (tela preta desde o inicio), a suspeita e SoC/alimentacao/
	// video, NAO a DRAM.
	draw_stage_box(y, "ETAPA 1/2: CPU/SoC OK", COL_GREEN);
	y += 16;
	gfx_puts_small(2, y, "(voce esta vendo isto = SoC ok,", COL_GREY); y += 8;
	gfx_puts_small(2, y, "nao depende da DRAM)", COL_GREY); y += 14;

	// ETAPA 2: a partir daqui, tudo depende da DRAM. Se travar exatamente
	// aqui (sem avancar, sem chegar no resultado), a suspeita passa a
	// ser DRAM/EMC - nao o SoC, que a Etapa 1 ja confirmou.
	draw_stage_box(y, "ETAPA 2/2: Testando DRAM", COL_YELLOW);
	y += 16;
	gfx_puts_small(2, y, "(trava a partir daqui = falha", COL_WHITE); y += 8;
	gfx_puts_small(2, y, "na DRAM/EMC, nao no SoC)", COL_WHITE); y += 14;

	sdram_init();

	gfx_puts_small(2, y, "OK, nao travou.", COL_GREEN); y += 8;
	{
		// Tegra X1 = interface de 64 bits, formada por 2 modulos LPDDR4
		// de 32 bits cada (nao 2 ranks de expansao de capacidade).
		u32 adr_cfg = EMC(EMC_ADR_CFG);
		char buf[36];
		s_printf(buf, "EMC_ADR_CFG=0x%08X", adr_cfg);
		gfx_puts_small(2, y, buf, COL_WHITE); y += 8;
	}
	{
		char buf[32];
		s_printf(buf, "Testando %d MB x %d padroes", DRAM_TEST_SIZE / SZ_1M, (int)NUM_PATTERNS);
		gfx_puts_small(2, y, buf, COL_WHITE); y += 10;
	}

	u32 anim_x = 2, anim_y = y + 4;
	y += 12;
	u32 status_y = y; // single line, cleared+redrawn each pattern (no stacking, no overlap)
	y += 10;

	volatile u32 *mem = (volatile u32 *)(DRAM_START + DRAM_TEST_SKIP);
	u32 words = DRAM_TEST_SIZE / sizeof(u32);
	u32 errors = 0;
	u32 err_addr = 0;

	// bit_errors_lo = word i even (low dword of the 64-bit pair at i,i+1)
	// bit_errors_hi = word i odd  (high dword of that same pair)
	// This is the real 32-bit half of the 64-bit bus each 32-bit CPU-side
	// access lands on - NOT an address-range split. Each half stays
	// permanently tied to one physical module's 32 DQ lines.
	u32 bit_errors_lo[BIT_LANES] = {0};
	u32 bit_errors_hi[BIT_LANES] = {0};
	u32 chunk_sz = words / TEST_CHUNKS;
	u32 anim = 0;

	for(u32 p = 0; p < NUM_PATTERNS; p++){
		u32 base_pattern = g_patterns[p].value;
		bool xor_idx = g_patterns[p].xor_index;

		for(u32 c = 0; c < TEST_CHUNKS; c++){
			u32 start = c * chunk_sz;
			u32 end = (c == TEST_CHUNKS - 1) ? words : start + chunk_sz;
			for(u32 i = start; i < end; i++)
				mem[i] = xor_idx ? (base_pattern ^ i) : base_pattern;
			draw_progress_dots(anim_x, anim_y, anim++);
		}

		for(u32 c = 0; c < TEST_CHUNKS; c++){
			u32 start = c * chunk_sz;
			u32 end = (c == TEST_CHUNKS - 1) ? words : start + chunk_sz;
			for(u32 i = start; i < end; i++){
				u32 expected = xor_idx ? (base_pattern ^ i) : base_pattern;
				u32 got = mem[i];
				if(got != expected){
					if(!errors)
						err_addr = DRAM_START + DRAM_TEST_SKIP + i * sizeof(u32);
					errors++;

					u32 diff = got ^ expected;
					u32 *bit_errors = (i & 1) ? bit_errors_hi : bit_errors_lo;
					for(u32 b = 0; b < BIT_LANES; b++)
						if(diff & (1u << b))
							bit_errors[b]++;
				}
			}
			draw_progress_dots(anim_x, anim_y, anim++);
		}

		char buf[36];
		s_printf(buf, "Padrao %d/%d ok. Erros: %d", p + 1, (int)NUM_PATTERNS, errors);
		fill_rect(2, status_y, 176, 8, COL_BLACK); // clear the previous line first
		gfx_puts_small(2, status_y, buf, COL_WHITE);
	}
	y += 4;

	if(!errors){
		gfx_puts_small(2, y, "PASS - sem erros", COL_GREEN);
	}
	else{
		char buf[40];
		s_printf(buf, "FAIL - %d erros, 1o em 0x%08X", errors, err_addr);
		gfx_puts_small(2, y, buf, COL_RED);
	}
	y += 12;

	// Both physical memory modules, side by side, no overlap. Each gets
	// its OWN 32-bit half of the 64-bit bus (word parity), not a
	// duplicated or address-halved result.
	gfx_puts_small(2, y, "Modulos de memoria (2x32=64bit):", COL_WHITE);
	y += 10;
	u32 mod1_x = 2, mod2_x = mod1_x + MOD_W + MOD_GAP;
	u32 *mod1_errors = (g_module_order == ORDER_N14856_LOW_N14857_HIGH) ? bit_errors_lo : bit_errors_hi;
	u32 *mod2_errors = (g_module_order == ORDER_N14856_LOW_N14857_HIGH) ? bit_errors_hi : bit_errors_lo;
	draw_module(mod1_x, y, "N14856", mod1_errors);
	draw_module(mod2_x, y, "N14857", mod2_errors);
	y += 8 + MOD_H + 6;

	y = draw_module_report(y, bit_errors_lo, bit_errors_hi);

	gfx_puts_small(2, y + 4, "Aperte um botao p/ desligar.", COL_WHITE);
	btn_wait();
}

int main(){
	dram_test();
	power_set_state(POWER_OFF);
	return 0;
}

void ipl_main(){
	hw_init();
	pivot_stack(IPL_STACK_TOP);
	heap_init((void*)IPL_HEAP_START);

	mc_enable_ahb_redirect(true);

	display_init();
	u8 *fb = (u8*)display_init_window_a_pitch_small_palette();

	gfx_init_ctxt(fb, 180, 320, 192);
	gfx_con_init();
	display_backlight_pwm_init();

	display_backlight_brightness(128, 1000);
	// checkerboard_test();
	main();
}