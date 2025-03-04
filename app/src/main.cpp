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

#include "MAX30101.hpp"

#include "arm_math.h"

bool is_use_display = false;
bool is_use_ble = false;
bool is_use_sd = false;
bool is_use_ppg = true;
bool is_use_acc = true;

uint8_t ledBrightnessRed = 0;	// Options: 0=Off to 255=50mA
uint8_t ledBrightnessIR = 0;	// Options: 0=Off to 255=50mA
uint8_t ledBrightnessGreen = 0; // Options: 0=Off to 255=50mA

#define PPG_STACK_SIZE 1024
#define PPG_PRIORITY 5

#define ACC_STACK_SIZE 1024
#define ACC_PRIORITY 5

#define FIFO_SAMPLES 32 // MAX30101 FIFO depth
static K_SEM_DEFINE(data_sem, 0, 1);
static struct sensor_value acc_data[3]; // Shared accelerometer data
static bool new_acc_data = false;

extern void ppg_entry_point(void *, void *, void *);

K_THREAD_DEFINE(ppg_tid, ACC_STACK_SIZE,
				ppg_entry_point, NULL, NULL, NULL,
				ACC_PRIORITY, 0, 0);

extern void acc_entry_point(void *, void *, void *);

K_THREAD_DEFINE(acc_tid, PPG_STACK_SIZE,
				acc_entry_point, NULL, NULL, NULL,
				PPG_PRIORITY, 0, 0);

#define IIR_ORDER 2
#define IIR_NUMSTAGES (IIR_ORDER / 2)

static float32_t m_biquad_red_state[IIR_ORDER];
static float32_t m_biquad_ir_state[IIR_ORDER];
static float32_t m_biquad_green_state[IIR_ORDER];
static float32_t m_biquad_coeffs[5 * IIR_NUMSTAGES] =
	{
		0.274727,
		0.549454,
		0.274727,
		0.073624,
	   -0.172531};

arm_biquad_cascade_df2T_instance_f32 const red_iir_inst =
	{
		IIR_ORDER / 2,
		m_biquad_red_state,
		m_biquad_coeffs};

arm_biquad_cascade_df2T_instance_f32 const ir_iir_inst =
	{
		IIR_ORDER / 2,
		m_biquad_ir_state,
		m_biquad_coeffs};

arm_biquad_cascade_df2T_instance_f32 const green_iir_inst =
	{
		IIR_ORDER / 2,
		m_biquad_green_state,
		m_biquad_coeffs};

MAX30101 ppg = MAX30101();

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
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

const struct device *display_dev, *max30101_dev, *adxl_dev;

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
	if (err)
	{
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
	if (err != 0)
	{
		LOG_ERR("Bluetooth failed to initialise: %d", err);
	}
	else
	{
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

	if (is_use_display)
	{

		display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
		if (!device_is_ready(display_dev))
		{
			LOG_ERR("Device not ready, aborting test");
			return 0;
		}

		lv_obj_t *img = lv_img_create(lv_scr_act());

		lv_img_set_src(img, IMG_FILE_PATH);
		lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

		text_label = lv_label_create(lv_scr_act());
		lv_label_set_text(text_label, "Bluetooth UART example");
		lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, 0, 0);

		lv_timer_handler();
		display_blanking_off(display_dev);
	}

	if (is_use_ble)
	{
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
	}

	if (is_use_sd)
	{
		if (init_sd_card() != 0)
		{
			LOG_ERR("Failed to initialize SD card\n");
			return 0;
		}
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

	while (1)
	{
		gpio_pin_toggle_dt(&led0);
		lv_timer_handler();
		k_sleep(K_MSEC(1000));
	}

	return 0;
}

void calbrate_ppg(void)
{
	LOG_INF("Calibrating PPG sensor...");
	// Initial setup with low brightness values
	uint8_t templedBrightnessRed = 0;
	uint8_t templedBrightnessIR = 0;
	uint8_t templedBrightnessGreen = 0;

	uint8_t sampleAverage = 1; // Use 1 for faster response during calibration
	uint8_t ledMode = 3;	   // All LEDs
	int sampleRate = 1600;	   // 100Hz
	int pulseWidth = 215;
	int adcRange = 16384;

	const uint32_t TARGET_DC = 262144 / 2; // Target DC level
	const uint32_t TOLERANCE = 4096;	   // Tolerance range
	bool is_calibrating = true;
	int stable_count = 0;

	ppg.setup(templedBrightnessRed, templedBrightnessIR, templedBrightnessGreen,
			  sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);

	while (is_calibrating)
	{
		ppg.check();

		while (ppg.available())
		{
			uint32_t red = ppg.getFIFORed();
			uint32_t ir = ppg.getFIFOIR();
			uint32_t green = ppg.getFIFOGreen();

			// Adjust RED LED
			if (red > TARGET_DC + TOLERANCE)
			{
				templedBrightnessRed = MAX(0, templedBrightnessRed - 1);
			}
			else if (red < TARGET_DC - TOLERANCE)
			{
				templedBrightnessRed = MIN(255, templedBrightnessRed + 1);
			}

			// Adjust IR LED
			if (ir > TARGET_DC + TOLERANCE)
			{
				templedBrightnessIR = MAX(0, templedBrightnessIR - 1);
			}
			else if (ir < TARGET_DC - TOLERANCE)
			{
				templedBrightnessIR = MIN(255, templedBrightnessIR + 1);
			}

			// // Adjust GREEN LED
			// if (green > TARGET_DC + TOLERANCE) {
			//     templedBrightnessGreen = MAX(0, templedBrightnessGreen - 1);
			// } else if (green < TARGET_DC - TOLERANCE) {
			//     templedBrightnessGreen = MIN(255, templedBrightnessGreen + 1);
			// }

			templedBrightnessGreen = 255;
			// Update LED brightness
			ppg.setPulseAmplitudeRed(templedBrightnessRed);
			ppg.setPulseAmplitudeIR(templedBrightnessIR);
			ppg.setPulseAmplitudeGreen(templedBrightnessGreen);

			// Print current values
			printk("R:%d(%d),IR:%d(%d),G:%d(%d)\n",
				   templedBrightnessRed, red,
				   templedBrightnessIR, ir,
				   templedBrightnessGreen, green);

			// Check if all LEDs are within tolerance
			if (abs(red - TARGET_DC) < TOLERANCE &&
				abs(ir - TARGET_DC) < TOLERANCE)
			{
				is_calibrating = false;
				ledBrightnessRed = templedBrightnessRed;
				ledBrightnessIR = templedBrightnessIR;
				ledBrightnessGreen = templedBrightnessGreen;
				break;
			}
			ppg.nextSample();
		}
		// k_sleep(K_MSEC(10));  // Prevent tight loop
	}

	LOG_INF("Calibration complete - R:%d, IR:%d, G:%d\n",
			ledBrightnessRed, ledBrightnessIR, ledBrightnessGreen);
}

void ppg_entry_point(void *a, void *b, void *c)
{
	max30101_dev = DEVICE_DT_GET_ANY(maxim_max30101);

	if (!device_is_ready(max30101_dev))
	{
		LOG_ERR("max30101 device is not ready\n");
	}

	if (!ppg.begin(max30101_dev))
	{
		LOG_ERR("Could not begin PPG device...");
	}

	// Setup to sense up to 18 inches, max LED brightness

	calbrate_ppg();

	uint8_t sampleAverage = 2; // Options: 1, 2, 4, 8, 16, 32
	uint8_t ledMode = 3;	   // Options: 1 = Red only, 2 = Red + IR, 3 = Red + IR + Green
	int sampleRate = 100;	   // Options: 50, 100, 200, 400, 800, 1000, 1600, 3200
	int pulseWidth = 215;	   // Options: 69, 118, 215, 411
	int adcRange = 16384;	   // Options: 2048, 4096, 8192, 16384

	ppg.setup(ledBrightnessRed, ledBrightnessIR, ledBrightnessGreen, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);

	uint32_t red;
	uint32_t ir;
	uint32_t green;
	uint32_t samplesTaken = 0;
	float sampleRateInHz;
	int sampleingRateTarget = (sampleRate / sampleAverage) + 1;
	uint32_t strat_time = k_uptime_get_32();

	while (1)
	{
		ppg.check(); // Check the sensor, read up to 3 samples

		while (ppg.available()) // do we have new data?
		{
			samplesTaken++;

			// Get raw values and convert to float
			float32_t raw_red = (float32_t)ppg.getFIFORed();
			float32_t raw_ir = (float32_t)ppg.getFIFOIR();
			float32_t raw_green = (float32_t)ppg.getFIFOGreen();

			sampleRateInHz = samplesTaken / ((k_uptime_get_32() - strat_time) / 1000.0);

			if (samplesTaken % sampleingRateTarget == 0)
			{
				samplesTaken = 0;
			}

			// Apply filters (in-place processing)
			float32_t filtered_red = raw_red;
			float32_t filtered_ir = raw_ir;
			float32_t filtered_green = raw_green;

			arm_biquad_cascade_df2T_f32(&red_iir_inst, &raw_red, &filtered_red, 1);
			arm_biquad_cascade_df2T_f32(&ir_iir_inst, &raw_ir, &filtered_ir, 1);
			arm_biquad_cascade_df2T_f32(&green_iir_inst, &raw_green, &filtered_green, 1);

			// Print PPG data only if accelerometer data not ready
			printk("C:%d,R:%.1f,IR:%.1f,G:%.1f\n",
				   samplesTaken, filtered_red, filtered_ir, filtered_green);

			ppg.nextSample(); // We're finished with this sample so move to next sample

			// Signal accelerometer to read data
			k_sem_give(&data_sem);

			k_yield();
		}
	}
}

void acc_entry_point(void *a, void *b, void *c)
{
	struct sensor_value accel[3];

	double x, y, z;

	struct sensor_value odr_attr, fs_attr;

	adxl_dev = DEVICE_DT_GET_ANY(st_lis2dw12);

	if (!device_is_ready(adxl_dev))
	{
		LOG_ERR("adxl device is not ready\n");
	}

	while (1)
	{
		// Wait for PPG data ready signal
		if (k_sem_take(&data_sem, K_MSEC(10)) == 0)
		{
			// Read the acceleration data
			if (sensor_sample_fetch_chan(adxl_dev, SENSOR_CHAN_ACCEL_XYZ) == 0)
			{
				sensor_channel_get(adxl_dev, SENSOR_CHAN_ACCEL_XYZ, acc_data);
				// Get accelerometer values
				x = sensor_value_to_double(&acc_data[0]);
				y = sensor_value_to_double(&acc_data[1]);
				z = sensor_value_to_double(&acc_data[2]);

				// Print synchronized data
				// printk("X:%f,Y:%f,Z:%f\n", x, y, z);
				printk("X:127,Y:127,Z:127\n");
			}
		}
	}
}
