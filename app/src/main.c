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

#include <bluetooth/services/nus.h>

#include <app/drivers/blink.h>

#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define BLINK_PERIOD_MS_STEP 100U
#define BLINK_PERIOD_MS_MAX  1000U

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN	(sizeof(DEVICE_NAME) - 1)

lv_obj_t *text_label;

static K_SEM_DEFINE(ble_init_ok, 0, 1);

static struct bt_conn *current_conn;
static uint8_t conn_state = BT_NUS_SEND_STATUS_DISABLED;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected %s", addr);

	current_conn = bt_conn_ref(conn);

	lv_label_set_text(text_label, "BLE Connected");
	lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 0);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s, reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	lv_label_set_text(text_label, "BLE Disconnected");
	lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 0);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
};

static void send_enabled(enum bt_nus_send_status status)
{
	conn_state = status;

	LOG_INF("Notifications %sabled", (status == BT_NUS_SEND_STATUS_ENABLED) ? "en" : "dis");

	if (status == BT_NUS_SEND_STATUS_ENABLED)
	{
		lv_label_set_text(text_label, "BLE Notifications Enabled");
		lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 0);
	}
	else
	{
		lv_label_set_text(text_label, "BLE Notifications Disabled");
		lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 0);
	}
}

// prase received data
void parse_data(const uint8_t *const data, uint16_t len)
{
	// command "POWER OFF"

	if (len == 9)
	{
		if (data[0] == 'P' && data[1] == 'O' && data[2] == 'W' && data[3] == 'E' && data[4] == 'R' && data[5] == ' ' && data[6] == 'O' && data[7] == 'F' && data[8] == 'F')
		{
			LOG_INF("Received command: POWER OFF");			
		}
	}

}

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data,
			  uint16_t len)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN] = {0};

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, ARRAY_SIZE(addr));

	LOG_INF("Received data from: %s", addr);

	parse_data(data, len);
}

static struct bt_nus_cb nus_cb = {
	.send_enabled = send_enabled,
	.received = bt_receive_cb,
};

int main(void)
{
	int ret;
	int err = 0;
	unsigned int period_ms = BLINK_PERIOD_MS_MAX;
	const struct device *sensor, *blink, *display_dev;
	struct sensor_value last_val = { 0 }, val;
	

	printk("Zephyr Example Application %s\n", APP_VERSION_STRING);
	
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Device not ready, aborting test");
		return 0;
	}

	sensor = DEVICE_DT_GET(DT_NODELABEL(example_sensor));
	if (!device_is_ready(sensor)) {
		LOG_ERR("Sensor not ready");
		return 0;
	}

	blink = DEVICE_DT_GET(DT_NODELABEL(blink_led));
	if (!device_is_ready(blink)) {
		LOG_ERR("Blink LED not ready");
		return 0;
	}

	ret = blink_off(blink);
	if (ret < 0) {
		LOG_ERR("Could not turn off LED (%d)", ret);
		return 0;
	}

	printk("Use the sensor to change LED blinking period\n");


	text_label = lv_label_create(lv_scr_act());
	lv_label_set_text(text_label, "Bluetooth UART example");
	lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 0);

	lv_timer_handler();
	display_blanking_off(display_dev);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}

	LOG_INF("Bluetooth initialized");

	k_sem_give(&ble_init_ok);

	LOG_INF("Bluetooth initialized");

	k_sem_give(&ble_init_ok);

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("Failed to initialize UART service (err: %d)", err);
		return 0;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd,
			      ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return 0;
	}

	while (1) {
		ret = sensor_sample_fetch(sensor);
		if (ret < 0) {
			LOG_ERR("Could not fetch sample (%d)", ret);
			return 0;
		}

		ret = sensor_channel_get(sensor, SENSOR_CHAN_PROX, &val);
		if (ret < 0) {
			LOG_ERR("Could not get sample (%d)", ret);
			return 0;
		}

		if ((last_val.val1 == 0) && (val.val1 == 1)) {
			if (period_ms == 0U) {
				period_ms = BLINK_PERIOD_MS_MAX;
			} else {
				period_ms -= BLINK_PERIOD_MS_STEP;
			}

			printk("Proximity detected, setting LED period to %u ms\n",
			       period_ms);
			blink_set_period_ms(blink, period_ms);
		}

		last_val = val;

		if (conn_state == BT_NUS_SEND_STATUS_ENABLED)
		{
			LOG_INF("Sending data over BLE connection");
			uint8_t data[100];

			strcpy(data, "Hello World!");

			int len = strlen(data);

			bt_nus_send(NULL, data, len);
		}
		else
		{
			LOG_INF("BLE connection not enabled");
		}

		lv_timer_handler();
		k_sleep(K_MSEC(100));

	}

	return 0;
}

