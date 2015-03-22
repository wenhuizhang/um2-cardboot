/*-------------------------------------------------------------------------/
/  Stand-alone MMC boot loader
/--------------------------------------------------------------------------/
/
/  Copyright (C) 2010, ChaN, all right reserved.
/
/ * This software is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
/
/--------------------------------------------------------------------------/
/ March 18, 2013
/--------------------------------------------------------------------------/
/ Frank26080115 modified this bootloader for use as the Ultimaker2's bootloader
/ The bootloader must be activated by several conditions
/ * the card must be inserted
/ * the file must exist
/ * the button on the front of the UM2 must be held down at boot
/ Some LED blinking as been added to indicate the bootloader status
/ When the bootloader is ready, it will blink evenly and rapidly
/ The user should then release the button, and the actual flash operations will start
/ A flash write will only be performed if the new page does not match
/ The final LED blink pattern indicates if anything new was written (3 blinks if yes, 1 blink if no)
/ The UM2 must be manually reset to start the application
/
/ This variation of the code can be easily adapted to other applications
/ Resource cleanup is not performed!
/
/ If the constant AS_2NDARY_BOOTLOADER is defined, then this bootloader will
/ reside in the memory area just before the actual bootloader memory region
/ which means two bootloaders will exist. This type of bootloader mechanism
/ manipulates instructions in the reset vector to work.
/
/ There are two flavours of vectors
/ * large chips use 32 bit vector entries containing JMP instructions
/ * small chips use 16 bit vector entries containing RJMP instructions
/
/--------------------------------------------------------------------------/
/ Dec 6, 2010  R0.01  First release
/--------------------------------------------------------------------------/
/ This is a stand-alone MMC/SD boot loader for megaAVRs. It requires a 4KB
/ boot section for code. To port the boot loader into your project, follow
/ instructions described below.
/
/ 1. Setup the hardware. Attach a memory card socket to the any GPIO port
/    where you like. Select boot size at least 4KB for the boot loader with
/    BOOTSZ fuses and enable boot loader with BOOTRST fuse.
/
/ 2. Setup the software. Change the four port definitions in the asmfunc.S.
/    Change MCU_TARGET, BOOT_ADR and MCU_FREQ in the Makefile. The BOOT_ADR
/    is a BYTE address of boot section in the flash. Build the boot loader
/    and write it to the device with a programmer.
/
/ 3. Build the application program and output it in binary form instead of
/    hex format. Rename the file "app.bin" and put it into the memory card.
/
/-------------------------------------------------------------------------*/

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/boot.h>
#include <util/delay.h>
#include <string.h>
#include "pff.h"
#include "debug.h"

#define addr_t int32_t
#ifdef __AVR_MEGA__
#define VECTORS_USE_JMP
#define xjmp_t uint32_t
#define pgm_read_xjmp(x) pgm_read_dword_far(x)
#else
#define VECTORS_USE_RJMP
#define xjmp_t uint16_t
#define pgm_read_xjmp(x) pgm_read_word(x)
#endif

void dly_100us (void); // from asmfunc.S
static void start_app(void);
void flash_write(uint32_t adr, uint8_t* dat);

FATFS Fatfs;             // Petit-FatFs work area
BYTE Buff[SPM_PAGESIZE]; // Page data buffer

#ifdef AS_2NDARY_BOOTLOADER
xjmp_t app_reset_vector;
#endif

// Hardware configuration below

#define CARDDETECT_DDRx  DDRG
#define CARDDETECT_PORTx PORTG
#define CARDDETECT_PINx  PING
#define CARDDETECT_BIT   2
#define CARD_DETECTED()  bit_is_clear(CARDDETECT_PINx, CARDDETECT_BIT)

#define BUTTON_DDRx  DDRD
#define BUTTON_PORTx PORTD
#define BUTTON_PINx  PIND
#define BUTTON_BIT   2
#define BUTTON_PRESSED() bit_is_clear(BUTTON_PINx, BUTTON_BIT)

#define LED_DDRx  DDRH
#define LED_PORTx PORTH
#define LED_BIT   5
#define LED_ON()  PORTH |= _BV(LED_BIT)
#define LED_OFF() PORTH &= ~_BV(LED_BIT)
#define LED_TOG() PORTH ^= _BV(LED_BIT)

#ifdef AS_2NDARY_BOOTLOADER
#ifdef VECTORS_USE_JMP
static xjmp_t make_jmp(addr_t x)
{
	x >>= 1;
	addr_t y = x & 0x0001FFFF;
	x &= 0xFFFE0000;
	x <<= 3;
	y |= x;
	y |= 0x940C0000;
	y &= 0x95FDFFFF;
	return ((y & 0x0000FFFF) << 16 | (y & 0xFFFF0000) >> 16);
	// AVR 32 bit instruction ordering isn't straight forward little-endian
}
#endif
#ifdef VECTORS_USE_RJMP
static xjmp_t make_rjmp(addr_t src, addr_t dst)
{
	addr_t delta = dst - src;
	uint16_t delta16 = (uint16_t)delta;
	delta16 >>= 1;
	delta16 &= 0x0FFF;
	xjmp_t res = 0xCFFF & ((delta16) | 0xC000));
}
#endif

static void check_reset_vector(void)
{
	xjmp_t tmp1, tmp2;
	#ifdef VECTORS_USE_JMP
	tmp1 = make_jmp(BOOT_ADR);
	#elif defined(VECTORS_USE_RJMP)
	tmp1 = make_rjmp(0, BOOT_ADR);
	#endif
	tmp2 = pgm_read_xjmp(0);
	if (tmp2 != tmp1)
	{
		dbg_printf("reset vector requires overwrite, read 0x%08X, should be 0x%08X\r\n", tmp2, tmp1);
		// this means existing flash will not activate the bootloader
		// so we force a rewrite of this vector
		memset(Buff, 0xFF, SPM_PAGESIZE); // Clear buffer
		(*((xjmp_t*)Buff)) = tmp1;
		flash_write(0, Buff);
	}
}

#endif

static void LED_blink_pattern(uint16_t x)
{
	// each _delay_ms call is inlined so this function exists so we don't put _delay_ms everywhere
	// thus saving memory

	// the highest bit is a boundary bit, used to dictate the length of the pattern
	// bit 0 doesn't actually do anything
	// all the bits in between dictate the pattern itself
	// all 1s would mean "keep LED on"
	// all 0s would mean "keep LED off"
	// 101010 would mean "blink the LED twice fast"
	// 1001100110 would mean "blink the LED twice slowly"

	while (x)
	{
		x >>= 1;
		if ((x & 1) != 0) {
			LED_ON();
		}
		else {
			LED_OFF();
		}
		_delay_ms(100);
	}
}

static char can_jump(void)
{
	#ifdef AS_2NDARY_BOOTLOADER
	check_reset_vector();
	xjmp_t tmpx = pgm_read_xjmp(BOOT_ADR - sizeof(xjmp_t));
	#ifdef VECTORS_USE_JMP
	if ((tmpx & 0xFFFF) == 0xFFFF || (tmpx & 0xFFFF) == 0x0000 || tmpx == make_jmp(BOOT_ADR))
	#elif defined(VECTORS_USE_RJMP)
	if (tmpx == 0xFFFF || tmpx == 0x0000 || tmpx == make_rjmp(0, BOOT_ADR))
	#endif
	{
		// jump into user app is missing
		return 0;
	}
	#else
	uint16_t tmp16 = pgm_read_word_near(0);
	if (tmp16 == 0xFFFF || tmp16 == 0x0000) {
		return 0;
	}
	#endif

	return 1;
}

void flash_write(uint32_t adr, uint8_t* dat)
{
	// I replaced the old flash_write from asmfunc.S with this version
	// because the old version didn't seem to work
	// I was hoping that this avr-libc implementation would be safer and more portable
	// but it still doesn't work

	boot_spm_busy_wait();
	boot_rww_enable();
	boot_page_erase(adr);
	boot_spm_busy_wait();
	uint32_t i;
	uint16_t j;
	for (i = adr, j = 0; j < SPM_PAGESIZE; i += 2, j += 2) {
		boot_page_fill(i, *((uint16_t*)(&dat[j])));
	}
	boot_page_write(adr);
	boot_spm_busy_wait();
}

int main (void)
{
	#ifdef ENABLE_DEBUG
	dbg_init();
	_delay_ms(100);
	#endif
	dbg_printf("\r\nUM2 SD Card Bootloader\r\n");
	#ifdef ENABLE_DEBUG
	dbg_printf("LFUSE 0x%02X, HFUSE 0x%02X\r\n", boot_lock_fuse_bits_get(GET_LOW_FUSE_BITS), boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS));
	dbg_printf("EFUSE 0x%02X, LOCKBITS 0x%02X\r\n", boot_lock_fuse_bits_get(GET_EXTENDED_FUSE_BITS), boot_lock_fuse_bits_get(GET_LOCK_BITS));
	#endif

	DWORD fa; // Flash address
	WORD br;  // Bytes read
	DWORD bw; // Bytes written
	WORD i;   // Index for page difference check
	char canjump;

	CARDDETECT_DDRx &= ~_BV(CARDDETECT_BIT); // pin as input
	CARDDETECT_PORTx |= _BV(CARDDETECT_BIT); // enable internal pull-up resistor
	BUTTON_DDRx &= ~_BV(BUTTON_BIT); // pin as input
	BUTTON_PORTx |= _BV(BUTTON_BIT); // enable internal pull-up resistor

	#ifdef AS_2NDARY_BOOTLOADER
	char end_of_file = 0;

	check_reset_vector();
	#endif

	canjump = can_jump();

	// prepare LED
	LED_DDRx |= _BV(LED_BIT); // pin as output
	LED_OFF();

	if (canjump)
	{
		#ifndef ENABLE_DEBUG
		dly_100us(); // only done to wait for signals to rise
		#endif

		if (!CARD_DETECTED()) {
			dbg_printf("card not detected\r\n");
			start_app();
		}

		if (!BUTTON_PRESSED()) {
			dbg_printf("button not pressed\r\n");
			start_app();
		}

		dbg_printf("can jump, almost primed\r\n");
	}
	else
	{
		dbg_printf("forced to boot from card\r\n");
	}

	pf_mount(&Fatfs); // Initialize file system
	if (pf_open("app.bin") != FR_OK) // Open application file
	{
		dbg_printf("file failed to open\r\n");
		start_app();
	}

	LED_ON();

	// wait for button release
	#ifdef ENABLE_DEBUG
	if (canjump) {
		dbg_printf("waiting for button release...");
	}
	#endif
	while (BUTTON_PRESSED() && canjump) {
		// blink the LED while waiting
		LED_blink_pattern(0x10C);
	}
	#ifdef ENABLE_DEBUG
	if (canjump) {
		dbg_printf(" RELEASED!!\r\n");
	}
	#endif

	for (fa = 0, bw = 0; fa < BOOT_ADR; fa += SPM_PAGESIZE) // Update all application pages
	{
		memset(Buff, 0xFF, SPM_PAGESIZE); // Clear buffer
		#ifdef AS_2NDARY_BOOTLOADER
		if (!end_of_file)
		#endif
		pf_read(Buff, SPM_PAGESIZE, &br); // Load a page data

		char to_write = 0;

		if (br > 0 // If data is available
		#ifdef AS_2NDARY_BOOTLOADER
		|| fa == (BOOT_ADR - SPM_PAGESIZE) // If is last page
		#endif
		)
		{
			#ifdef AS_2NDARY_BOOTLOADER
			if (fa < SPM_PAGESIZE) // If is very first page
			{
				// the old reset vector will point inside the application
				// but we need it to point into the bootloader, or else the bootloader will never launch
				// we need to save the old reset vector, then replace it
				app_reset_vector = (*((xjmp_t*)Buff));
				(*((xjmp_t*)Buff)) = 
				#ifdef VECTORS_USE_JMP
					make_jmp(BOOT_ADR);
				#elif defined(VECTORS_USE_RJMP)
					make_rjmp(0, BOOT_ADR);
				#endif
				dbg_printf("reset vector, old = 0x%08X , new = 0x%08X\r\n", app_reset_vector, (*((xjmp_t*)Buff)));
				if (br <= 0) br += sizeof(xjmp_t);
			}
			else if (fa == (BOOT_ADR - SPM_PAGESIZE)) // If is trampoline
			{
				// we need a way to launch the application
				// that's why we saved the old reset vector
				// we check where it points to, and write it as a trampoline
				// use the trampoline to launch the real app from the SD card bootloader

				xjmp_t* inst_ptr = ((xjmp_t*)(&Buff[SPM_PAGESIZE-sizeof(xjmp_t)]));
				#ifdef VECTORS_USE_JMP
				if ((app_reset_vector & 0xFE0E0000) == 0x940C0000) {
					// this is a JMP instruction, we can put it here without changing it
					(*inst_ptr) = app_reset_vector;
					dbg_printf("trampoline use JMP, addr 0x%08X, insn 0x%08X\r\n", fa, app_reset_vector);
					if (br <= 0) br += sizeof(xjmp_t); // indicate that we wrote something useful
				}
				else if ((app_reset_vector & 0x0000F000) == 0x0000C000) {
					// this is a RJMP instruction
					(*inst_ptr) = make_jmp((app_reset_vector & 0x0FFF) << 1);
					dbg_printf("trampoline RJMP converted to JMP, addr 0x%08X, RJMP 0x%04X, JMP 0x%08X\r\n", fa, app_reset_vector, (*inst_ptr));
					if (br <= 0) br += sizeof(xjmp_t); // indicate that we wrote something useful
				}
				else if ((app_reset_vector & 0xFFFF) == 0xFFFF || (app_reset_vector & 0xFFFF) == 0x0000) {
					(*inst_ptr) = make_jmp(BOOT_ADR); // if app doesn't exist, make it loop back into the bootloader
					dbg_printf("trampoline, no app, addr 0x%08X, JMP to boot 0x%08X\r\n", fa, (*inst_ptr));
				}
				#elif defined(VECTORS_USE_RJMP)
				if ((app_reset_vector & 0xF000) == 0xC000) {
					// this is a RJMP instruction
					addt_t dst = (app_reset_vector & 0x0FFF) << 1;
					(*inst_ptr) = make_rjmp(BOOT_ADR - sizeof(xjmp_t), dst);
					dbg_printf("trampoline, addr 0x%08X, RJMP 0x%04X\r\n", fa, (*inst_ptr));
				}
				else if (app_reset_vector == 0xFFFF || app_reset_vector == 0x0000) {
					(*inst_ptr) = make_rjmp(BOOT_ADR - sizeof(xjmp_t), BOOT_ADR); // if app doesn't exist, make it loop back into the bootloader
					dbg_printf("trampoline, no app, addr 0x%08X, RJMP 0x%04X\r\n", fa, (*inst_ptr));
				}
				#endif
				else {
					// hmm... it wasn't a JMP or RJMP but it wasn't blank, we put it here and hope for the best
					(*inst_ptr) = app_reset_vector;
					dbg_printf("trampoline, unknown, addr 0x%08X, RJMP 0x%04X\r\n", fa, (*inst_ptr));
					if (br <= 0) br += sizeof(xjmp_t); // indicate that we wrote something useful
				}
			}
			#endif

			for (i = 0; i < SPM_PAGESIZE && to_write == 0; i++)
			{ // check if the page has differences
				if (
					#if (FLASHEND > USHRT_MAX)
						pgm_read_byte_far(i)
					#else
						pgm_read_byte_far(i)
					#endif
						!= Buff[i])
				{
					to_write = 1;
				}
			}
		}
		#ifdef AS_2NDARY_BOOTLOADER
		else if (br <= 0)
		{
			end_of_file = 1;
		}
		#endif

		if (to_write) // write only if required
		{
			LED_TOG(); // blink the LED while writing
			flash_write(fa, Buff);
			bw += br;
			dbg_printf("bytes written: %d\r\n", bw);
		}
	}

	if (bw > 0)
	{
		dbg_printf("all done\r\n");
		// triple blink the LED to indicate that new firmware written
		while (1) {
			LED_blink_pattern(0x402A);
		}
	}
	else
	{
		dbg_printf("all done, nothing written\r\n");
		// single blink the LED to indicate that nothing was actually written
		while (1) {
			LED_blink_pattern(0x4002);
		}
	}
}

static void start_app(void)
{
	char canjump = can_jump();

	#ifdef ENABLE_DEBUG
	if (!canjump) {
		dbg_printf("no app to start\r\n");
	}
	else {
		dbg_printf("starting app\r\n");
	}
	#endif

	// long blink to indicate blank app
	while (!canjump)
	{
		LED_blink_pattern(0x87FF);
	}

	dbg_deinit();

	#ifdef AS_2NDARY_BOOTLOADER
		// there is an instruction stored here, jump here and execute it
		#ifdef VECTORS_USE_JMP
			asm volatile("rjmp (__vectors - 4)");
		#elif defined(VECTORS_USE_RJMP)
			asm volatile("rjmp (__vectors - 2)");
		#endif
	#else
		// this "xjmp to 0" approach is better than the "function pointer to 0" approach when dealing with a larger chip
		#ifdef __AVR_HAVE_JMP_CALL__
			asm volatile("jmp 0000");
		#else
			asm volatile("rjmp 0000");
		#endif
	#endif
}