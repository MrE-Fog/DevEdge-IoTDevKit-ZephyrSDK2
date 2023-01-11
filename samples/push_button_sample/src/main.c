/*
 * Copyright (c) 2022 T-Mobile USA, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>
#include <inttypes.h>

#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>

#include <stdio.h>

/*
 * The escape value in the following macro should align with description
 * outputted in phase 1.
 */
#define PHASE1_ESCAPE_MS_TIME  3000
#define DEBOUNCE_DWELL_MS_TIME 20

#define SW0_NODE DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS(SW0_NODE, okay)
#error "Unsupported board: sw0 device-tree alias is not defined"
#endif
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(SW0_NODE, gpios, {0});
static struct gpio_callback button_cb_data;

/*
 * The led0 devicetree alias is optional. If present, we'll use it
 * to turn on the LED whenever the button is pressed.
 */
static struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

/*
 * Variables pertaining to the interrupt service routine
 */
static volatile int push_button_isr_count = 0;
static volatile bool print_edges = false;

/*
 * User Push Button (sw0) interrupt callback
 */
void user_push_button_intr_callback(const struct device *port, struct gpio_callback *cb,
				    gpio_port_pins_t pin_mask)
{
	static struct {
		int previous, current;
	} button_state = {-2, +2}; /* Initialize with OoB values */

	button_state.current = gpio_pin_get_dt(&button);
	gpio_pin_set_dt(&led, button_state.current);
	push_button_isr_count++;
	if (print_edges) {
		printk("\r%s interrupt (%d): %s edge%s\n", port->name, push_button_isr_count,
		       button_state.current ? " rising" : "falling",
		       button_state.current == button_state.previous ? " (bounce detected)" : "");
	}
	button_state.previous = button_state.current;
}

/*
 *	Print greeting
 */
static void greeting()
{
	puts("\n\n\t\t\tWelcome to T-Mobile DevEdge!\n\n"
	     "This application aims to demonstrate the Gecko's Energy Mode 2 (EM2) (Deep\n"
	     "Sleep Mode) and Wake capabilities in conjunction with the SW0 interrupt pin,\n"
	     "which is connected to the user pushbutton switch of the DevEdge module.\n");
}

/*
 *	Set up the GPIO and interrupt service structures
 */
static void setup(void)
{
	int ret;

	puts("Setting up GPIO and IRS structures");

	if (!device_is_ready(button.port)) {
		printk("Error: button device %s is not ready\n", button.port->name);
		return;
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT | GPIO_PULL_UP | GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		printk("Error %d: failed to configure %s pin %d\n", ret, button.port->name,
		       button.pin);
		return;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		printk("Error %d: failed to configure interrupt on %s pin %d\n", ret,
		       button.port->name, button.pin);
		return;
	}
	gpio_init_callback(&button_cb_data, user_push_button_intr_callback, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);
	printf("Set up button at %s pin %d\n\n", button.port->name, button.pin);
}

/*
 *	Phase 1 of the sample
 */
static void phase1()
{
	struct {
		int previous, current;
	} button_state = {1, 1}; /* Initialize with OoB values */

	/* Disable the interrupt edge printing mechanism */
	print_edges = false;

	puts("Phase 1\n"
	     "In this phase, a firmware-based debounce filter cleans up the interrupt signal\n"
	     "generated by the user pushbutton switch. Momentarily pressing the pushbutton\n"
	     "will display the amount of time the button was pressed. Releasing the button\n"
	     "after holding it for more than three seconds will advance the demonstration to\n"
	     "the next and final phase.\n\n"
	     "Awaiting debounced user pushbutton interrupts...");

	for (int64_t elapsedTime = 0, referenceTime = 0; PHASE1_ESCAPE_MS_TIME > elapsedTime;
	     button_state.current = gpio_pin_get_dt(&button)) {
		if (button_state.previous < button_state.current) {
			/* Button press detected */
			referenceTime = k_uptime_get();
			button_state.previous = button_state.current;
		} else if (button_state.previous > button_state.current) {
			/* Button release detected */
			if (referenceTime) {
				elapsedTime = k_uptime_delta(&referenceTime);
				printf("\rPushbutton press-event (duration: %lldms)\n",
				       elapsedTime);
			}
			button_state.previous = button_state.current;
		} else {
			/* Debounce time */
			k_msleep(DEBOUNCE_DWELL_MS_TIME);
		}
	}
	puts("Exiting phase 1\n");
}

/*
 * Phase 2 of the sample
 */
static void phase2()
{
	puts("Phase 2\n"
	     "In this phase, the SW0 interrupt signal temporarily wakes the Gecko from EM2 to\n"
	     "print the signal's edge, rising or falling.\n\n"
	     "Awaiting unfiltered user pushbutton interrupts...");

	/* Reset the interrupt counter */
	push_button_isr_count = 0;

	/* Enable the interrupt edge printing mechanism */
	print_edges = true;

	/* Enable Energy Mode 2 (EM2) -- Deep Sleep Mode */
	pm_state_force(0u, &(struct pm_state_info){PM_STATE_SUSPEND_TO_IDLE, 0, 0});

	/* Maintain sleep */
	while (true) {
		puts("Entering EM2 sleep...\n");
		k_msleep(1);
		puts("Awake from EM2 sleep...");
	}
}

/* Prevent the deep sleep (non-recoverable shipping mode) from being entered on
 * long timeouts or `K_FOREVER` due to the default residency policy.
 *
 * This has to be done before anything tries to sleep, which means
 * before the threading system starts up between PRE_KERNEL_2 and
 * POST_KERNEL.  Do it at the start of PRE_KERNEL_2.
 */
static int disable_deep_sleep(const struct device *dev)
{
	ARG_UNUSED(dev);

	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	return 0;
}
SYS_INIT(disable_deep_sleep, PRE_KERNEL_2, 0);

/*
 * Entry point
 */
void main(void)
{
	greeting();
	setup();
	phase1();
	phase2();
}