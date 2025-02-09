/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>

#include <lvgl.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/nus.h>

#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>

#include <ff.h>

#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT "/" DISK_DRIVE_NAME ":"
#define IMG_FILE_PATH "/" DISK_DRIVE_NAME ":/51.png"

#define FS_RET_OK FR_OK

#define MAX_PATH 128
#define SOME_FILE_NAME "some.dat"
#define SOME_DIR_NAME "some"
#define SOME_REQUIRED_LEN MAX(sizeof(SOME_FILE_NAME), sizeof(SOME_DIR_NAME))

static int lsdir(const char *path);

static const char *disk_mount_pt = DISK_MOUNT_PT;

static FATFS fat_fs;
/* mounting info */
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
};

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

lv_obj_t *text_label;

static const struct gpio_dt_spec sw0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

const struct device *display_dev;

static struct bt_conn *current_conn;

static struct k_work advertise_work;

void parse_data(uint8_t *data, uint16_t len);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static void notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);

	printk("%s() - %s\n", __func__, (enabled ? "Enabled" : "Disabled"));

	if (enabled)
	{
		lv_label_set_text(text_label, "BLE Notifications Enabled");
		lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, 0, 0);
		gpio_pin_set_dt(&led2, 1);
	}
	else
	{
		lv_label_set_text(text_label, "BLE Notifications Disabled");
		lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, 0, 0);
		gpio_pin_set_dt(&led2, 0);
	}
}

static void received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);

	char addr[BT_ADDR_LE_STR_LEN] = {0};

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, ARRAY_SIZE(addr));

	LOG_INF("Received data from: %s", addr);

	parse_data((uint8_t *)data, len);
}

struct bt_nus_cb nus_listener = {
	.notif_enabled = notif_enabled,
	.received = received,
};

static void advertise(struct k_work *work)
{
	int err;

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (rc %d)", err);
		return;
	}

	LOG_INF("Advertising successfully started");
}


static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err)
	{
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		k_work_submit(&advertise_work);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected %s", addr);

	current_conn = bt_conn_ref(conn);

	lv_label_set_text(text_label, "BLE Connected");
	lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, 0, 0);

	gpio_pin_set_dt(&led1, 1);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s, reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));

	if (current_conn)
	{
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	lv_label_set_text(text_label, "BLE Disconnected");
	lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, 0, 0);

	gpio_pin_set_dt(&led1, 0);
	gpio_pin_set_dt(&led2, 0);
}

static void on_conn_recycled(void)
{
	k_work_submit(&advertise_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = on_conn_recycled,
};

static void bt_ready(int err)
{
	if (err != 0) {
		LOG_ERR("Bluetooth failed to initialise: %d", err);
	} else {
		k_work_submit(&advertise_work);
	}
}

void system_off(void)
{
	int err;

	LOG_INF("System off");

	err = pm_device_action_run(display_dev, PM_DEVICE_ACTION_SUSPEND);
	if (err < 0)
	{
		printf("Could not suspend display (%d)\n", err);
	}

	gpio_pin_set_dt(&led0, 0);
	gpio_pin_set_dt(&led1, 0);
	gpio_pin_set_dt(&led2, 0);

	sys_poweroff();
}

// prase received data
void parse_data(uint8_t *data, uint16_t len)
{
	// command "OFF"

	if (len == 3 && data[0] == 'O' && data[1] == 'F' && data[2] == 'F')
	{
		LOG_INF("Received OFF command");

		system_off();
	}
}

/* List dir entry by path
 *
 * @param path Absolute path to list
 *
 * @return Negative errno code on error, number of listed entries on
 *         success.
 */
static int lsdir(const char *path)
{
	int res;
	struct fs_dir_t dirp;
	static struct fs_dirent entry;
	int count = 0;

	fs_dir_t_init(&dirp);

	/* Verify fs_opendir() */
	res = fs_opendir(&dirp, path);
	if (res)
	{
		printk("Error opening dir %s [%d]\n", path, res);
		return res;
	}

	printk("\nListing dir %s ...\n", path);
	for (;;)
	{
		/* Verify fs_readdir() */
		res = fs_readdir(&dirp, &entry);

		/* entry.name[0] == 0 means end-of-dir */
		if (res || entry.name[0] == 0)
		{
			break;
		}

		if (entry.type == FS_DIR_ENTRY_DIR)
		{
			printk("[DIR ] %s\n", entry.name);
		}
		else
		{
			printk("[FILE] %s (size = %zu)\n",
				   entry.name, entry.size);
		}
		count++;
	}

	/* Verify fs_closedir() */
	fs_closedir(&dirp);
	if (res == 0)
	{
		res = count;
	}

	return res;
}

static int init_sd_card(void)
{
	static const char *disk_pdrv = "SD";
	uint64_t memory_size_mb;
	uint32_t block_count;
	uint32_t block_size;

	int err;

	if (disk_access_ioctl(disk_pdrv,
						  DISK_IOCTL_CTRL_INIT, NULL) != 0)
	{
		LOG_ERR("Storage init ERROR!");
		return -1;
	}

	if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_COUNT, &block_count))
	{
		printk("Unable to get sector count");
		return -1;
	}

	if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_SIZE, &block_size))
	{
		printk("Unable to get sector size");
		return -1;
	}

	memory_size_mb = (uint64_t)block_count * block_size / (1024 * 1024);
	printk("Memory Size(MB): %u\n", (uint32_t)memory_size_mb);

	if (disk_access_ioctl(disk_pdrv,
						  DISK_IOCTL_CTRL_DEINIT, NULL) != 0)
	{
		LOG_ERR("Storage deinit ERROR!");
		return -1;
	}

	mp.mnt_point = disk_mount_pt;

	err = fs_mount(&mp);
	if (err)
	{
		printk("Error mounting fat_fs [%d]\n", err);
		return err;
	}

	if (lsdir(disk_mount_pt) == 0)
	{
	}

	return 0;
}

int main(void)
{
	int ret;

	printk("Zephyr Example Application %s\n", APP_VERSION_STRING);

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev))
	{
		LOG_ERR("Device not ready, aborting test");
		return 0;
	}

	if (init_sd_card() != 0)
	{
		LOG_ERR("Failed to initialize SD card\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&sw0, GPIO_INPUT);
	if (ret < 0)
	{
		LOG_ERR("Could not configure sw0 GPIO (%d)\n", ret);
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&sw0, GPIO_INT_LEVEL_ACTIVE);
	if (ret < 0)
	{
		LOG_ERR("Could not configure sw0 GPIO interrupt (%d)\n", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT);
	if (ret < 0)
	{
		LOG_ERR("Could not configure led0 GPIO (%d)\n", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&led1, GPIO_OUTPUT);
	if (ret < 0)
	{
		LOG_ERR("Could not configure led1 GPIO (%d)\n", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&led2, GPIO_OUTPUT);
	if (ret < 0)
	{
		LOG_ERR("Could not configure led2 GPIO (%d)\n", ret);
		return 0;
	}

	gpio_pin_set_dt(&led0, 0);
	gpio_pin_set_dt(&led1, 0);
	gpio_pin_set_dt(&led2, 0);

	lv_obj_t *img = lv_img_create(lv_scr_act());

	lv_img_set_src(img, IMG_FILE_PATH);
	lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

	text_label = lv_label_create(lv_scr_act());
	lv_label_set_text(text_label, "Bluetooth UART example");
	lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, 0, 0);

	lv_timer_handler();
	display_blanking_off(display_dev);

	ret = bt_nus_cb_register(&nus_listener, NULL);
	if (ret)
	{
		printk("Failed to register NUS callback: %d\n", ret);
		return ret;
	}

	k_work_init(&advertise_work, advertise);

	ret = bt_enable(bt_ready);
	if (ret)
	{
		LOG_ERR("Bluetooth init failed (err %d)", ret);
		return 0;
	}

	LOG_INF("Bluetooth initialized");

	while (1)
	{	
		gpio_pin_toggle_dt(&led0);

		lv_timer_handler();
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
