// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_BIOS_H
#define DOSBOX_BIOS_H

#include "dosbox.h"

#include <optional>

#define BIOS_BASE_ADDRESS_COM1          0x400
#define BIOS_BASE_ADDRESS_COM2          0x402
#define BIOS_BASE_ADDRESS_COM3          0x404
#define BIOS_BASE_ADDRESS_COM4          0x406
#define BIOS_ADDRESS_LPT1               0x408
#define BIOS_ADDRESS_LPT2               0x40a
#define BIOS_ADDRESS_LPT3               0x40c
/* 0x40e is reserved */
#define BIOS_CONFIGURATION              0x410
/* 0x412 is reserved */
#define BIOS_MEMORY_SIZE                0x413
#define BIOS_TRUE_MEMORY_SIZE           0x415
/* #define bios_expansion_memory_size      (*(unsigned int   *) 0x415) */
#define BIOS_KEYBOARD_STATE             0x417
#define BIOS_KEYBOARD_FLAGS1            BIOS_KEYBOARD_STATE
#define BIOS_KEYBOARD_FLAGS2            0x418
#define BIOS_KEYBOARD_TOKEN             0x419
/* used for keyboard input with Alt-Number */
#define BIOS_KEYBOARD_BUFFER_HEAD       0x41a
#define BIOS_KEYBOARD_BUFFER_TAIL       0x41c
#define BIOS_KEYBOARD_BUFFER            0x41e
/* #define bios_keyboard_buffer            (*(unsigned int   *) 0x41e) */
#define BIOS_DRIVE_ACTIVE               0x43e
#define BIOS_DRIVE_RUNNING              0x43f
#define BIOS_DISK_MOTOR_TIMEOUT         0x440
#define BIOS_DISK_STATUS                0x441
/* #define bios_fdc_result_buffer          (*(unsigned short *) 0x442) */
#define BIOS_VIDEO_MODE                 0x449
#define BIOS_SCREEN_COLUMNS             0x44a
#define BIOS_VIDEO_MEMORY_USED          0x44c
#define BIOS_VIDEO_MEMORY_ADDRESS       0x44e
#define BIOS_VIDEO_CURSOR_POS	        0x450


#define BIOS_CURSOR_SHAPE               0x460
#define BIOS_CURSOR_LAST_LINE           0x460
#define BIOS_CURSOR_FIRST_LINE          0x461
#define BIOS_CURRENT_SCREEN_PAGE        0x462
#define BIOS_VIDEO_PORT                 0x463
#define BIOS_VDU_CONTROL                0x465
#define BIOS_VDU_COLOR_REGISTER         0x466
/* 0x467-0x468 is reserved */
#define BIOS_LAST_UNEXPECTED_IRQ        0x46b
#define BIOS_TIMER                      0x46c
#define BIOS_24_HOURS_FLAG              0x470
#define BIOS_KEYBOARD_FLAGS             0x471
#define BIOS_CTRL_ALT_DEL_FLAG          0x472
#define BIOS_HARDDISK_COUNT		0x475
/* 0x474, 0x476, 0x477 is reserved */
#define BIOS_LPT1_TIMEOUT               0x478
#define BIOS_LPT2_TIMEOUT               0x479
#define BIOS_LPT3_TIMEOUT               0x47a
/* 0x47b is reserved */
#define BIOS_COM1_TIMEOUT               0x47c
#define BIOS_COM2_TIMEOUT               0x47d
#define BIOS_COM3_TIMEOUT               0x47e
#define BIOS_COM4_TIMEOUT               0x47f
/* 0x47e is reserved */ //<- why that?
/* 0x47f-0x4ff is unknown for me */
#define BIOS_KEYBOARD_BUFFER_START      0x480
#define BIOS_KEYBOARD_BUFFER_END        0x482

#define BIOS_ROWS_ON_SCREEN_MINUS_1     0x484
#define BIOS_FONT_HEIGHT                0x485

#define BIOS_VIDEO_INFO_0               0x487
#define BIOS_VIDEO_INFO_1               0x488
#define BIOS_VIDEO_INFO_2               0x489
#define BIOS_VIDEO_COMBO                0x48a

#define BIOS_KEYBOARD_FLAGS3            0x496
#define BIOS_KEYBOARD_LEDS              0x497

#define BIOS_WAIT_FLAG_POINTER          0x498
#define BIOS_WAIT_FLAG_COUNT	        0x49c		
#define BIOS_WAIT_FLAG_ACTIVE		0x4a0
#define BIOS_WAIT_FLAG_TEMP		0x4a1


#define BIOS_PRINT_SCREEN_FLAG          0x500

#define BIOS_VIDEO_SAVEPTR              0x4a8


#define BIOS_DEFAULT_HANDLER_LOCATION	(RealMake(0xf000, 0xff53))
#define BIOS_DEFAULT_INT5_LOCATION	(RealMake(0xf000, 0xff54))
#define BIOS_DEFAULT_IRQ0_LOCATION	(RealMake(0xf000, 0xfea5))
#define BIOS_DEFAULT_IRQ1_LOCATION	(RealMake(0xf000, 0xe987))
#define BIOS_DEFAULT_IRQ2_LOCATION	(RealMake(0xf000, 0xff55))
#define BIOS_DEFAULT_RESET_LOCATION	(RealMake(0xf000, is_machine_pcjr() ? 0x0043 : 0xe05b))

// The maximum "normal key" scancode value handled by keyboard bios routines.
// This should match the maximum return value set in KEYBOARD_AddKey()'s switch
// statement. The scan code is read from an 8-bit register (reg_al) and
// therefore limited to handling 255 keys.
constexpr uint8_t MAX_SCAN_CODE = 115;

/* The Section handling Bios Disk Access */
//#define BIOS_MAX_DISK 10

//#define MAX_SWAPPABLE_DISKS 20

void BIOS_ZeroExtendedSize(bool in);
void char_out(uint8_t chr,uint32_t att,uint8_t page);
void INT10_StartUp(void);
void INT16_StartUp(void);
void INT2A_StartUp(void);
void INT2F_StartUp(void);
void INT33_StartUp(void);
void INT13_StartUp(void);

bool BIOS_AddKeyToBuffer(uint16_t code);

void INT10_ReloadRomFonts();

void BIOS_SetComPorts (uint16_t baseaddr[]);

bool BIOS_ConfigureTandyDacCallbacks(const std::optional<bool> maybe_request_dac = {});

#endif
