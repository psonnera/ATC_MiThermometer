/*
 * m5stack.h
 *
 * Emulation of the "M5Stack" BLE peripheral used by xDrip4iOS
 * (https://github.com/JohanDegraeve/M5_NightscoutMon) so that the
 * app can push glucose readings to the thermometer display.
 */
#ifndef _M5STACK_H_
#define _M5STACK_H_

#include "app_config.h"

#if USE_M5STACK_EMU

#define M5_PKT_MAX		20	// xDrip4iOS packet size (ATT_MTU 23)
#define M5_PWD_MAX		16	// max BLE password length stored in flash
#define M5_PWD_GEN_LEN	10	// length of a generated password

extern u16 m5ValueInCCC;

int m5Write(void *p);			// GATT write callback (M5_DP_H)
void m5_task(void);				// call once per second (main_loop)
void m5_on_disconnect(void);	// call from ble_disconnect_callback()
int m5_lcd_show(void);			// draws the glucose screen, returns 1 if drawn

#endif // USE_M5STACK_EMU
#endif // _M5STACK_H_
