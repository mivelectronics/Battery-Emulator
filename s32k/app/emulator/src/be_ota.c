/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_ota.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "be_cfg.h"
#include "be_wdt.h"
#include "link.h"

#ifdef CONFIG_BOOTLOADER_MCUBOOT
#include <zephyr/devicetree.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>

#include "be_core.h"
#include "be_gpio.h"
#include "be_sha256.h"

#define CONFIRM_AFTER_MS (60U * 1000U)
#define GIVE_UP_MS       (5U * 60U * 1000U)

#define SLOT0_ADDR  PARTITION_ADDRESS(slot0_partition)
#define SLOT0_SIZE  PARTITION_SIZE(slot0_partition)
#define SLOT1_ID    PARTITION_ID(slot1_partition)
#define SLOT1_SIZE  PARTITION_SIZE(slot1_partition)
#define SECTOR_SIZE 8192U
/* The last sector of slot 1 holds MCUboot's trailer (magic, swap status): the image stays below it */
#define MAX_IMAGE   (SLOT1_SIZE - SECTOR_SIZE)

#define SRAM_ADDR DT_REG_ADDR(DT_CHOSEN(zephyr_sram))
#define SRAM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_sram))
#define DTCM_ADDR DT_REG_ADDR(DT_CHOSEN(zephyr_dtcm))
#define DTCM_SIZE DT_REG_SIZE(DT_CHOSEN(zephyr_dtcm))

#define IMAGE_MAGIC     0x96f3b83dU
#define TLV_INFO_MAGIC  0x6907U
#define TLV_PROT_MAGIC  0x6908U
#define TLV_SHA256      0x10U
#define MARKER_MAGIC    0x4F544131U /* "OTA1" */
#define RESTART_MS      2000U

/* State of the image */
static enum be_image_state state = BE_IMAGE_CONFIRMED;
static bool cfg_ok;
static enum be_update_result result;
static char image_version[32]; /* "255.255.65535+4294967295" at most */
static char update_version[32];

/* "Update to version X pending": written before the image is marked, read by whichever image starts
 * next. Kept outside the "be" settings tree, so it never shows among the emulator's settings. */
struct ota_marker {
	uint32_t magic;
	char version[32];
};
static struct ota_marker marker;
static bool marker_found;

static int marker_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "pending") == 0 && len == sizeof(marker)) {
		marker_found = read_cb(cb_arg, &marker, sizeof(marker)) == sizeof(marker) &&
			       marker.magic == MARKER_MAGIC;
		marker.version[sizeof(marker.version) - 1] = '\0';
	}
	return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(beota, "beota", NULL, marker_set, NULL, NULL);

static void version_text(char *out, size_t cap, const uint8_t ver[8])
{
	const uint16_t rev = (uint16_t)(ver[2] | (ver[3] << 8));
	const uint32_t build = (uint32_t)ver[4] | ((uint32_t)ver[5] << 8) | ((uint32_t)ver[6] << 16) |
			       ((uint32_t)ver[7] << 24);

	if (build != 0U) {
		snprintf(out, cap, "%u.%u.%u+%u", ver[0], ver[1], rev, (unsigned int)build);
	} else {
		snprintf(out, cap, "%u.%u.%u", ver[0], ver[1], rev);
	}
}

void be_ota_init(int cfg_err)
{
	cfg_ok = cfg_err == 0;

	struct mcuboot_img_header h;

	if (boot_read_bank_header(PARTITION_ID(slot0_partition), &h, sizeof(h)) == 0) {
		const struct mcuboot_img_sem_ver *v = &h.h.v1.sem_ver;
		const uint8_t raw[8] = {v->major, v->minor, (uint8_t)v->revision, (uint8_t)(v->revision >> 8),
					(uint8_t)v->build_num, (uint8_t)(v->build_num >> 8),
					(uint8_t)(v->build_num >> 16), (uint8_t)(v->build_num >> 24)};

		version_text(image_version, sizeof(image_version), raw);
	}
	(void)settings_load_subtree("beota");

	if (mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT) {
		/* MCUboot swapped this image in for a test: it swaps back unless it is confirmed */
		state = BE_IMAGE_TEST;
		result = BE_UPDATE_ON_TEST;
		strncpy(update_version, marker_found ? marker.version : image_version,
			sizeof(update_version) - 1);
	} else {
		/* A confirmed image, or one written through SWD without a trailer: mark it, so that a
		 * later update finds a confirmed image in slot 0 */
		if (!boot_is_img_confirmed()) {
			printk("image written without a confirm, confirming: %d\n", boot_write_img_confirmed());
		}
		state = BE_IMAGE_CONFIRMED;
		if (marker_found) {
			/* An update was marked, and this image is not it on test: it did not confirm (or
			 * MCUboot did not take it) and the old image is back */
			result = BE_UPDATE_REVERTED;
			strncpy(update_version, marker.version, sizeof(update_version) - 1);
			(void)settings_delete("beota/pending");
		}
	}
	printk("image %s: %s\n", image_version,
	       state == BE_IMAGE_TEST ? "test image, not confirmed" : "confirmed");
	if (result == BE_UPDATE_REVERTED) {
		printk("update to %s did not confirm: the previous image is back\n", update_version);
	}
}

/* ---- Receiving an update ---- */

enum ota_phase { OTA_IDLE, OTA_RECEIVING, OTA_DONE };

static enum ota_phase phase;
static const struct flash_area *fa;
static uint32_t ota_image_id, ota_total, ota_written, ota_erased_to;
static uint8_t ota_sha[32];
static uint8_t ota_ver[8];
static uint16_t ota_hdr_size, ota_prot_size;
static uint32_t ota_img_size;
static int64_t ota_last_ms;
static uint8_t io_buf[BE_LINK_OTA_DATA_MAX + 8] __aligned(8);
static uint8_t rd_buf[BE_LINK_OTA_DATA_MAX + 8] __aligned(8);

static void send_ack(const struct be_link_ota_hdr *h, int status)
{
	struct be_link_ota_ack a = {
		.ack = {.acked_type = BE_LINK_OTA, .acked_seq = h->id, .status = (int16_t)status},
		.op = h->op,
		.offset = ota_written,
	};

	(void)link_send(BE_LINK_ACK, &a, sizeof(a));
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* Section 7 of OTA_PLAN.md: nothing may be switching or carrying current while the core stops */
static int safety_check(void)
{
	if ((be_gpio_levels() & 0x0FU) != 0U) {
		return BE_OTA_E_CONTACTORS;
	}
	if (!be_core_active()) {
		return 0;
	}
	uint16_t engaged = 0, current = 0, p[4] = {0};

	(void)be_core_reg_read(205, &engaged);
	(void)be_core_reg_read(212, &current);
	for (int i = 0; i < 4; i++) {
		(void)be_core_reg_read(213 + i, &p[i]);
	}
	if (engaged != 0U) {
		return BE_OTA_E_CONTACTORS;
	}
	if ((p[0] | p[1] | p[2] | p[3]) != 0U) {
		return BE_OTA_E_NOT_PAUSED;
	}
	const int16_t dA = (int16_t)current;

	if (dA >= 10 || dA <= -10) {
		return BE_OTA_E_CURRENT;
	}
	return 0;
}

/* Ends a transfer that cannot go on: nothing is marked, the old image starts again with its core */
static void give_up(const char *why, int status)
{
	printk("update: %s (%d), restarting into the running image\n", why, status);
	phase = OTA_DONE;
	be_cfg_system_reset_in(RESTART_MS);
}

static int on_begin(const struct be_link_ota_begin *b)
{
	const uint8_t *hd = b->head;

	if (state == BE_IMAGE_TEST) {
		return BE_OTA_E_TEST_IMAGE;
	}
	if (le32(hd) != IMAGE_MAGIC) {
		return BE_OTA_E_NOT_IMAGE;
	}
	const uint32_t load_addr = le32(hd + 4);
	const uint16_t hdr_size = le16(hd + 8);
	const uint32_t img_size = le32(hd + 12);
	const uint32_t flags = le32(hd + 16);

	if (flags != 0U || load_addr != 0U) {
		return BE_OTA_E_FLAGS;
	}
	if (hdr_size != CONFIG_ROM_START_OFFSET) {
		return BE_OTA_E_VECTORS;
	}
	if (b->hdr.total > MAX_IMAGE) {
		return BE_OTA_E_TOO_BIG;
	}
	if (b->hdr.total < (uint32_t)hdr_size + img_size + 4U) {
		return BE_OTA_E_NOT_IMAGE;
	}
	int err = safety_check();

	if (err) {
		return err;
	}
	/* From here on the old image only comes back through a restart */
	printk("update: %u bytes, stopping the core\n", (unsigned int)b->hdr.total);
	be_core_halt();
	if (fa == NULL && flash_area_open(SLOT1_ID, &fa) != 0) {
		give_up("slot 1 does not open", BE_OTA_E_FLASH);
		return BE_OTA_E_FLASH;
	}
	/* The trailer sector: boot_request_upgrade() writes its magic there at the end */
	if (flash_area_erase(fa, SLOT1_SIZE - SECTOR_SIZE, SECTOR_SIZE) != 0) {
		give_up("erasing the trailer failed", BE_OTA_E_FLASH);
		return BE_OTA_E_FLASH;
	}
	phase = OTA_RECEIVING;
	ota_image_id = b->hdr.image_id;
	ota_total = b->hdr.total;
	ota_written = 0;
	ota_erased_to = 0;
	ota_hdr_size = hdr_size;
	ota_prot_size = le16(hd + 10);
	ota_img_size = img_size;
	memcpy(ota_sha, b->sha256, sizeof(ota_sha));
	memcpy(ota_ver, hd + 20, sizeof(ota_ver));
	return 0;
}

static int on_data(const struct be_link_ota_hdr *h, const uint8_t *data, uint16_t len)
{
	if (phase != OTA_RECEIVING || h->image_id != ota_image_id) {
		return BE_OTA_E_SEQUENCE;
	}
	if (len > 0U && h->offset + len == ota_written) {
		return 0; /* repeated because our ack was lost: written already */
	}
	const uint32_t end = h->offset + len;

	if (h->offset != ota_written || len == 0U || len > BE_LINK_OTA_DATA_MAX || end > ota_total ||
	    ((len % 8U) != 0U && end != ota_total)) {
		return BE_OTA_E_SEQUENCE;
	}
	const uint32_t wlen = ROUND_UP(len, 8U);

	/* Erase what this frame reaches; the whole frame is in, the ack follows */
	while (ota_erased_to < h->offset + wlen) {
		if (flash_area_erase(fa, ota_erased_to, SECTOR_SIZE) != 0) {
			return BE_OTA_E_FLASH;
		}
		ota_erased_to += SECTOR_SIZE;
	}
	memset(io_buf, 0xFF, wlen);
	memcpy(io_buf, data, len);
	if (flash_area_write(fa, h->offset, io_buf, wlen) != 0 ||
	    flash_area_read(fa, h->offset, rd_buf, wlen) != 0 || memcmp(io_buf, rd_buf, wlen) != 0) {
		return BE_OTA_E_FLASH;
	}
	ota_written = end;
	return 0;
}

/* Board id in the protected TLV area, right after the image */
static int check_tlvs(void)
{
	uint32_t off = (uint32_t)ota_hdr_size + ota_img_size;
	uint8_t info[4];

	if (flash_area_read(fa, off, info, sizeof(info)) != 0) {
		return BE_OTA_E_TLV;
	}
	if (le16(info) != TLV_PROT_MAGIC) {
		return BE_OTA_E_BOARD; /* no protected TLVs: no board id */
	}
	const uint32_t end = off + le16(info + 2);

	if (end > ota_total) {
		return BE_OTA_E_TLV;
	}
	for (off += sizeof(info); off + 4U <= end;) {
		uint8_t t[4];

		if (flash_area_read(fa, off, t, sizeof(t)) != 0) {
			return BE_OTA_E_TLV;
		}
		const uint16_t type = le16(t), len = le16(t + 2);

		if (off + 4U + len > end) {
			return BE_OTA_E_TLV;
		}
		if (type == BE_OTA_TLV_BOARD) {
			char name[32] = {0};

			if (len != strlen(CONFIG_BOARD) || len >= sizeof(name) ||
			    flash_area_read(fa, off + 4U, name, len) != 0) {
				return BE_OTA_E_BOARD;
			}
			return strcmp(name, CONFIG_BOARD) == 0 ? 0 : BE_OTA_E_BOARD;
		}
		off += 4U + len;
	}
	return BE_OTA_E_BOARD;
}

/* The image's own SHA-256 (TLV 0x10 in the unprotected TLV area), the hash MCUboot checks */
static int find_hash_tlv(uint32_t at, uint8_t out[32])
{
	uint8_t info[4];

	if (flash_area_read(fa, at, info, sizeof(info)) != 0) {
		return BE_OTA_E_FLASH;
	}
	if (le16(info) != TLV_INFO_MAGIC) {
		return BE_OTA_E_TLV;
	}
	const uint32_t end = at + le16(info + 2);

	if (end > ota_total) {
		return BE_OTA_E_TLV;
	}
	for (uint32_t off = at + sizeof(info); off + 4U <= end;) {
		uint8_t t[4];

		if (flash_area_read(fa, off, t, sizeof(t)) != 0) {
			return BE_OTA_E_FLASH;
		}
		const uint16_t type = le16(t), len = le16(t + 2);

		if (off + 4U + len > end) {
			return BE_OTA_E_TLV;
		}
		if (type == TLV_SHA256 && len == 32U) {
			return flash_area_read(fa, off + 4U, out, 32) == 0 ? 0 : BE_OTA_E_FLASH;
		}
		off += 4U + len;
	}
	return BE_OTA_E_TLV;
}

/*
 * Reads slot 1 back once: the image's own SHA-256 over header, image and protected TLVs (as MCUboot will
 * check it) and, when BEGIN carried one, the SHA-256 of the whole file as sent.
 */
static int check_image(void)
{
	static const uint8_t none[32];
	const uint32_t hashed = (uint32_t)ota_hdr_size + ota_img_size + ota_prot_size;
	struct be_sha256 whole, image;
	uint8_t want[32], d_whole[32], d_image[32];

	if (hashed + 4U > ota_total) {
		return BE_OTA_E_TLV;
	}
	int err = find_hash_tlv(hashed, want);

	if (err) {
		return err;
	}
	be_sha256_init(&whole);
	be_sha256_init(&image);
	for (uint32_t off = 0; off < ota_total; off += sizeof(rd_buf)) {
		const uint32_t n = MIN(sizeof(rd_buf), ota_total - off);

		if (flash_area_read(fa, off, rd_buf, n) != 0) {
			return BE_OTA_E_FLASH;
		}
		be_sha256_update(&whole, rd_buf, n);
		if (off < hashed) {
			be_sha256_update(&image, rd_buf, MIN(n, hashed - off));
		}
		if ((off & 0xFFFFU) == 0U) {
			be_wdt_main_alive();
		}
	}
	be_sha256_final(&whole, d_whole);
	be_sha256_final(&image, d_image);
	if (memcmp(d_image, want, sizeof(want)) != 0) {
		return BE_OTA_E_HASH;
	}
	if (memcmp(ota_sha, none, sizeof(none)) != 0 && memcmp(d_whole, ota_sha, sizeof(d_whole)) != 0) {
		return BE_OTA_E_HASH;
	}
	/* The vector table: initial stack in RAM, reset handler (Thumb) in slot 0 behind the header */
	uint8_t vt[8];

	if (flash_area_read(fa, ota_hdr_size, vt, sizeof(vt)) != 0) {
		return BE_OTA_E_FLASH;
	}
	const uint32_t msp = le32(vt), reset = le32(vt + 4);
	const bool msp_ok = (msp > SRAM_ADDR && msp <= SRAM_ADDR + SRAM_SIZE) ||
			    (msp > DTCM_ADDR && msp <= DTCM_ADDR + DTCM_SIZE);
	const bool reset_ok = (reset & 1U) != 0U && reset > SLOT0_ADDR + ota_hdr_size &&
			      reset < SLOT0_ADDR + SLOT0_SIZE;

	if (!msp_ok || !reset_ok) {
		return BE_OTA_E_VECTORS;
	}
	return check_tlvs();
}

static int on_finish(const struct be_link_ota_hdr *h)
{
	if (phase == OTA_DONE && h->image_id == ota_image_id) {
		return 0; /* repeated: marked already, the restart is due */
	}
	if (phase != OTA_RECEIVING || h->image_id != ota_image_id || ota_written != ota_total) {
		return BE_OTA_E_SEQUENCE;
	}
	int err = check_image();

	if (err) {
		give_up("the image in slot 1 failed its check", err);
		return err;
	}
	struct ota_marker m = {.magic = MARKER_MAGIC};

	version_text(m.version, sizeof(m.version), ota_ver);
	(void)settings_save_one("beota/pending", &m, sizeof(m));
	if (boot_request_upgrade(BOOT_UPGRADE_TEST) != 0) {
		(void)settings_delete("beota/pending");
		give_up("marking slot 1 failed", BE_OTA_E_MARK);
		return BE_OTA_E_MARK;
	}
	printk("update %s in slot 1 checked and marked, restarting\n", m.version);
	phase = OTA_DONE;
	be_cfg_system_reset_in(RESTART_MS);
	return 0;
}

bool be_ota_frame(const struct be_link_frame *f)
{
	if (f->type != BE_LINK_OTA) {
		return false;
	}
	struct be_link_ota_hdr h;

	if (f->len < sizeof(h)) {
		return true;
	}
	memcpy(&h, f->payload, sizeof(h));
	int status;

	switch (h.op) {
	case BE_OTA_BEGIN:
		if (f->len < sizeof(struct be_link_ota_begin)) {
			status = BE_OTA_E_SEQUENCE;
		} else if (phase == OTA_RECEIVING && h.image_id == ota_image_id && ota_written == 0U) {
			status = 0; /* repeated because our ack was lost */
		} else if (phase != OTA_IDLE) {
			status = BE_OTA_E_SEQUENCE;
		} else {
			struct be_link_ota_begin b;

			memcpy(&b, f->payload, sizeof(b));
			status = on_begin(&b);
		}
		break;
	case BE_OTA_DATA:
		status = on_data(&h, f->payload + sizeof(h), (uint16_t)(f->len - sizeof(h)));
		if (status == BE_OTA_E_FLASH) {
			give_up("writing slot 1 failed", status);
		}
		break;
	case BE_OTA_FINISH:
		status = on_finish(&h);
		break;
	case BE_OTA_ABORT:
		status = 0;
		if (phase == OTA_RECEIVING) {
			give_up("aborted by the network module", 0);
		}
		break;
	default:
		status = BE_OTA_E_SEQUENCE;
		break;
	}
	ota_last_ms = k_uptime_get();
	send_ack(&h, status);
	return true;
}

bool be_ota_busy(void)
{
	return phase != OTA_IDLE;
}

void be_ota_poll(void)
{
	const int64_t up = k_uptime_get();

	if (phase == OTA_RECEIVING && up - ota_last_ms > BE_LINK_OTA_TIMEOUT_MS) {
		give_up("no data for 10 s", 0);
	}
	if (state != BE_IMAGE_TEST) {
		return;
	}
	/* A start after a watchdog or fault reset does not confirm */
	const enum be_reset_cause cause = be_wdt_reset_cause();
	const bool clean_start = cause != BE_RESET_WDT_FIRST && cause != BE_RESET_FATAL &&
				 cause != BE_RESET_WDT_HW;

	if (cfg_ok && clean_start && link_peer_alive() && up >= CONFIRM_AFTER_MS) {
		/* The marker first: once confirmed, a leftover marker would read as "reverted" */
		(void)settings_delete("beota/pending");
		int err = boot_write_img_confirmed();

		printk("image confirmed: %d\n", err);
		if (err == 0) {
			state = BE_IMAGE_CONFIRMED;
			result = BE_UPDATE_INSTALLED;
		}
		return;
	}
	/*
	 * Not confirmed within 5 minutes, whatever held it (no link, settings not read, a start after a
	 * watchdog or fault reset): restart, MCUboot swaps the old image back. Waiting longer would only
	 * leave the device on an image that MCUboot drops at the next reset anyway.
	 */
	if (up >= GIVE_UP_MS && phase == OTA_IDLE) {
		printk("test image not confirmed after 5 minutes (%s), restarting for the old image\n",
		       !cfg_ok ? "settings not read" : !clean_start ? "start after a watchdog or fault"
						 : "no link to the ESP32");
		state = BE_IMAGE_CONFIRMED; /* once */
		be_cfg_system_reset();
	}
}

enum be_image_state be_ota_image_state(void)
{
	return state;
}

enum be_update_result be_ota_update_result(void)
{
	return result;
}

const char *be_ota_image_version(void)
{
	return image_version;
}

const char *be_ota_update_version(void)
{
	return update_version;
}

#else /* !CONFIG_BOOTLOADER_MCUBOOT */

void be_ota_init(int cfg_err)
{
	ARG_UNUSED(cfg_err);
}

void be_ota_poll(void)
{
}

bool be_ota_frame(const struct be_link_frame *f)
{
	if (f->type != BE_LINK_OTA) {
		return false;
	}
	struct be_link_ota_hdr h = {0};

	memcpy(&h, f->payload, MIN(f->len, sizeof(h)));
	struct be_link_ota_ack a = {
		.ack = {.acked_type = BE_LINK_OTA, .acked_seq = h.id, .status = BE_OTA_E_NO_BOOTLOADER},
		.op = h.op,
	};

	(void)link_send(BE_LINK_ACK, &a, sizeof(a));
	return true;
}

bool be_ota_busy(void)
{
	return false;
}

enum be_image_state be_ota_image_state(void)
{
	return BE_IMAGE_NO_BOOTLOADER;
}

enum be_update_result be_ota_update_result(void)
{
	return BE_UPDATE_NONE;
}

const char *be_ota_image_version(void)
{
	return "";
}

const char *be_ota_update_version(void)
{
	return "";
}

#endif
