/* Minimal, freestanding 68000 C program for Hatari's GDB stub.
 * GEMDOS Pterm0 is the only OS call; no MiNT C library is needed.
 */
/* Recognizable values when inspecting memory in the debugger. */
#define SAMPLE_SEED 0x12345678UL
#define SAMPLE_ITERATIONS 10
#define SAMPLE_BYTE_MASK 0xffUL

volatile unsigned long counter;
volatile unsigned long seed = SAMPLE_SEED;

/** Update the counter from the low byte of the sample seed. */
static void tick(void)
{
	counter += seed & SAMPLE_BYTE_MASK;
}

/** Atari program entry point; terminate through GEMDOS Pterm0. */
__attribute__((used, section(".text.start")))
void _start(void)
{
	unsigned i;

	for (i = 0; i < SAMPLE_ITERATIONS; ++i)
		tick();

	__asm__ volatile ("move.w #0,-(%%sp)\n\ttrap #1\n\taddq.l #2,%%sp"
			  : : : "memory", "cc"); /* GEMDOS Pterm0 */
	for (;;) { }
}
