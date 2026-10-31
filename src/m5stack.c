/*
 * m5stack.c
 *
 * Emulation of the M5_NightscoutMon BLE protocol so that xDrip4iOS
 * (xdripswift, M5StackBluetoothTransmitter) can send glucose readings
 * to the thermometer as if it were an "M5Stack" display.
 *
 * Protocol summary (JohanDegraeve/M5_NightscoutMon, xdripswift):
 *   service        AF6E5F78-706A-43FB-B1F4-C27D7D5C762F
 *   characteristic 6D810E9F-0983-4030-BDA7-C7C9A6A19C1C, write w/o rsp + notify
 *   text packets:  [opcode][packet no, 1-based][total packets][<= 17 bytes of text]
 *   app -> device: 0x09 read password, 0x0A [password] authenticate,
 *                  0x03 "true"/"false" mg/dL, 0x14 "<local-UTC sec>",
 *                  0x12 "<local epoch sec>", 0x13 "<slope name>",
 *                  0x10 "<mg/dL> <UTC epoch sec>", 0x21 read battery,
 *                  0x15/0x17/0x18/0x19/0x04..0x06/0x22 display settings (ignored)
 *   device -> app: 0x0E password, 0x0B/0x0C auth ok/fail, 0x16 request all
 *                  parameters, 0x11 request time, 0x20 battery, 0x21 heartbeat
 *
 * Display mapping (same convention as WatchDrip+ on the pvvx firmware):
 *   big number   = glucose (mg/dL integer, or mmol/L with one decimal)
 *   small number = change since the previous reading, unsigned
 *   smiley       = (^_^) in range, (-^-) high or low, (ooo) reading older than 15 min
 *   trend        = the three bars next to the °C/°F sign:
 *                  top = rising, top+middle = slowly rising, middle = flat,
 *                  middle+bottom = slowly falling, bottom = falling
 *   after 60 min without a new reading the display returns to T/H.
 */
#include "tl_common.h"
#include "app_config.h"
#if USE_M5STACK_EMU
#include "drivers.h"
#include "stack/ble/ble.h"
#include "ble.h"
#include "app.h"
#include "lcd.h"
#include "flash_eep.h"
#if (DEV_SERVICES & SERVICE_HARD_CLOCK)
#include "rtc.h"
#endif
#include "m5stack.h"

#if !((DEVICE_TYPE == DEVICE_LYWSD03MMC) || (DEVICE_TYPE == DEVICE_MJWSD06MMC))
#error "USE_M5STACK_EMU: the display mapping is only implemented for LYWSD03MMC and MJWSD06MMC"
#endif
#if !(DEV_SERVICES & SERVICE_SCREEN)
#error "USE_M5STACK_EMU requires SERVICE_SCREEN"
#endif

#if (DEV_SERVICES & SERVICE_TIME_ADJUST)
extern u32 utc_set_time_sec; // cmd_parser.c
#endif

/* opcodes written by the app (M5StackTransmitterOpCodeTx) */
enum {
	M5_TX_MGDL         = 0x03, // text "true" / "false"
	M5_TX_BRIGHTNESS1  = 0x04,
	M5_TX_BRIGHTNESS2  = 0x05,
	M5_TX_BRIGHTNESS3  = 0x06,
	M5_TX_READ_PWD     = 0x09,
	M5_TX_AUTH         = 0x0A, // [0x0A][password bytes], no packet header
	M5_TX_BG           = 0x10, // text "<mg/dL> <UTC epoch seconds>"
	M5_TX_TIMESTAMP    = 0x12, // text "<local epoch seconds>"
	M5_TX_SLOPE        = 0x13, // text "Flat", "FortyFiveUp", ...
	M5_TX_TIME_OFFSET  = 0x14, // text "<local - UTC, seconds>"
	M5_TX_TEXT_COLOR   = 0x15, // [op][u16 color]
	M5_TX_BG_COLOR     = 0x17, // [op][u16 color]
	M5_TX_ROTATION     = 0x18, // [op][u16]
	M5_TX_BRIGHTNESS   = 0x19, // [op][int]
	M5_TX_READ_BATTERY = 0x21,
	M5_TX_POWER_OFF    = 0x22
};

/* opcodes notified to the app (M5StackTransmitterOpCodeRx) */
enum {
	M5_RX_AUTH_OK      = 0x0B,
	M5_RX_AUTH_FAIL    = 0x0C,
	M5_RX_PWD_ERR1     = 0x0D, // password must be set in the app settings
	M5_RX_PWD          = 0x0E, // password text
	M5_RX_PWD_ERR2     = 0x0F, // password already known, reset device
	M5_RX_REQ_TIME     = 0x11,
	M5_RX_REQ_PARAMS   = 0x16,
	M5_RX_BATTERY      = 0x20, // [0x20][level %]
	M5_RX_HEARTBEAT    = 0x21
};

#ifndef M5_BG_LOW_MGDL
#define M5_BG_LOW_MGDL		70		// below: sad smiley
#endif
#ifndef M5_BG_HIGH_MGDL
#define M5_BG_HIGH_MGDL		180		// above: sad smiley
#endif
#ifndef M5_STALE_SEC
#define M5_STALE_SEC		(15*60)	// reading age: "(ooo)", trend hidden
#endif
#ifndef M5_EXPIRE_SEC
#define M5_EXPIRE_SEC		(60*60)	// reading age: back to temperature/humidity
#endif
#define M5_HEARTBEAT_AFTER_SEC	300	// like M5_NightscoutMon: heartbeat when the reading is old
#define M5_HEARTBEAT_PERIOD_SEC	60
#define M5_ASM_BUF_SIZE		48		// longest text: "<mg/dL> <epoch>" / "NOT COMPUTABLE"

enum {
	M5_STAGE_NONE = 0,	// nothing to show, normal T/H screen
	M5_STAGE_FRESH,		// reading younger than M5_STALE_SEC
	M5_STAGE_STALE		// reading older than M5_STALE_SEC, younger than M5_EXPIRE_SEC
};

/* trend bars = ext.flg.temp_symbol bits: 1 "°Г" top, 2 " -" middle, 4 " _" bottom */
enum {
	M5_TREND_NONE   = 0,
	M5_TREND_UP     = 1,
	M5_TREND_UP45   = 3,
	M5_TREND_FLAT   = 2,
	M5_TREND_DOWN45 = 6,
	M5_TREND_DOWN   = 4
};

#define SMILE_OLD	7	// "(ooo)"

typedef struct _m5_t {
	u8 pwd[M5_PWD_MAX];
	u8 pwd_len;			// 0 = no password stored
	u8 pwd_loaded;		// password read from flash
	u8 authenticated;
	u8 params_requested;// 0x16 sent in this connection
	u8 mgdl;			// 1 = mg/dL, 0 = mmol/L
	u8 time_set;		// clock set by the app
	u8 have_bg;			// at least one reading received
	u8 stage;			// M5_STAGE_x
	u8 trend;			// M5_TREND_x
	s16 bg;				// mg/dL
	s16 bg_prev;		// mg/dL, previous reading
	u32 bg_ts;			// UTC epoch seconds of the reading
	s32 utc_offset;		// local time - UTC, seconds
	u32 age_sec;		// age of the reading, seconds
	u32 hb_age;			// age_sec at the last heartbeat
	/* text packet reassembly */
	u8 asm_op;
	u8 asm_len;
	u8 asm_buf[M5_ASM_BUF_SIZE];
} m5_t;

RAM m5_t m5;

/* ---------------------------------------------------------------- */

static void m5_notify(u8 *buf, u8 len) {
	if (wrk.ble_connected && (m5ValueInCCC & 1))
		bls_att_pushNotifyData(M5_DP_H, buf, len);
}

static void m5_send_op(u8 op) {
	u8 buf[3];
	buf[0] = op;
	buf[1] = 1; // packet 1
	buf[2] = 1; // of 1
	m5_notify(buf, sizeof(buf));
}

static void m5_load_pwd(void) {
	if (m5.pwd_loaded)
		return;
	m5.pwd_loaded = 1;
	s32 len = flash_read_cfg(m5.pwd, EEP_ID_M5P, M5_PWD_MAX);
	m5.pwd_len = (len > 0 && len <= M5_PWD_MAX) ? (u8)len : 0;
}

static void m5_store_pwd(const u8 *p, u8 len) {
	if (len > M5_PWD_MAX)
		len = M5_PWD_MAX;
	memcpy(m5.pwd, p, len);
	m5.pwd_len = len;
	flash_write_cfg(m5.pwd, EEP_ID_M5P, len);
}

static void m5_new_pwd(void) {
	static const u8 chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	u8 pwd[M5_PWD_GEN_LEN];
	u32 r = clock_time() ^ ((u32)mac_public[0] | ((u32)mac_public[1] << 8) | ((u32)mac_public[2] << 16) | ((u32)mac_public[3] << 24));
	for (u8 i = 0; i < M5_PWD_GEN_LEN; i++) {
		r = r * 1103515245 + 12345 + clock_time();
		pwd[i] = chars[(r >> 16) % (sizeof(chars) - 1)];
	}
	m5_store_pwd(pwd, M5_PWD_GEN_LEN);
}

static void m5_send_pwd(void) {
	u8 buf[3 + M5_PWD_MAX];
	buf[0] = M5_RX_PWD;
	buf[1] = 1;
	buf[2] = 1;
	memcpy(&buf[3], m5.pwd, m5.pwd_len);
	m5_notify(buf, 3 + m5.pwd_len);
}

/* decimal integer with optional '-', returns the number of bytes consumed */
static u8 m5_parse_int(const u8 *s, u8 len, s32 *out) {
	u8 i = 0;
	u8 neg = 0;
	s32 v = 0;
	if (i < len && s[i] == '-') {
		neg = 1;
		i++;
	}
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		v = v * 10 + (s[i] - '0');
		i++;
	}
	*out = neg ? -v : v;
	return i;
}

/* reassembles [n][total][text] packets, returns 1 when the text is complete */
static int m5_assemble(u8 op, u8 *d, u8 len) {
	if (len < 2)
		return 0;
	u8 n = d[0];
	u8 total = d[1];
	d += 2;
	len -= 2;
	if (n == 1 || op != m5.asm_op) {
		m5.asm_len = 0;
		m5.asm_op = op;
	}
	if (n == 0 || total == 0 || n > total || m5.asm_len + len > M5_ASM_BUF_SIZE) {
		m5.asm_len = 0;
		return 0;
	}
	memcpy(&m5.asm_buf[m5.asm_len], d, len);
	m5.asm_len += len;
	return n == total;
}

/* ---------------------------------------------------------------- */

static u32 m5_utc_now(void) {
	return wrk.utc_time_sec - m5.utc_offset;
}

static void m5_update_stage(void) {
	u8 st;
	if (!m5.have_bg)
		st = M5_STAGE_NONE;
	else if (m5.age_sec < M5_STALE_SEC)
		st = M5_STAGE_FRESH;
	else if (m5.age_sec < M5_EXPIRE_SEC)
		st = M5_STAGE_STALE;
	else
		st = M5_STAGE_NONE;
	if (m5.stage != st) {
		m5.stage = st;
		SET_LCD_UPDATE();
	}
}

/* 0x10 "<mg/dL> <UTC epoch seconds>" */
static void m5_on_bg(const u8 *s, u8 len) {
	s32 bg, ts;
	u8 n = m5_parse_int(s, len, &bg);
	if (n == 0 || bg <= 0 || bg > 1999)
		return;
	while (n < len && s[n] == ' ')
		n++;
	m5_parse_int(&s[n], len - n, &ts);
	if (!m5.have_bg || (u32)ts != m5.bg_ts) { // new reading (the app re-sends the last one at reconnect/refresh)
		m5.bg_prev = m5.have_bg ? m5.bg : (s16)bg;
		m5.bg = (s16)bg;
		m5.bg_ts = (u32)ts;
		m5.have_bg = 1;
		if (m5.time_set && ts > 0 && (u32)ts < m5_utc_now())
			m5.age_sec = m5_utc_now() - (u32)ts;
		else
			m5.age_sec = 0;
		m5.hb_age = 0;
	}
	m5_update_stage();
	SET_LCD_UPDATE();
}

/* 0x13 slope name: "DoubleUp", "SingleUp", "FortyFiveUp", "Flat",
 * "FortyFiveDown", "SingleDown", "DoubleDown", "NONE", "NOT COMPUTABLE", ... */
static void m5_on_slope(const u8 *s, u8 len) {
	u8 t = M5_TREND_NONE;
	u8 up = (len >= 2 && s[len - 2] == 'U' && s[len - 1] == 'p');
	u8 down = (len >= 4 && s[len - 4] == 'D' && s[len - 1] == 'n');
	if (len == 4 && s[0] == 'F' && s[1] == 'l')
		t = M5_TREND_FLAT;
	else if (len >= 9 && s[0] == 'F' && s[1] == 'o') // FortyFiveUp / FortyFiveDown
		t = up ? M5_TREND_UP45 : (down ? M5_TREND_DOWN45 : M5_TREND_NONE);
	else if (up)
		t = M5_TREND_UP;
	else if (down)
		t = M5_TREND_DOWN;
	if (m5.trend != t) {
		m5.trend = t;
		SET_LCD_UPDATE();
	}
}

/* 0x12 "<local epoch seconds>": the firmware clock displays utc_time_sec as-is,
 * so it is set to local time, like CMD_ID_UTC_TIME does. */
static void m5_on_time(const u8 *s, u8 len) {
	s32 t;
	if (m5_parse_int(s, len, &t) == 0 || t <= 0)
		return;
	wrk.utc_time_sec = (u32)t;
#if (DEV_SERVICES & SERVICE_TIME_ADJUST)
	utc_set_time_sec = wrk.utc_time_sec;
#endif
#if (DEV_SERVICES & SERVICE_HARD_CLOCK)
	rtc_set_utime(wrk.utc_time_sec);
#endif
	m5.time_set = 1;
	SET_LCD_UPDATE();
	// like M5_NightscoutMon: ask for all parameters (unit, ...) after the first time sync
	if (!m5.params_requested) {
		m5.params_requested = 1;
		m5_send_op(M5_RX_REQ_PARAMS);
	}
}

/* 0x14 "<local - UTC, seconds>" */
static void m5_on_time_offset(const u8 *s, u8 len) {
	s32 t;
	if (m5_parse_int(s, len, &t))
		m5.utc_offset = t;
}

/* 0x03 "true" (mg/dL) / "false" (mmol/L) */
static void m5_on_unit(const u8 *s, u8 len) {
	u8 mgdl = (len > 0 && s[0] == 't');
	if (m5.mgdl != mgdl) {
		m5.mgdl = mgdl;
		SET_LCD_UPDATE();
	}
}

/* ---------------------------------------------------------------- */

/* GATT write callback */
int m5Write(void *p) {
	rf_packet_att_data_t *req = (rf_packet_att_data_t *) p;
	u32 l = req->l2cap - 3;
	if (l < 1 || l > M5_PKT_MAX + 4)
		return 0;
	u8 op = req->dat[0];
	u8 *d = &req->dat[1];
	u8 len = (u8)(l - 1);
	switch (op) {
	case M5_TX_READ_PWD:
		m5_load_pwd();
		if (!m5.pwd_len)
			m5_new_pwd();
		// deviation from M5_NightscoutMon: the stored password is always sent back,
		// so re-installing the app does not require a device reset
		m5.authenticated = 1;
		m5_send_pwd();
		break;
	case M5_TX_AUTH:
		m5_load_pwd();
		if (!m5.pwd_len && len)
			m5_store_pwd(d, len); // first password seen is adopted
		if (m5.pwd_len && len == m5.pwd_len && memcmp(d, m5.pwd, len) == 0) {
			m5.authenticated = 1;
			m5_send_op(M5_RX_AUTH_OK);
		} else {
			m5.authenticated = 0;
			m5_send_op(M5_RX_AUTH_FAIL);
		}
		break;
	case M5_TX_READ_BATTERY:
		if (m5.authenticated) {
			u8 buf[2];
			buf[0] = M5_RX_BATTERY;
			buf[1] = measured_data.battery_level;
			m5_notify(buf, sizeof(buf));
		}
		break;
	case M5_TX_MGDL:
		if (m5_assemble(op, d, len))
			m5_on_unit(m5.asm_buf, m5.asm_len);
		break;
	case M5_TX_BG:
	case M5_TX_TIMESTAMP:
	case M5_TX_SLOPE:
	case M5_TX_TIME_OFFSET:
		if (!m5.authenticated)
			break;
		if (m5_assemble(op, d, len)) {
			if (op == M5_TX_BG)
				m5_on_bg(m5.asm_buf, m5.asm_len);
			else if (op == M5_TX_SLOPE)
				m5_on_slope(m5.asm_buf, m5.asm_len);
			else if (op == M5_TX_TIMESTAMP)
				m5_on_time(m5.asm_buf, m5.asm_len);
			else
				m5_on_time_offset(m5.asm_buf, m5.asm_len);
		}
		break;
	default: // colors, rotation, brightness, power off: nothing to do on a segment LCD
		break;
	}
	return 0;
}

/* once per second, from main_loop() */
void m5_task(void) {
	if (!m5.have_bg)
		return;
	m5.age_sec++;
	m5_update_stage();
	if (m5.authenticated
			&& m5.age_sec >= M5_HEARTBEAT_AFTER_SEC
			&& m5.age_sec - m5.hb_age >= M5_HEARTBEAT_PERIOD_SEC) {
		m5.hb_age = m5.age_sec;
		m5_send_op(M5_RX_HEARTBEAT);
	}
}

void m5_on_disconnect(void) {
	m5.authenticated = 0;
	m5.params_requested = 0;
	m5.asm_len = 0;
}

/* draws the glucose screen, returns 0 when the normal T/H screen must be shown */
int m5_lcd_show(void) {
	if (m5.stage == M5_STAGE_NONE)
		return 0;
	s16 big, small;
	s16 delta = m5.bg - m5.bg_prev;
	if (delta < 0)
		delta = -delta;
	if (m5.mgdl) {
		big = m5.bg;
		small = delta;
	} else { // mmol/L x10 = mg/dL / 1.8
		big = (m5.bg * 10 + 9) / 18;
		small = (delta * 10 + 9) / 18;
	}
	if (small > 99)
		small = 99;
	if (m5.stage == M5_STAGE_FRESH) {
		show_smiley((m5.bg < M5_BG_LOW_MGDL || m5.bg > M5_BG_HIGH_MGDL) ? SMILE_SAD : SMILE_HAPPY);
	} else {
		show_smiley(SMILE_OLD);
	}
	show_battery_symbol(measured_data.battery_level <= 10);
	show_small_number(small, false);
	show_temp_symbol((m5.stage == M5_STAGE_FRESH) ? (u8)(m5.trend << 5) : 0);
	if (m5.mgdl)
		show_big_number(big);
	else
		show_big_number_x10(big);
	lcd_flg.update_next_measure = 1;
	return 1;
}

#endif // USE_M5STACK_EMU
