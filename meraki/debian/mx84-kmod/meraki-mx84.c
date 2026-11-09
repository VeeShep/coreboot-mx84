// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/gpio_keys.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input-event-codes.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_data/leds-lp55xx.h>
#include <linux/platform_device.h>
#include <linux/string.h>

#define MX84_LP5562_ADAPTER	"SMBus I801 adapter"
#define MX84_LP5562_ADDR	0x30
#define MX84_LED_ENABLE_GPIO	35
#define MX84_RESET_GPIO		0

static struct lp55xx_led_config mx84_leds[] = {
	{ .name = "tricolor0:red",   .chan_nr = 0, .led_current = 150, .max_current = 255 },
	{ .name = "tricolor0:green", .chan_nr = 1, .led_current = 150, .max_current = 255 },
	{ .name = "tricolor0:blue",  .chan_nr = 2, .led_current = 150, .max_current = 255 },
	{ .name = "power:white",     .chan_nr = 3, .led_current = 150, .max_current = 225 },
};

static struct lp55xx_platform_data mx84_led_pdata = {
	.led_config = mx84_leds,
	.num_channels = ARRAY_SIZE(mx84_leds),
	.clock_mode = LP55XX_CLOCK_AUTO,
};

static struct i2c_board_info mx84_led_info = {
	I2C_BOARD_INFO("lp5562", MX84_LP5562_ADDR),
	.platform_data = &mx84_led_pdata,
};

static struct gpio_keys_button mx84_reset_button = {
	.code = KEY_RESTART,
	.gpio = -1,
	.active_low = 1,
	.desc = "restore",
	.type = EV_KEY,
};

static const struct gpio_keys_platform_data mx84_reset_pdata = {
	.buttons = &mx84_reset_button,
	.nbuttons = 1,
	.poll_interval = 60,
	.rep = 1,
	.name = "MX84 reset button",
};

static struct gpio_desc *mx84_led_enable;
static struct i2c_client *mx84_led_client;
static struct platform_device *mx84_reset_device;

static int mx84_find_i801_adapter(struct device *dev, void *data)
{
	struct i2c_adapter **found = data;
	struct i2c_adapter *adapter = i2c_verify_adapter(dev);

	if (!adapter || !strstarts(adapter->name, MX84_LP5562_ADAPTER))
		return 0;

	get_device(dev);
	*found = adapter;
	return 1;
}

static const struct dmi_system_id mx84_dmi_table[] __initconst = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Cisco Meraki"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MX84-HW"),
		},
	},
	{}
};

static int __init mx84_board_init(void)
{
	struct gpio_device *gdev;
	struct gpio_desc *enable;
	struct i2c_adapter *adapter;
	struct i2c_client *client;
	struct platform_device *reset;
	struct gpio_chip *chip;

	if (!dmi_check_system(mx84_dmi_table)) {
		pr_err("meraki-mx84: DMI does not match %s %s\n",
		       dmi_get_system_info(DMI_SYS_VENDOR),
		       dmi_get_system_info(DMI_PRODUCT_NAME));
		return -ENODEV;
	}

	gdev = gpio_device_find_by_label("gpio_ich");
	if (!gdev) {
		pr_err("meraki-mx84: gpio_ich not found\n");
		return -ENODEV;
	}

	chip = gpio_device_get_chip(gdev);
	if (chip->base < 0 || MX84_RESET_GPIO >= chip->ngpio) {
		pr_err("meraki-mx84: reset GPIO is outside gpio_ich\n");
		gpio_device_put(gdev);
		return -ENODEV;
	}
	mx84_reset_button.gpio = chip->base + MX84_RESET_GPIO;
	enable = gpiochip_request_own_desc(chip, MX84_LED_ENABLE_GPIO,
					   "master reset signal",
					   GPIO_OPEN_DRAIN, GPIOD_OUT_HIGH);
	gpio_device_put(gdev);
	if (IS_ERR(enable)) {
		pr_err("meraki-mx84: failed to request enable GPIO: %ld\n",
		       PTR_ERR(enable));
		return PTR_ERR(enable);
	}

	mx84_led_pdata.enable_gpiod = enable;
	adapter = NULL;
	i2c_for_each_dev(&adapter, mx84_find_i801_adapter);
	if (!adapter) {
		pr_err("meraki-mx84: I801 adapter not found\n");
		gpiochip_free_own_desc(enable);
		return -ENODEV;
	}

	client = i2c_new_client_device(adapter, &mx84_led_info);
	i2c_put_adapter(adapter);
	if (IS_ERR(client)) {
		pr_err("meraki-mx84: failed to register LP5562: %ld\n",
		       PTR_ERR(client));
		gpiochip_free_own_desc(enable);
		return PTR_ERR(client);
	}

	reset = platform_device_register_data(NULL, "gpio-keys-polled",
					      PLATFORM_DEVID_NONE,
					      &mx84_reset_pdata,
					      sizeof(mx84_reset_pdata));
	if (IS_ERR(reset)) {
		pr_err("meraki-mx84: failed to register reset button: %ld\n",
		       PTR_ERR(reset));
		i2c_unregister_device(client);
		gpiochip_free_own_desc(enable);
		return PTR_ERR(reset);
	}

	mx84_led_enable = enable;
	mx84_led_client = client;
	mx84_reset_device = reset;
	pr_info("meraki-mx84: registered LP5562 status LED and reset button\n");
	return 0;
}

static void __exit mx84_board_exit(void)
{
	platform_device_unregister(mx84_reset_device);
	i2c_unregister_device(mx84_led_client);
	gpiochip_free_own_desc(mx84_led_enable);
}

late_initcall(mx84_board_init);
module_exit(mx84_board_exit);

MODULE_DESCRIPTION("Cisco Meraki MX84 status LED and reset-button board support");
MODULE_LICENSE("GPL");
