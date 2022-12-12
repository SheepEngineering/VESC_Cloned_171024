/*
	Copyright 2016 - 2017 Benjamin Vedder	benjamin@vedder.se

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "app.h"

#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "mc_interface.h"
#include "timeout.h"
#include "utils_math.h"
#include "utils_sys.h"
#include "comm_can.h"
#include "commands.h"
#include "hw.h"
#include <math.h>

// Settings
#define MAX_CAN_AGE						0.1
#define MIN_MS_WITHOUT_POWER			500
#define FILTER_SAMPLES					5
#define RPM_FILTER_SAMPLES				8
#define TC_DIFF_MAX_PASS				60  // TODO: move to app_conf

#define CTRL_USES_BUTTON(ctrl_type)(\
		ctrl_type == ADC_CTRL_TYPE_CURRENT_REV_BUTTON || \
		ctrl_type == ADC_CTRL_TYPE_CURRENT_REV_BUTTON_BRAKE_ADC || \
		ctrl_type == ADC_CTRL_TYPE_CURRENT_REV_BUTTON_BRAKE_CENTER || \
		ctrl_type == ADC_CTRL_TYPE_CURRENT_NOREV_BRAKE_BUTTON || \
		ctrl_type == ADC_CTRL_TYPE_DUTY_REV_BUTTON || \
		ctrl_type == ADC_CTRL_TYPE_PID_REV_BUTTON)

// Threads
// #define THD_FUNCTION(tname, arg) void tname(void *arg)
static THD_FUNCTION(adc_thread, arg);
// #define THD_WORKING_AREA(s, n)
// stkalign_t s[THD_WORKING_AREA_SIZE(n) / sizeof(stkalign_t)]
static THD_WORKING_AREA(adc_thread_wa, 512);

// Private variables
static volatile adc_config config;
static volatile float ms_without_power = 0.0;
static volatile float decoded_level = 0.0;
static volatile float read_voltage = 0.0;
static volatile float decoded_level2 = 0.0;
static volatile float read_voltage2 = 0.0;
static volatile float adc1_override = 0.0;
static volatile float adc2_override = 0.0;
static volatile bool use_rx_tx_as_buttons = false;
static volatile bool stop_now = true;
static volatile bool is_running = false;
static volatile bool adc_detached = false;
static volatile bool buttons_detached = false;
static volatile bool rev_override = false;
static volatile bool cc_override = false;

void app_adc_configure(adc_config *conf) {
	if (!buttons_detached && (((conf->buttons >> 0) & 1) || CTRL_USES_BUTTON(conf->ctrl_type))) {
		if (use_rx_tx_as_buttons) {
			palSetPadMode(HW_UART_TX_PORT, HW_UART_TX_PIN, PAL_MODE_INPUT_PULLUP);
			palSetPadMode(HW_UART_RX_PORT, HW_UART_RX_PIN, PAL_MODE_INPUT_PULLUP);
		} else {
			palSetPadMode(HW_ICU_GPIO, HW_ICU_PIN, PAL_MODE_INPUT_PULLUP);
		}
	}

	config = *conf;
	ms_without_power = 0.0;
}

void app_adc_start(bool use_rx_tx) {
#ifdef HW_ADC_EXT_GPIO
	palSetPadMode(HW_ADC_EXT_GPIO, HW_ADC_EXT_PIN, PAL_MODE_INPUT_ANALOG);
#endif
#ifdef HW_ADC_EXT2_GPIO
	palSetPadMode(HW_ADC_EXT2_GPIO, HW_ADC_EXT2_PIN, PAL_MODE_INPUT_ANALOG);
#endif

	if (buttons_detached) {
		use_rx_tx_as_buttons = false;
	} else {
		use_rx_tx_as_buttons = use_rx_tx;
	}

	stop_now = false;
	chThdCreateStatic(adc_thread_wa, sizeof(adc_thread_wa), NORMALPRIO, adc_thread, NULL);
}

void app_adc_stop(void) {
	stop_now = true;
	while (is_running) {
		chThdSleepMilliseconds(1);
	}
}

float app_adc_get_decoded_level(void) {
	return decoded_level;
}

float app_adc_get_voltage(void) {
	return read_voltage;
}

float app_adc_get_decoded_level2(void) {
	return decoded_level2;
}

float app_adc_get_voltage2(void) {
	return read_voltage2;
}

void app_adc_detach_adc(bool detach){
	adc_detached = detach;
}

void app_adc_adc1_override(float val){
	val = utils_map(val, 0.0, 1.0, 0.0, 3.3);
	utils_truncate_number(&val, 0, 3.3);
	adc1_override = val;
}

void app_adc_adc2_override(float val){
	val = utils_map(val, 0.0, 1.0, 0.0, 3.3);
	utils_truncate_number(&val, 0, 3.3);
	adc2_override = val;
}

void app_adc_detach_buttons(bool state){
	buttons_detached = state;
}

void app_adc_rev_override(bool state){
	rev_override = state;
}

void app_adc_cc_override(bool state){
	cc_override = state;
}

float calc_efficient_power()
{
	//float rpm = mc_interface_get_rpm();
	return 0.5;
}	

static THD_FUNCTION(adc_thread, arg) {
	(void)arg;

	chRegSetThreadName("APP_ADC");
	is_running = true;

	for(;;) {
		// Sleep for a time according to the specified rate
		systime_t sleep_time = CH_CFG_ST_FREQUENCY / config.update_rate_hz;

		// At least one tick should be slept to not block the other threads
		if (sleep_time == 0) {
			sleep_time = 1;
		}
		chThdSleep(sleep_time);

		if (stop_now) {
			is_running = false;
			return;
		}

		// For safe start when fault codes occur
		if (mc_interface_get_fault() != FAULT_CODE_NONE && config.safe_start != SAFE_START_NO_FAULT) {
			ms_without_power = 0;
		}

		// Read the external ADC pin voltage
		float pwr = ADC_VOLTS(ADC_IND_EXT);

		// Override pwr value, when used from LISP
		if(adc_detached){
			pwr = adc1_override;
		}

		read_voltage = pwr;

		// Optionally apply a filter
		static float filter_val = 0.0;
		UTILS_LP_MOVING_AVG_APPROX(filter_val, pwr, FILTER_SAMPLES);

		if (config.use_filter) {
			pwr = filter_val;
		}

		// Linear mapping between the start and end voltage
		pwr = utils_map(pwr, config.voltage_start, config.voltage_end, 0.0, 1.0);

		// Truncate the read voltage
		utils_truncate_number(&pwr, 0.0, 1.0);

		// Optionally invert the read voltage
		if (config.voltage_inverted) {
			pwr = 1.0 - pwr;
		}
		decoded_level = pwr;

		// Read the external ADC pin and convert the value to a voltage.
		float turbo = ADC_VOLTS(ADC_IND_EXT2);
		read_voltage2 = turbo;

		// Optionally apply a filter
		static float filter_val_2 = 0.0;
		UTILS_LP_MOVING_AVG_APPROX(filter_val_2, turbo, FILTER_SAMPLES);

		if (config.use_filter) {
			turbo = filter_val_2;
		}

		// Map and truncate the read voltage
		turbo = utils_map(turbo, config.voltage2_start, config.voltage2_end, 0.0, 1.0);
		utils_truncate_number(&turbo, 0.0, 1.0);

		// Optionally invert the read voltage
		if (config.voltage2_inverted) {
			turbo = 1.0 - turbo;
		}
		decoded_level2 = turbo;

		// Read the button pins (Keep for Setup)
		bool cc_button = false;
		bool rev_button = false;
		if (use_rx_tx_as_buttons) {
			cc_button = !palReadPad(HW_UART_TX_PORT, HW_UART_TX_PIN);
			if ((config.buttons >> 1) & 1) {
				cc_button = !cc_button;
			}
			rev_button = !palReadPad(HW_UART_RX_PORT, HW_UART_RX_PIN);
			if ((config.buttons >> 2) & 1) {
				rev_button = !rev_button;
			}
		} else {
			// When only one button input is available, use it differently depending on the control mode
			if (config.ctrl_type == ADC_CTRL_TYPE_CURRENT_REV_BUTTON ||
                    config.ctrl_type == ADC_CTRL_TYPE_CURRENT_REV_BUTTON_BRAKE_CENTER ||
					config.ctrl_type == ADC_CTRL_TYPE_CURRENT_NOREV_BRAKE_BUTTON ||
					config.ctrl_type == ADC_CTRL_TYPE_DUTY_REV_BUTTON ||
					config.ctrl_type == ADC_CTRL_TYPE_PID_REV_BUTTON) {
				rev_button = !palReadPad(HW_ICU_GPIO, HW_ICU_PIN);
				if ((config.buttons >> 2) & 1) {
					rev_button = !rev_button;
				}
			} else {
				cc_button = !palReadPad(HW_ICU_GPIO, HW_ICU_PIN);
				if ((config.buttons >> 1) & 1) {
					cc_button = !cc_button;
				}
			}
		}

		// Override button values, when used from LISP
		if (buttons_detached) {
			cc_button = cc_override;
			rev_button = rev_override;
			if ((config.buttons >> 1) & 1) {
				cc_button = !cc_button;
			}
			if ((config.buttons >> 2) & 1) {
				rev_button = !rev_button;
			}
		}
		if (!((config.buttons >> 0) & 1)) {
			cc_button = false;
		}

		// All pins and buttons are still decoded for debugging, even
		// when output is disabled.
		if (app_is_output_disabled()) {
			continue;
		}

		// Apply deadband
		utils_deadband(&pwr, config.hyst, 1.0);

		// Apply throttle curve (COME BACK TO)
		// pwr = utils_throttle_curve(pwr, config.throttle_exp, config.throttle_exp_brake, config.throttle_exp_mode);

		if ( turbo >= 0.5 )
		{
			pwr = 1.0;
		}
		else if ( pwr >= 0.5 )
		{
			pwr = calc_efficient_power();
		}
		else
		{
			pwr = 0;
		}


		// Apply ramping (KEEP)
		static systime_t last_time = 0;
		static float pwr_ramp = 0.0;
		float ramp_time = fabsf(pwr) > fabsf(pwr_ramp) ? config.ramp_time_pos : config.ramp_time_neg;

		if (ramp_time > 0.01) {
			const float ramp_step = (float)ST2MS(chVTTimeElapsedSinceX(last_time)) / (ramp_time * 1000.0);
			utils_step_towards(&pwr_ramp, pwr, ramp_step);
			last_time = chVTGetSystemTimeX();
			pwr = pwr_ramp;
		}

		float current_rel = 0.0;
		bool current_mode = false;
		bool current_mode_brake = false;
		const volatile mc_configuration *mcconf = mc_interface_get_configuration();
		const float rpm_now = mc_interface_get_rpm();
		bool send_duty = false;

		// Use the filtered and mapped voltage for control according to the configuration.
		current_mode = true;
		current_rel = pwr;

		if (fabsf(pwr) < 0.001) {
			ms_without_power += (1000.0 * (float)sleep_time) / (float)CH_CFG_ST_FREQUENCY;
		}

		bool range_ok = read_voltage >= config.voltage_min && read_voltage <= config.voltage_max;

		// If safe start is enabled and the output has not been zero for long enough
		if ((ms_without_power < MIN_MS_WITHOUT_POWER && config.safe_start) || !range_ok) {
			static int pulses_without_power_before = 0;
			if (ms_without_power == pulses_without_power_before) {
				ms_without_power = 0;
			}
			pulses_without_power_before = ms_without_power;
			mc_interface_set_brake_current(timeout_get_brake_current());

			if (config.multi_esc) {
				for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
					can_status_msg *msg = comm_can_get_status_msg_index(i);

					if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
						comm_can_set_current_brake(msg->id, timeout_get_brake_current());
					}
				}
			}

			continue;
		}

		// Reset timeout
		timeout_reset();

		// If c is pressed and no throttle is used, maintain the current speed with PID control
		static bool was_pid = false;

		// Filter RPM to avoid glitches
		static float rpm_filtered = 0.0;
		UTILS_LP_MOVING_AVG_APPROX(rpm_filtered, mc_interface_get_rpm(), RPM_FILTER_SAMPLES);
		if (current_mode && cc_button && fabsf(pwr) < 0.001) {
			static float pid_rpm = 0.0;
			if (!was_pid) {
				was_pid = true;
				pid_rpm = rpm_filtered;
			}
			mc_interface_set_pid_speed(pid_rpm);
			continue;
		}
		was_pid = false;

		// Set the Current!!
		float current_out = current_rel;
		mc_interface_set_current_rel(current_out);

	}
}
