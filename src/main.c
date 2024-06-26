// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Gabriel Marcano, 2023

#include <scron.h>
#include <power_control.h>
#include <artemia.h>

#include <adc.h>
#include <spi.h>
#include <lora.h>
#include <gpio.h>
#include <uart.h>
#include <am1815.h>
#include <syscalls.h>
#include <flash.h>
#include <asimple_littlefs.h>
#include <bmp280.h>
#include <pdm.h>
#include <fft.h>
#include <kiss_fftr.h>
#include <systick.h>

#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

#include <sys/time.h>

#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

static struct spi_bus *spi_bus;
static struct spi_bus *spi_bus_2;
static struct spi_device *flash_spi;
static struct spi_device *bmp280_spi;
static struct spi_device *rtc_spi;
static struct spi_device *lora_spi;
static struct am1815 rtc;
static struct power_control power_control;
static struct scron scron;
static struct uart *uart;
static struct asimple_littlefs fs;
static struct flash flash;
static struct bmp280 bmp280;
static struct adc adc;
static struct gpio adc_enable_vadp;
static struct gpio adc_enable_vrtc;
static struct gpio lora_enable;
static struct pdm *pdm;
static struct fft fft;
static struct lora lora;

static const uint8_t PHOTORES_PIN = 16;
static const uint8_t VADP_PIN = 29;
static const uint8_t VRTC_PIN = 11;

// Convert tv_sec, which is a long representing seconds, to a string in buffer
// Make sure to initialize buffer before calling
// uint8_t buffer[21] = {0};
void time_to_string(uint8_t buffer[21], uint64_t tv_sec) {
	// This is one way to prepare an uint64_t as a string
	int max = ceil(log10(tv_sec));
	uint64_t tmp = tv_sec;
	for (uint8_t *c = buffer + max - 1; c >= buffer; --c)
	{
		*c = '0' + (tmp % 10);
		tmp /= 10;
	}
}

// Write a line to fp in the format "data,time\n"
// Gets time from the RTC
void write_csv_line(FILE * fp, uint32_t data) {
	struct timeval time = am1815_read_time(&rtc);
	uint8_t buffer[21] = {0};
	time_to_string(buffer, (uint64_t) time.tv_sec);
	fprintf(fp, "%s,%lu\r\n", buffer, data);
}

static int task_get_temperature_data(void* data)
{
	(void)data;
	// Open the file and check the header
	char header[] = "time,temperature data celsius\r\n";
	int len = strlen(header);
	FILE * tfile = fopen("fs:/temperature_data.csv", "a+");
	fseek(tfile, 0 , SEEK_SET);

	char buffer[len];
	fread(buffer, len, 1, tfile);
	if(strncmp(header, buffer, len) != 0){
		fprintf(tfile, "%s", header);
	}
	fseek(tfile, 0, SEEK_END);

	// Read current temperature from BMP280 sensor and write to flash
	uint32_t raw_temp = bmp280_get_adc_temp(&bmp280);
	uint32_t compensate_temp = (uint32_t) (bmp280_compensate_T_double(&bmp280, raw_temp) * 1000);

	write_csv_line(tfile, compensate_temp);
    fclose(tfile);

	return 0;
}

static int task_get_pressure_data(void* data)
{
	(void)data;
	// Open the file and check the header
	char header[] = "time,pressure data pascals\r\n";
	int len = strlen(header);
	FILE * pfile = fopen("fs:/pressure_data.csv", "a+");
	fseek(pfile, 0 , SEEK_SET);
	char buffer[len];
	fread(buffer, len, 1, pfile);
	if(strncmp(header, buffer, len) != 0){
		fprintf(pfile, "%s", header);
	}
	fseek(pfile, 0, SEEK_END);

	// Read current pressure from BMP280 sensor and write to flash
	uint32_t raw_temp = bmp280_get_adc_temp(&bmp280);
	uint32_t raw_press = bmp280_get_adc_pressure(&bmp280);
	uint32_t compensate_press = (uint32_t) (bmp280_compensate_P_double(&bmp280, raw_press, raw_temp));
	write_csv_line(pfile, compensate_press);

	fclose(pfile);

	return 0;
}

static int task_get_light_data(void* data)
{
	(void)data;
	// Open the file and check the header
	char header[] = "time,light data ohms\r\n";
	int len = strlen(header);
	FILE * lfile = fopen("fs:/light_data.csv", "a+");
	fseek(lfile, 0 , SEEK_SET);
	char buffer[len];
	fread(buffer, len, 1, lfile);
	if(strncmp(header, buffer, len) != 0){
		fprintf(lfile, "%s", header);
	}
	fseek(lfile, 0, SEEK_END);

	// Read current resistance of the Photo Resistor and write to flash
	uint32_t adc_data[1] = {0};
	uint8_t pins[] = {PHOTORES_PIN};
	uint32_t resistance;

	adc_trigger(&adc);
	while (!(adc_get_sample(&adc, adc_data, pins, 1))){
		// am_util_stdio_printf("you shouldn't be here\r\n");
	}
	const double reference = 2.0;
	double voltage = adc_data[0] * reference / ((1 << 14) - 1);
	// am_util_stdio_printf("VOLTAGE: %f\r\n", voltage);
	resistance = (uint32_t)((10000 * voltage)/(3.3 - voltage));

	write_csv_line(lfile, resistance);

	fclose(lfile);

	return 0;
}

static int task_get_microphone_data(void* data)
{
	(void)data;
	// Open the file and check the header
	char header[] = "time,microphone data Hz\r\n";
	int len = strlen(header);
	FILE * mfile = fopen("fs:/microphone_data.csv", "a+");
	fseek(mfile, 0 , SEEK_SET);
	char buffer[len];
	fread(buffer, len, 1, mfile);
	if(strncmp(header, buffer, len) != 0){
		fprintf(mfile, "%s", header);
	}
	fseek(mfile, 0, SEEK_END);

	// Turn on the PDM and start the first DMA transaction.
	uint32_t* buffer1 = pdm_get_buffer1(pdm);
	memset(buffer1, 2, PDM_SIZE * sizeof(uint32_t));
	pdm_flush(pdm);
	pdm_data_get(pdm, buffer1);
	while(!isPDMDataReady())
	{
		am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_DEEP);
	}

	uint32_t max = 0;
	uint32_t N = fft_get_N(&fft);
	if (isPDMDataReady())
	{
		int16_t *pi16PDMData = (int16_t *)buffer1;
		// FFT transform
		kiss_fft_scalar in[N];
		kiss_fft_cpx out[N / 2 + 1];
		for (uint32_t j = 0; j < N; j++){
			in[j] = pi16PDMData[j];
		}
		max = TestFftReal(&fft, in, out);
	}

	// Save frequency with highest amplitude to flash
	write_csv_line(mfile, max);
	fclose(mfile);

	return 0;
}

static int task_send_lora(void* data)
{
	(void)data;
	unsigned char buffer[] = "Hello World! :)";

	lora_send_packet(&lora, buffer, strlen((const char*)buffer));

	am_util_delay_ms(1000);		//wait 1 second

	printf("done sending\r\n");

	return 0;
}

#define ARRAY_SIZE(array) (sizeof(array)/sizeof(*array))

/*
 * Measured minimum voltages for each task:
 *  - temperature: 1.40
 *  - pressure: 1.85
 *  - light: 1.69
 *  - microphone: 2.08
 *
 *  Going to add some headroom to all of them.
 *  One caveat-- we can't set a minimum above 2.0, as that's the maximum our
 *  ADC can detect
 */
static struct scron_task tasks_[] = {
	{
		.name = "task_get_temperature_data",
		.minimum_voltage = 1.8,
		.function = task_get_temperature_data,
		.schedule = {
			.hour = -1,
			.minute = -1,
			.second = 10,
		},
	},
	{
		.name = "task_get_pressure_data",
		.minimum_voltage = 1.9,
		.function = task_get_pressure_data,
		.schedule = {
			.hour = -1,
			.minute = -1,
			.second = 20,
		},
	},
	{
		.name = "task_get_light_data",
		.minimum_voltage = 1.8,
		.function = task_get_light_data,
		.schedule = {
			.hour = -1,
			.minute = -1,
			.second = 30,
		},
	},
	{
		.name = "task_get_microphone_data",
		.minimum_voltage = 2.00,
		.function = task_get_microphone_data,
		.schedule = {
			.hour = -1,
			.minute = -1,
			.second = 40,
		},
	},
	{
		.name = "task_send_lora",
		.minimum_voltage = 1.8,
		.function = task_send_lora,
		.schedule = {
			.hour = -1,
			.minute = -1,
			.second = 50,
		},
	},
};

static struct scron_tasks tasks = {
	.tasks = tasks_,
	.size = ARRAY_SIZE(tasks_),
};

/**
 * The basic gist is to read back the last 2 time_t saved to the file, and if
 * they match we likely don't have any corruption. If they don't go back until
 * we find a matching valid pair.
 */
static void load_callback(const char *name, time_t *last_run)
{
	char *buf = malloc(strlen(name) + 4 + 1);
	memcpy(buf, "fs:/", 4);
	memcpy(buf + 4, name, strlen(name) + 1);
	FILE *file = fopen(buf, "r");
	free(buf);
	if (!file)
	{
		*last_run = 0;
		return;
	}
	fseek(file, -16, SEEK_END);
	time_t time1, time2;
	static_assert(sizeof(time1) == 8, "wrong time_t size guess");
	do
	{
		fread(&time1, sizeof(time1), 1, file);
		fread(&time2, sizeof(time1), 1, file);
		fseek(file, -32, SEEK_CUR);
		// FIXME implement tellg so we can tell when we've gone too far back
	} while (time1 != time2);
	fclose(file);

	*last_run = time1;
}

/**
 * The basic gist is to append to the file (FIXME what if we run out of space?)
 * two time_t, which are checked by load_callback.
 */
static void save_callback(const char *name, time_t last_run)
{
	char *buf = malloc(strlen(name) + 4 + 1);
	memcpy(buf, "fs:/", 4);
	memcpy(buf + 4, name, strlen(name) + 1);
	FILE *file = fopen(buf, "a");
	free(buf);
	if (!file)
	{
		// FIXME we should somehow alert that there is a bug or issue
		return;
	}
	fwrite(&last_run, sizeof(last_run), 1, file);
	fwrite(&last_run, sizeof(last_run), 1, file);
	fclose(file);
}

__attribute__((constructor))
static void redboard_init(void)
{
	power_control_init(&power_control, 42, 43);
	// Prepare MCU by init-ing clock, cache, and power level operation
	am_hal_clkgen_control(AM_HAL_CLKGEN_CONTROL_SYSCLK_MAX, 0);
	am_hal_cachectrl_config(&am_hal_cachectrl_defaults);
	am_hal_cachectrl_enable();
	am_bsp_low_power_init();
	am_hal_sysctrl_fpu_enable();
	am_hal_sysctrl_fpu_stacking_enable(true);

	// After basic init is done, enable interrupts
	am_hal_interrupt_master_enable();

	spi_bus = spi_bus_get_instance(SPI_BUS_0);
	spi_bus_2 = spi_bus_get_instance(SPI_BUS_1);
	spi_bus_enable(spi_bus);
	flash_spi = spi_device_get_instance(spi_bus, SPI_CS_2, 24000000u);
	bmp280_spi = spi_device_get_instance(spi_bus, SPI_CS_1, 10000000u);
	rtc_spi = spi_device_get_instance(spi_bus, SPI_CS_3, 2000000u);
	lora_spi = spi_device_get_instance(spi_bus_2, SPI_CS_0, 1000000u);

	// FIXME wouldn't it be better to turn on LoRa later?
	// Don't turn on SPI bus unless you want the LoRa device to power itself
	// from it, unless you turn on the main power for the device first.
	gpio_init(&lora_enable, 40, GPIO_MODE_OUTPUT, 1);
	// Per datasheet, it takes 10ms for the LoRa module to be ready.
	am_util_delay_ms(10);
	spi_bus_enable(spi_bus_2);
	lora_init(&lora, lora_spi, 915000000, 23);
	lora_standby(&lora);
	lora_set_spreading_factor(&lora, 7);
	lora_set_coding_rate(&lora, 1);
	lora_set_bandwidth(&lora, 0x7);

	am1815_init(&rtc, rtc_spi);
	// Configure Alarm pulse to shortest, just in case, and enable it
	am1815_enable_alarm_interrupt(&rtc, AM1815_SHORTEST);
	// If set, clear oscillator failure bit, ensure we're running off crystal
	// and not RC
	uint8_t OF = am1815_read_register(&rtc, 0x1D);
    uint8_t OFmask = 0b00000010;
    uint8_t OFresult = OF & ~OFmask;
    am1815_write_register(&rtc, 0x1D, OFresult);

	flash_init(&flash, flash_spi);
	asimple_littlefs_init(&fs, &flash);

	int err = asimple_littlefs_mount(&fs);
	if (err < 0)
	{
		asimple_littlefs_format(&fs);
		asimple_littlefs_mount(&fs);
	}

	uart = uart_get_instance(UART_INST0);

	bmp280_init(&bmp280, bmp280_spi);

	uint8_t pins[] = {PHOTORES_PIN, VADP_PIN, VRTC_PIN};
	adc_init(&adc, pins, ARRAY_SIZE(pins));

	pdm = pdm_get_instance();
	fft_init(&fft);

	gpio_init(&adc_enable_vrtc, 0, GPIO_MODE_OUTPUT, 1);
	gpio_init(&adc_enable_vadp, 1, GPIO_MODE_OUTPUT, 1);

	syscalls_rtc_init(&rtc);
	syscalls_uart_init(uart);
	syscalls_littlefs_init(&fs);

	scron_init(&scron, &tasks);
	scron_load(&scron, load_callback);

	// initialize systick
	systick_reset();
	systick_start();
}

__attribute__((destructor))
static void redboard_shutdown(void)
{
	gpio_set(&lora_enable, false);
	gpio_set(&adc_enable_vrtc, false);
	gpio_set(&adc_enable_vadp, false);
	scron_save(&scron, save_callback);
	power_control_shutdown(&power_control);
}

static double convert_adc_voltage(uint32_t sample)
{
	return (sample * 2.0) / 16383u;
}

int main(void)
{
	for(;;)
	{
		// Request sample from ADC, and while that's happening fetch the time
		// from the RTC
		adc_trigger(&adc);
		struct timeval now;
		gettimeofday(&now, NULL);
		//
		// This is a FIXME for testing purposes only-- if the last_run
		// timestamp is ahead of now, move now to that timestamp
		// We avoid issues with falsifying data by comparing timestamps with
		// events logged through other means, e.g. power traces from
		// RocketLogger
		{
			size_t scron_history = scron_get_task_count(&scron);
			time_t update_time = 0;
			for (size_t i = 0; i < scron_history; ++i)
			{
				if (scron.history[i].last_run > now.tv_sec)
					update_time = scron.history[i].last_run;
			}
			if (update_time)
			{
				now.tv_sec = update_time;
				am1815_write_time(&rtc, &now);
				//gettimeofday(&now, NULL);
				now = am1815_read_time(&rtc);
			}
		}
		// end of FIXME
		//
		uint32_t adc_data[2] = {0};
		uint8_t pins[] = {VRTC_PIN, VADP_PIN};
		while (!(adc_get_sample(&adc, adc_data, pins, ARRAY_SIZE(pins))));

		double current_voltage = convert_adc_voltage(adc_data[1]);
		time_t now_s = now.tv_sec;

		bool ran_task = artemia_scheduler(&scron, current_voltage, now_s);
		if (!ran_task)
		{
			// Time is stale here, as task could have taken non-negligible time
			// to run
			gettimeofday(&now, NULL);
			struct tm tm;
			gmtime_r(&now.tv_sec, &tm);
			printf("current seconds: %lu\r\n", (uint32_t)tm.tm_sec);
			// Reconfigure the alarm
			time_t next = scron_next_time(&scron);
			printf("next alarm in: %lu\r\n", (uint32_t)(next - now.tv_sec));
			struct timeval tv = { .tv_sec = next, };
			am1815_write_alarm(&rtc, &tv);
			am1815_repeat_alarm(&rtc, 6); // Repeat every FIXME minute
			am1815_enable_alarm_interrupt(&rtc, AM1815_SHORTEST);
			break;
		}
	}
}
