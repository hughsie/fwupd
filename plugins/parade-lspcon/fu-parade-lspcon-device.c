/*
 * Copyright (C) 2021 Peter Marheine <pmarheine@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <linux/i2c-dev.h>

#include "fu-parade-lspcon-device.h"

/* device registers are split into pages, where
 * each page has its own I2C address */
#define I2C_ADDR_PAGE2			0x4A
#define REG_ADDR_CLT2SPI		0x82
/* FLASH_ADDR_* are the upper 16 bits of the 24-bit flash address that gets
 * mapped into page 7. Writing 0x01, 0x42 will map the 256 bytes from 0x420100
 * into page 7. */
#define REG_ADDR_FLASH_ADDR_LO		0x8E
#define REG_ADDR_FLASH_ADDR_HI		0x8F
/* 16-deep SPI write and read buffer FIFOs */
#define REG_ADDR_WR_FIFO		0x90
#define REG_ADDR_RD_FIFO		0x91
/* Low nibble is write operation length, high nibble for read commands.
 * Reset to 0 after command completion. */
#define REG_ADDR_SPI_LEN		0x92

#define REG_ADDR_SPI_CTL		0x93
/* set to do a write-only transaction */
#define SPI_CTL_NOREAD			0x04
/* set to begin executing command */
#define SPI_CTL_TRIGGER			0x01

/* operation status fields: set to 1 when operation begins, 2 when command has been
 * sent, reset to 0 when command completed */
#define REG_ADDR_SPI_STATUS		0x9e
/* byte programming */
#define SPI_STATUS_BP_MASK		0x03
/* sector erase */
#define SPI_STATUS_SE_MASK		0x0C
/* chip erase */
#define SPI_STATUS_CE_MASK		0x30

/* write WR_PROTECT_DISABLE to permit flash write operations */
#define REG_ADDR_WR_PROTECT		0xB3
#define WR_PROTECT_DISABLE		0x10

/* MPU control register */
#define REG_ADDR_MPU			0xBC

/* write a magic sequence to this register to enable writes to
 * mapped memory via page 7, or anything else to disable */
#define REG_ADDR_MAP_WRITE		0xDA

#define I2C_ADDR_PAGE5			0x4D
#define REG_ADDR_ACTIVE_PARTITION	0x0E

#define I2C_ADDR_PAGE7			0x4F

#define FLASH_BLOCK_SIZE		0x10000

/*
 * user1: 0x10000 - 0x20000
 * user2: 0x20000 - 0x30000
 * flag:  0x00002 - 0x00004
 */
struct _FuParadeLspconDevice {
	FuI2cDevice		 parent_instance;
	guint8			 active_partition;
	gchar			*aux_device_name;
};

G_DEFINE_TYPE (FuParadeLspconDevice, fu_parade_lspcon_device, FU_TYPE_I2C_DEVICE)

static void
fu_parade_lspcon_device_init (FuParadeLspconDevice *self)
{
	FuDevice *device = FU_DEVICE (self);
	fu_device_set_vendor (device, "Parade Technologies");
	fu_device_add_vendor_id (device, "PCI:0x1AF8");
	fu_device_add_protocol (device, "com.paradetech.ps176");
	fu_device_add_icon (device, "video-display");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_DUAL_IMAGE);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_CAN_VERIFY);
	fu_device_set_firmware_size (device, 0x10000);
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_PAIR);
}

static void
fu_parade_lspcon_device_finalize (GObject *object)
{
	FuParadeLspconDevice *self = FU_PARADE_LSPCON_DEVICE (object);
	g_free (self->aux_device_name);
}

static gboolean
fu_parade_lspcon_device_set_quirk_kv (FuDevice *device,
				      const gchar *key,
				      const gchar *value,
				      GError **error)
{
	FuParadeLspconDevice *self = FU_PARADE_LSPCON_DEVICE (device);

	if (g_strcmp0 (key, "ParadeLspconAuxDeviceName") == 0) {
		self->aux_device_name = g_strdup (value);
		return TRUE;
	}
	return FU_DEVICE_CLASS (fu_parade_lspcon_device_parent_class)
		->set_quirk_kv (device, key, value, error);
}

static gboolean
fu_parade_lspcon_device_probe (FuDevice *device, GError **error)
{
	FuParadeLspconDevice *self = FU_PARADE_LSPCON_DEVICE (device);
	FuContext *context = fu_device_get_context (device);
	FuUdevDevice *udev_device = FU_UDEV_DEVICE (device);
	const gchar *device_name;
	g_autofree gchar *instance_id_hwid = NULL;
	g_autofree gchar *instance_id = NULL;

	/* custom instance IDs to get device quirks */
	instance_id = g_strdup_printf ("PARADE-LSPCON\\NAME_%s",
				       fu_udev_device_get_sysfs_attr (udev_device, "name", NULL));
	fu_device_add_instance_id (device, instance_id);
	instance_id_hwid = g_strdup_printf ("%s&FAMILY_%s", instance_id,
					    fu_context_get_hwid_value (context,
								       FU_HWIDS_KEY_FAMILY));
	fu_device_add_instance_id_full (device, instance_id_hwid,
					FU_DEVICE_INSTANCE_FLAG_ONLY_QUIRKS);

	device_name = fu_device_get_name (device);
	if (g_strcmp0 (device_name, "PS175") != 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "device name %s is not supported by this plugin",
			     device_name);
		return FALSE;
	}

	/* should know which aux device over which we read DPCD version */
	if (self->aux_device_name == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "ParadeLspconAuxDeviceName must be specified");
		return FALSE;
	}

	/* FuI2cDevice->probe */
	return FU_DEVICE_CLASS (fu_parade_lspcon_device_parent_class)->probe (device, error);
}

static gboolean
fu_parade_lspcon_ensure_i2c_address (FuParadeLspconDevice *self,
				     guint8 address,
				     GError **error)
{
	if (!fu_udev_device_ioctl (FU_UDEV_DEVICE (self),
				   I2C_SLAVE, (guint8 *) (guintptr) address, NULL,
				   error)) {
		g_prefix_error (error, "failed to set I2C slave address: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_parade_lspcon_device_open (FuDevice *device, GError **error)
{
	if (!FU_DEVICE_CLASS (fu_parade_lspcon_device_parent_class)->open (device, error))
		return FALSE;

	/* general assumption is that page 2 is selected: code that uses another address
	 * should use an address guard to ensure it gets reset */
	return fu_parade_lspcon_ensure_i2c_address (FU_PARADE_LSPCON_DEVICE (device),
						    I2C_ADDR_PAGE2,
						    error);
}

/**
 * creates a scope in which the device's target I2C address is something
 * other than page 2, and resets it to page 2 when the scope is left.
 */
typedef struct {
	FuParadeLspconDevice *device;
} FuParadeLspconI2cAddressGuard;

static FuParadeLspconI2cAddressGuard *
fu_parade_lspcon_i2c_address_guard_new (FuParadeLspconDevice *self, guint8 new_address, GError **error)
{
	FuParadeLspconI2cAddressGuard *out;

	if (!fu_parade_lspcon_ensure_i2c_address (self, new_address, error))
		return NULL;
	out = g_new0 (FuParadeLspconI2cAddressGuard, 1);
	out->device = self;
	return out;
}

static void
fu_parade_lspcon_i2c_address_guard_free (FuParadeLspconI2cAddressGuard *guard)
{
	g_autoptr(GError) error_local = NULL;
	if (!fu_parade_lspcon_ensure_i2c_address (guard->device,
						  I2C_ADDR_PAGE2,
						  &error_local)) {
		g_warning ("failed to set page2 back: %s", error_local->message);
	}
	g_free (guard);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuParadeLspconI2cAddressGuard,
			      fu_parade_lspcon_i2c_address_guard_free);

static gboolean
fu_parade_lspcon_write_register (FuParadeLspconDevice *self,
				 guint8 register_addr,
				 guint8 value,
				 GError **error)
{
	guint8 transaction[] = { register_addr, value };
	return fu_i2c_device_write_full (FU_I2C_DEVICE (self), transaction,
					 sizeof(transaction), error);
}

static gboolean
fu_parade_lspcon_read_register (FuParadeLspconDevice *self,
				guint8 register_addr,
				guint8 *value,
				GError **error)
{
	FuI2cDevice *i2c_device = FU_I2C_DEVICE (self);
	if (!fu_i2c_device_write (i2c_device, register_addr, error))
		return FALSE;
	return fu_i2c_device_read (i2c_device, value, error);
}

/* map the page containing the given address into page 7 */
static gboolean
fu_parade_lspcon_map_page (FuParadeLspconDevice *self, guint32 address, GError **error)
{
	if (!fu_parade_lspcon_write_register (self,
					      REG_ADDR_FLASH_ADDR_HI,
					      address >> 16,
					      error))
		return FALSE;
	return fu_parade_lspcon_write_register (self,
						REG_ADDR_FLASH_ADDR_LO,
						address >> 8,
						error);
}

/* wait until the specified register masked with mask reads the expected
 * value, up to 10 seconds */
static gboolean
fu_parade_lspcon_poll_register (FuParadeLspconDevice *self,
				guint8 register_address,
				guint8 mask,
				guint8 expected,
				GError **error)
{
	guint8 value;
	g_autoptr(GTimer) timer = g_timer_new();

	do {
		if (!fu_parade_lspcon_read_register (self,
						     register_address,
						     &value,
						     error))
			return FALSE;
		if ((value & mask) == expected)
			return TRUE;
	} while (g_timer_elapsed (timer, NULL) <= 10.0);

	g_set_error (error,
		     G_IO_ERROR,
		     G_IO_ERROR_TIMED_OUT,
		     "register %x did not read %x (mask %x) within 10 seconds: read %x",
		     register_address, expected, mask, value);
	return FALSE;
}

static gboolean
fu_parade_lspcon_flash_read (FuParadeLspconDevice *self,
			     guint32 base_address,
			     guint8 *data,
			     gsize len,
			     GError **error)
{
	FuDevice *device = FU_DEVICE (self);
	FuI2cDevice *i2c_device = FU_I2C_DEVICE (self);
	const gsize target_len = len;

	while (len > 0) {
		/* page 7 reads always start from the base of the mapped window- we'll
		 * read the whole page then pull out the parts we care about, using the
		 * full page everywhere except possibly in the first and last reads */
		guint8 page_data[256] = { 0x0 };
		guint8 page_data_start = base_address & 0xFF;
		gsize page_data_take = MIN ((gssize) len, 256 - page_data_start);
		g_autoptr(FuParadeLspconI2cAddressGuard) guard = NULL;

		if (!fu_parade_lspcon_map_page (self, base_address, error))
			return FALSE;
		guard = fu_parade_lspcon_i2c_address_guard_new (self,
								I2C_ADDR_PAGE7,
								error);
		if (guard == NULL)
			return FALSE;
		if (!fu_i2c_device_read_full (i2c_device, page_data, 256, error))
			return FALSE;

		memcpy (data, page_data + page_data_start, page_data_take);
		base_address += page_data_take;
		data += page_data_take;
		len -= page_data_take;

		fu_device_set_progress_full (device, target_len - len, target_len);
	}

	return TRUE;
}

static gboolean
fu_parade_lspcon_flash_transmit_command (FuParadeLspconDevice *self,
					 const guint8 *command,
					 gsize command_len,
					 GError **error)
{
	/* write length field is 4 bits wide */
	g_return_val_if_fail (command_len > 0 && command_len <= 16, FALSE);

	/* fill transmit buffer */
	for (gsize i = 0; i < command_len; i++) {
		if (!fu_parade_lspcon_write_register (self,
						      REG_ADDR_WR_FIFO,
						      command[i],
						      error))
			return FALSE;
	}

	/* set command length */
	if (!fu_parade_lspcon_write_register (self,
					      REG_ADDR_SPI_LEN,
					      command_len - 1,
					      error))
		return FALSE;

	/* execute operation */
	return fu_parade_lspcon_write_register (self,
						REG_ADDR_SPI_CTL,
						SPI_CTL_NOREAD | SPI_CTL_TRIGGER,
						error);
}

/*
 * set the flash Write Enable Latch, permitting the next program, erase or
 * status register write operation.
 */
static gboolean
fu_parade_lspcon_flash_enable_write (FuParadeLspconDevice *self, GError **error)
{
	const guint8 write_enable[] = { 0x06 };
	return fu_parade_lspcon_flash_transmit_command (self,
							write_enable,
							sizeof(write_enable),
							error);
}

static gboolean
fu_parade_lspcon_flash_read_status (FuParadeLspconDevice *self,
				    guint8 *value,
				    GError **error)
{
	if (!fu_parade_lspcon_write_register (self, REG_ADDR_WR_FIFO, 0x05, error))
		return FALSE;
	if (!fu_parade_lspcon_write_register (self, REG_ADDR_SPI_LEN, 0, error))
		return FALSE;
	if (!fu_parade_lspcon_write_register (self, REG_ADDR_SPI_CTL, SPI_CTL_TRIGGER, error))
		return FALSE;

	/* wait for command completion */
	if (!fu_parade_lspcon_poll_register (self, REG_ADDR_SPI_CTL, SPI_CTL_TRIGGER, 0, error))
		return FALSE;

	/* read SR value */
	return fu_parade_lspcon_read_register (self, REG_ADDR_RD_FIFO, value, error);
}

/* poll the flash status register for operation completion */
static gboolean
fu_parade_lspcon_flash_wait_ready (FuParadeLspconDevice *self, GError **error)
{
	g_autoptr(GTimer) timer = g_timer_new ();

	do {
		guint8 status_register;
		if (!fu_parade_lspcon_flash_read_status (self, &status_register, error))
			return FALSE;

		/* BUSY bit clears on completion */
		if ((status_register & 1) == 0)
			return TRUE;

		/* flash operations generally take between 1ms and 4s; polling
		 * at 1000 Hz is still quite responsive and not overly slow */
		g_usleep (G_TIME_SPAN_MILLISECOND);
	} while (g_timer_elapsed (timer, NULL) <= 10.0);

	g_set_error_literal (error,
			     G_IO_ERROR,
			     G_IO_ERROR_TIMED_OUT,
			     "flash did not become ready within 10 seconds");
	return FALSE;
}

static gboolean
fu_parade_lspcon_flash_write (FuParadeLspconDevice *self,
	     guint32 base_address,
	     GBytes *data,
	     GError **error)
{
	FuDevice *device = FU_DEVICE (self);
	FuI2cDevice *i2c_device = FU_I2C_DEVICE (self);
	const guint8 unlock_writes[] = { 0xaa, 0x55, 0x50, 0x41, 0x52, 0x44 };
	gsize data_len = g_bytes_get_size (data);
	g_autoptr(GPtrArray) chunks = NULL;

	/* address must be 256-byte aligned */
	g_return_val_if_fail ((base_address & 0xFF) == 0, FALSE);
	g_debug ("flash write %" G_GSIZE_FORMAT " bytes at %#x",
		 g_bytes_get_size (data), base_address);

	/* unlock map writes by writing the magic sequence */
	for (gsize i = 0; i < sizeof(unlock_writes); i++) {
		if (!fu_parade_lspcon_write_register (self,
						      REG_ADDR_MAP_WRITE,
						      unlock_writes[i],
						      error))
			return FALSE;
	}

	/* reset clt2SPI, required before write */
	if (!fu_parade_lspcon_write_register (self, REG_ADDR_CLT2SPI, 0x20, error))
		return FALSE;
	g_usleep (100 * G_TIME_SPAN_MILLISECOND);
	if (!fu_parade_lspcon_write_register (self, REG_ADDR_CLT2SPI, 0, error))
		return FALSE;

	chunks = fu_chunk_array_new_from_bytes (data, base_address, 0, 256);
	for (gsize i = 0; i < chunks->len; i++) {
		FuChunk *chunk = g_ptr_array_index (chunks, i);
		guint32 address = fu_chunk_get_address (chunk);
		guint32 chunk_size = fu_chunk_get_data_sz (chunk);
		guint8 write_data[257] = { 0x0 };
		g_autoptr(FuParadeLspconI2cAddressGuard) guard = NULL;

		/* map target address range in page 7 */
		if (!fu_parade_lspcon_map_page (self, address, error))
			return FALSE;

		/* write data to page 7 memory window */
		guard = fu_parade_lspcon_i2c_address_guard_new (self,
								I2C_ADDR_PAGE7,
								error);
		/* page write is prefixed with an offset:
		 * we always start from offset 0 */
		write_data[0] = 0;
		memcpy(write_data + 1, fu_chunk_get_data (chunk), chunk_size);

		if (!fu_i2c_device_write_full (i2c_device,
					       write_data,
					       chunk_size + 1,
					       error))
			return FALSE;

		fu_device_set_progress_full (device, address - base_address, data_len);
	}

	/* re-lock map writes */
	return fu_parade_lspcon_write_register (self, REG_ADDR_MAP_WRITE, 0, error);
}

static gboolean
fu_parade_lspcon_flash_erase_block (FuParadeLspconDevice *self,
				    guint32 base_address,
				    guint32 size,
				    GError **error)
{
	const guint8 block_erase[] = { 0xd8,
				       base_address >> 16,
				       base_address >> 8,
				       base_address };

	/* address must be block-aligned */
	g_return_val_if_fail ((base_address & (FLASH_BLOCK_SIZE - 1)) == 0, FALSE);
	g_return_val_if_fail (size == FLASH_BLOCK_SIZE, FALSE);

	g_debug ("flash erase block at %#x", base_address);

	if (!fu_parade_lspcon_flash_enable_write (self, error))
		return FALSE;

	if (!fu_parade_lspcon_flash_transmit_command (self,
						      block_erase,
						      sizeof(block_erase),
						      error))
		return FALSE;

	/* wait for command completion */
	if (!fu_parade_lspcon_poll_register (self,
					     REG_ADDR_SPI_STATUS,
					     SPI_STATUS_SE_MASK,
					     0,
					     error))
		return FALSE;

	/* wait for flash to complete erase */
	return fu_parade_lspcon_flash_wait_ready (self, error);
}

static gboolean
fu_parade_lspcon_probe_active_flash_partition (FuParadeLspconDevice *self,
					       guint8 *partition,
					       GError **error)
{
	guint8 data = 0x0;
	g_autoptr(FuParadeLspconI2cAddressGuard) guard = NULL;

	/* read currently-running flash partition number */
	guard = fu_parade_lspcon_i2c_address_guard_new (self, I2C_ADDR_PAGE5, error);
	if (guard == NULL)
		return FALSE;
	if (!fu_parade_lspcon_read_register (self,
					     REG_ADDR_ACTIVE_PARTITION,
					     &data,
					     error))
		return FALSE;

	*partition = data;
	return TRUE;
}

static gboolean
fu_parade_lspcon_device_reload (FuDevice *device, GError **error)
{
	FuParadeLspconDevice *self = FU_PARADE_LSPCON_DEVICE (device);
	guint32 oui;
	guint8 version_buf[2] = { 0x0 };
	g_autofree gchar *version = NULL;
	g_autoptr(FuDeviceLocker) aux_device_locker = NULL;
	g_autoptr(FuUdevDevice) aux_device = NULL;
	g_autoptr(GList) aux_devices = NULL;
	g_autoptr(GUdevClient) udev_client = g_udev_client_new (NULL);
	g_autoptr(GUdevEnumerator) enumerator = g_udev_enumerator_new (udev_client);

	/* determine active partition for flashing later */
	if (!fu_parade_lspcon_probe_active_flash_partition (self, &self->active_partition, error))
		return FALSE;
	g_debug ("device reports running from partition %d", self->active_partition);
	if (self->active_partition < 1 || self->active_partition > 3) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "unexpected active flash partition: %d",
			     self->active_partition);
		return FALSE;
	}

	/* find the drm_dp_aux_dev specified by quirks that is connected to the
	 * LSPCON, in order to read DPCD from it */
	if (self->aux_device_name == NULL) {
		g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
				     "no DP aux device specified, unable to query LSPCON");
		return FALSE;
	}
	g_udev_enumerator_add_match_subsystem (enumerator, "drm_dp_aux_dev");
	g_udev_enumerator_add_match_sysfs_attr (enumerator, "name", self->aux_device_name);
	aux_devices = g_udev_enumerator_execute (enumerator);
	if (aux_devices == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to locate a DP aux device named \"%s\"",
			     self->aux_device_name);
		return FALSE;
	}
	if (g_list_length (aux_devices) > 1) {
		g_list_free_full (aux_devices, g_object_unref);
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "found multiple DP aux devices with name \"%s\"",
			     self->aux_device_name);
		return FALSE;
	}
	aux_device = fu_udev_device_new (g_steal_pointer (&aux_devices->data));
	g_debug ("using aux dev %s", fu_udev_device_get_sysfs_path (aux_device));

	/* the following open() requires the device have IDs set */
	if (!fu_udev_device_set_physical_id (aux_device, "drm_dp_aux_dev", error))
		return FALSE;

	/* open device to read version from DPCD */
	if ((aux_device_locker = fu_device_locker_new (aux_device, error)) == NULL)
		return FALSE;

	/* DPCD address 00500-00502: device OUI */
	if (!fu_udev_device_pread_full (aux_device, 0x500, (guint8 *) &oui, 3, error))
		return FALSE;
	oui = GUINT32_FROM_BE(oui) >> 8;
	if (oui != 0x001CF8) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "device OUI %06X does not match expected value for Paradetech",
			     oui);
		return FALSE;
	}

	/* DPCD address 0x50A, 0x50B: branch device firmware
	 * major and minor revision */
	if (!fu_udev_device_pread_full (aux_device, 0x50a,
					version_buf, sizeof(version_buf),
					error))
		return FALSE;
	version = g_strdup_printf ("%d.%d", version_buf[0], version_buf[1]);
	fu_device_set_version (device, version);
	return TRUE;
}

static gboolean
fu_parade_lspcon_device_write_firmware (FuDevice *device,
					FuFirmware *firmware,
					FwupdInstallFlags flags,
					GError **error)
{
	FuParadeLspconDevice *self = FU_PARADE_LSPCON_DEVICE (device);
	const guint8 write_sr_volatile[] = { 0x50 };
	const guint8 write_sr_disable_bp[] = {
		0x01,	/* write SR */
		0x80,	/* write protect follows /WP signal, no block protection */
		0x00
	};
	const guint8 write_sr_enable_bp[] = { 0x01, 0x8c, 0x00 };
	/* if the boot partition is active we could flash either, but prefer
	 * the first */
	const guint8 target_partition = self->active_partition == 1 ? 2 : 1;
	const guint32 target_address = target_partition << 16;
	const guint8 flag_data[] = { 0x55, 0xaa, target_partition, 1 - target_partition };
	const guint8 *buf;
	gsize bufsz;
	g_autofree guint8 *readback_buf = NULL;
	g_autoptr(GBytes) blob_fw = NULL;
	g_autoptr(GBytes) flag_data_bytes = NULL;

	blob_fw = fu_firmware_get_bytes (firmware, error);
	if (blob_fw == NULL)
		return FALSE;

	buf = g_bytes_get_data (blob_fw, &bufsz);
	if (bufsz != FLASH_BLOCK_SIZE) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "invalid image size %#" G_GSIZE_MODIFIER "x, expected %#x",
			     bufsz, (unsigned) FLASH_BLOCK_SIZE);
		return FALSE;
	}

	/* deassert flash /WP */
	if (!fu_parade_lspcon_write_register (self,
					      REG_ADDR_WR_PROTECT,
					      WR_PROTECT_DISABLE,
					      error))
		return FALSE;

	/* disable flash protection until next power-off */
	if (!fu_parade_lspcon_flash_transmit_command (self,
						      write_sr_volatile,
						      sizeof(write_sr_volatile),
						      error))
		return FALSE;
	if (!fu_parade_lspcon_flash_transmit_command (self,
						      write_sr_disable_bp,
						      sizeof(write_sr_disable_bp),
						      error))
		return FALSE;

	/* wait for SR write to complete */
	if (!fu_parade_lspcon_flash_wait_ready (self, error))
		return FALSE;

	/* erase entire target partition (one flash block) */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_ERASE);
	if (!fu_parade_lspcon_flash_erase_block (self, target_address, bufsz, error)) {
		g_prefix_error (error, "failed to erase flash partition %d: ",
				target_partition);
		return FALSE;
	}

	/* write image */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	if (!fu_parade_lspcon_flash_write (self, target_address, blob_fw, error)) {
		g_prefix_error (error, "failed to write firmware to partition %d: ",
				target_partition);
		return FALSE;
	}

	/* read back written image to verify */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_VERIFY);
	readback_buf = g_malloc0 (bufsz);
	if (!fu_parade_lspcon_flash_read (self, target_address,
					  readback_buf, bufsz,
					  error))
		return FALSE;
	if (!fu_common_bytes_compare_raw (buf, bufsz,
					  readback_buf, bufsz,
					  error)) {
		g_prefix_error (error, "flash contents do not match: ");
		return FALSE;
	}

	/* erase flag partition */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_ERASE);
	if (!fu_parade_lspcon_flash_erase_block (self, 0, FLASH_BLOCK_SIZE, error))
		return FALSE;

	/* write flag indicating device should boot the target partition */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	flag_data_bytes = g_bytes_new_static (flag_data, sizeof(flag_data));
	if (!fu_parade_lspcon_flash_write (self, 0, flag_data_bytes, error))
		return FALSE;

	/* verify flag partition */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_VERIFY);
	if (!fu_parade_lspcon_flash_read (self, 0,
					  readback_buf, sizeof(flag_data),
					  error))
		return FALSE;
	if (!fu_common_bytes_compare_raw (flag_data, sizeof(flag_data),
					  readback_buf, bufsz,
					  error)) {
		g_prefix_error (error, "flag partition contents do not match: ");
		return FALSE;
	}

	/* re-enable flash protection */
	if (!fu_parade_lspcon_flash_transmit_command (self,
						      write_sr_volatile,
						      sizeof(write_sr_volatile),
						      error))
		return FALSE;
	if (!fu_parade_lspcon_flash_transmit_command (self,
						      write_sr_enable_bp,
						      sizeof(write_sr_enable_bp),
						      error))
		return FALSE;

	/* reassert /WP to flash */
	return fu_parade_lspcon_write_register (self, REG_ADDR_WR_PROTECT, 0, error);
}

static gboolean
fu_parade_lspcon_set_mpu_running (FuParadeLspconDevice *self,
				  gboolean running,
				  GError **error)
{
	/* reset */
	if (!fu_parade_lspcon_write_register (self, REG_ADDR_MPU, 0xc0, error))
		return FALSE;

	/* release reset, set MPU active or not */
	return fu_parade_lspcon_write_register (self, REG_ADDR_MPU,
						running ? 0 : 0x40, error);
}

static gboolean
fu_parade_lspcon_device_detach (FuDevice *device, GError **error)
{
	FuParadeLspconDevice *self = FU_PARADE_LSPCON_DEVICE (device);
	return fu_parade_lspcon_set_mpu_running (self, FALSE, error);
}

static gboolean
fu_parade_lspcon_device_attach (FuDevice *device, GError **error)
{
	FuParadeLspconDevice *self = FU_PARADE_LSPCON_DEVICE (device);
	return fu_parade_lspcon_set_mpu_running (self, TRUE, error);
}

static GBytes *
fu_parade_lspcon_device_dump_firmware (FuDevice *device, GError **error)
{
	FuParadeLspconDevice *self = FU_PARADE_LSPCON_DEVICE (device);
	g_autofree guint8 *data = g_malloc0 (FLASH_BLOCK_SIZE);

	if (!fu_parade_lspcon_flash_read (self,
					  self->active_partition * FLASH_BLOCK_SIZE,
					  data, FLASH_BLOCK_SIZE, error))
		return NULL;
	return g_bytes_new_take (g_steal_pointer (&data), FLASH_BLOCK_SIZE);
}

static void
fu_parade_lspcon_device_class_init (FuParadeLspconDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	GObjectClass *klass_object = G_OBJECT_CLASS (klass);

	klass_object->finalize = fu_parade_lspcon_device_finalize;
	klass_device->set_quirk_kv = fu_parade_lspcon_device_set_quirk_kv;
	klass_device->probe = fu_parade_lspcon_device_probe;
	klass_device->setup = fu_parade_lspcon_device_reload;
	klass_device->open = fu_parade_lspcon_device_open;
	klass_device->reload = fu_parade_lspcon_device_reload;
	klass_device->detach = fu_parade_lspcon_device_detach;
	klass_device->write_firmware = fu_parade_lspcon_device_write_firmware;
	klass_device->attach = fu_parade_lspcon_device_attach;
	klass_device->dump_firmware = fu_parade_lspcon_device_dump_firmware;
}
