// BK-W8 A/B slot: tell the vendor boot hook that this boot is healthy.
//
// On the iWFCam BK-W8, OpenBeken runs from a slot inside the vendor app partition.
// A hook in the vendor app reads ab_boot from the vendor EasyFlash ENV, adds 1, saves
// it and then jumps into the slot. The hook does not jump when ab_boot is 1 or more.
// Without this file, every reboot therefore stays in the vendor app.
//
// When OpenBeken marks its own boot complete, this file sets ab_boot back to 0. If a
// slot image crashes or hangs before that point, ab_boot stays at 1 and the next boot
// stays in the vendor app.
//
// The vendor ENV is EasyFlash V3 in one 4 KB sector at 0x3FF000. The upper half of
// flash is not encoded, so a flash read returns these bytes as they are:
//   +0  u32  absolute end address of the data
//   +4  u32  CRC32 over the 4 bytes at +0, then over the data from +8 to the end
//   +8  "key=value\0" strings, each padded to a 4-byte boundary
// "ab_boot=1" becomes "ab_boot=0". One byte changes, the layout stays the same, and
// the CRC is calculated again. If anything in the sector is not as expected, this
// file writes nothing.

#include "../../new_common.h"
#include "../../logging/logging.h"

#if PLATFORM_BK7252

#include "BkDriverFlash.h"

// BkDriverFlash.c defines these, but no header declares them.
int hal_flash_lock(void);
int hal_flash_unlock(void);

#define ENV_ADDR		0x3FF000
#define ENV_SIZE		0x1000
#define SLOT_HDR_ADDR	0xD6000
#define SLOT_IMG_START	0xD6100
#define SLOT_IMG_END	0x18F000
#define SLOT_MAGIC		0x39574B42	// "BKW9"
#define AB_KEY			"ab_boot="
#define AB_KEY_LEN		8

void HAL_ABSlot_MarkBootOK();

static uint32_t crc32_update(uint32_t crc, const byte *p, int len) {
	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
	}
	return ~crc;
}

static uint32_t env_crc(const byte *env, int dataEnd) {
	return crc32_update(crc32_update(0, env, 4), env + 8, dataEnd - 8);
}

// Returns the offset of the ab_boot value in env, or -1.
static int find_ab_boot(const byte *env, int dataEnd, int *valueLen) {
	int p = 8;
	while (p < dataEnd) {
		int n = 0;
		while (p + n < dataEnd && env[p + n])
			n++;
		if (p + n == dataEnd)
			return -1;
		if (n > AB_KEY_LEN && !memcmp(env + p, AB_KEY, AB_KEY_LEN)) {
			*valueLen = n - AB_KEY_LEN;
			return p + AB_KEY_LEN;
		}
		p = (p + n + 1 + 3) & ~3;
	}
	return -1;
}

static const char *g_abStatus = "not run yet";

const char *HAL_ABSlot_Status() {
	return g_abStatus;
}

static void set_status(const char *status) {
	g_abStatus = status;
	ADDLOG_INFO(LOG_FEATURE_CFG, "ABSlot: %s", status);
}

void HAL_ABSlot_MarkBootOK() {
	uint32_t self = (uint32_t)&HAL_ABSlot_MarkBootOK;
	uint32_t magic, end, crc;
	byte *env = 0, *check = 0;
	int dataEnd, value, valueLen;

	if (self < SLOT_IMG_START || self >= SLOT_IMG_END) {
		set_status("not running from the BK-W8 slot, ab_boot not changed");
		return;
	}
	// Read the slot header through the CPU address space. The flash stores the app
	// region encoded, so a flash read at this address does not return the header.
	memcpy(&magic, (void *)SLOT_HDR_ADDR, 4);
	if (magic != SLOT_MAGIC) {
		set_status("slot header magic is not BKW9, ab_boot not changed");
		return;
	}

	env = os_malloc(ENV_SIZE);
	check = os_malloc(ENV_SIZE);
	if (env == 0 || check == 0) {
		set_status("no memory, ab_boot not changed");
		goto out;
	}
	hal_flash_lock();
	bk_flash_abs_addr_read(ENV_ADDR, env, ENV_SIZE);
	hal_flash_unlock();

	memcpy(&end, env, 4);
	memcpy(&crc, env + 4, 4);
	dataEnd = (int)(end - ENV_ADDR);
	if (end < ENV_ADDR + 8 || end > ENV_ADDR + ENV_SIZE || (dataEnd & 3)) {
		set_status("vendor ENV end address is not valid, ab_boot not changed");
		goto out;
	}
	if (env_crc(env, dataEnd) != crc) {
		set_status("vendor ENV CRC is not valid, ab_boot not changed");
		goto out;
	}
	value = find_ab_boot(env, dataEnd, &valueLen);
	if (value < 0 || valueLen != 1 || env[value] < '0' || env[value] > '9') {
		set_status("no single-digit ab_boot in vendor ENV, not changed");
		goto out;
	}
	if (env[value] == '0') {
		set_status("ab_boot is already 0");
		goto out;
	}

	env[value] = '0';
	crc = env_crc(env, dataEnd);
	memcpy(env + 4, &crc, 4);

	hal_flash_lock();
	bk_flash_abs_addr_write(ENV_ADDR, env, ENV_SIZE, 1);
	bk_flash_abs_addr_read(ENV_ADDR, check, ENV_SIZE);
	hal_flash_unlock();
	if (memcmp(env, check, ENV_SIZE))
		set_status("ab_boot write failed, read-back does not match");
	else
		set_status("boot is healthy, ab_boot=0 saved, next boot goes to OpenBeken");
out:
	if (env)
		os_free(env);
	if (check)
		os_free(check);
}

#endif
