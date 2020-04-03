#include "stm8s.h"
#include "stm8s_uart1.h"

#include "circbuf.h"
#include "delay.h"
#include "nmea.h"
#include "ubxgps.h"

#include <stdbool.h>
#include <stddef.h>

// Return the number of items in a statically allocated array
#define COUNT_OF(x) ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))

#define TEST_PIN_PORT GPIOC
#define TEST_PIN_1 (1<<3)
#define TEST_PIN_2 (1<<4)

#define LDR_PORT GPIOD
#define LDR_PIN (1<<3)

#define GPS_PORT GPIOB
#define GPS_PIN_TIMEPULSE (1<<4)
#define GPS_PIN_EXTINT (1<<5)

#define BUTTON_PORT GPIOA
#define BUTTON_PIN_TIMEZONE (1<<2)
#define BUTTON_PIN_DST (1<<3)

#define MAX72XX_PORT GPIOD
#define MAX72XX_CS_PIN (1<<2)

#define kNumDigits 6
#define kNumSegments 8
static uint8_t _segmentWiseData[kNumSegments];

static int8_t _timezoneOffset = 13;
static DateTime _gpsTime = {0, 0, 0, 0, 0, 0};

static inline void uart_send_blocking(uint8_t byte)
{
    // Wait for the last transmission to complete
    while ( (UART1->SR & UART1_SR_TC) == 0 );

    // Put the byte in the TX buffer
    UART1->DR = byte;
}

static void uart_send_stream_blocking(uint8_t* bytes, uint8_t length)
{
    while (length > 0) {
        uart_send_blocking(*bytes);

        --length;
        ++bytes;
    }
}

/**
 * Add a value to a checksum (8-Bit Fletcher Algorithm)
 * The initial checksum value must be {0,0}
 */
static inline void ubx_update_checksum(uint8_t* checksum, uint8_t value)
{
	checksum[0] += value;
	checksum[1] += checksum[0];
}

static void ubx_update_checksum_multi(uint8_t* checksum, uint8_t* data, uint16_t length)
{
	for (uint16_t i = 0; i < length; ++i) {
		ubx_update_checksum(checksum, data[i]);
	}
}

static void ubx_send(uint8_t msgClass, uint8_t msgId, uint8_t* data, uint16_t length)
{
	uint8_t header[6] = {
		0xB5, 0x62, // Every message starts with these sync characters
		msgClass,
		msgId,
		(uint8_t) (length & 0xFF), // Payload length as little-endian (LSB first)
		(uint8_t) (length >> 8),
	};

	uint8_t checksum[2] = {0, 0};

	// Checksum includes the payload and the header minus its two fixed bytes
	ubx_update_checksum_multi(checksum, (uint8_t*)(&header) + 2, sizeof(header) - 2);
	ubx_update_checksum_multi(checksum, data, length);

	// Send the message over serial
	uart_send_stream_blocking(header, sizeof(header));
	uart_send_stream_blocking(data, length);
	uart_send_stream_blocking(checksum, sizeof(checksum));

    // Hack: wait for ACK response
    // TODO: replace this with actually reading the ACK/NACK response and returning true/false
    _delay_ms(50);
}

// Wait for GPS start-up
void gps_init()
{
    // Configure time-pulse
    {
        const uint8_t cfg_tp5_data[] = {
            UBX_VALUE_U8(0), // Timepulse selection (only one available on NEO-6M)
            UBX_VALUE_U8(0), // Reserved 0
            UBX_VALUE_U16(0), // Reserved 1
            UBX_VALUE_S16(50), // Antenna cable delay (nS)
            UBX_VALUE_S16(0), // RF group delay (nS)
            UBX_VALUE_U32(0), // Freqency of time pulse in Hz
            UBX_VALUE_U32(1), // Freqency of time pulse in Hz when locked to GPS time
            UBX_VALUE_U32(1000), // Length of time pulse in uS
            UBX_VALUE_U32(10000), // Length of time pulse in uS when locked to GPS time
            UBX_VALUE_S32(0), // User configurable timepuse delay (nS)
            UBX_VALUE_U32(0b11111111), // Flags
        };

        ubx_send(0x06, 0x31, cfg_tp5_data, sizeof(cfg_tp5_data));
    }

    // Configure stationary mode
    {
        const uint8_t cfg_nav5_data[] = {
            UBX_VALUE_U16(0b00111111), // Mask selecting settings to apply
            UBX_VALUE_U8(2), // "Stationary" dynamic platform model
            UBX_VALUE_U8(3), // Get either a 3D or 2D fix
            UBX_VALUE_U32(0), // fixed altitude for 2D
            UBX_VALUE_U32(0), // fixed altitude variance for 2D
            UBX_VALUE_U8(20), // minimum elevation is 20 degrees...
            UBX_VALUE_U8(180), // maximum time to perform dead reckoning in case of GPS signal loss (s)
            UBX_VALUE_U16(100), // position DoP mask is 10.0...
            UBX_VALUE_U16(100), // time DoP mask is 10.0...
            UBX_VALUE_U16(100), // position accuracy mask in meters...
            UBX_VALUE_U16(100), // time accuracy mask in meters...
            UBX_VALUE_U8(0), // static hold threshold is 0 cm/s...
            UBX_VALUE_U8(60), // dynamic GNSS timeout is 60 seconds (not used)...
            UBX_VALUE_U32(0), // Reserved
            UBX_VALUE_U32(0), // Reserved
            UBX_VALUE_U32(0), // Reserved
        };

        ubx_send(0x06, 0x24, cfg_nav5_data, sizeof(cfg_nav5_data));
    }

    // Disable NMEA messages we don't care about
    // These are mostly positioning messages since we're operating in stationary mode
    {
        // ID byte of each message to disable
        // All messages share the same class byte of 0xF0
        const uint8_t disableMessages = {
            0x05, // VTG (Course over ground and ground speed)
            0x01, // GGL (Latitude and longitude with time of position fix and status)
        };
    }
}

inline void spi_send_blocking(uint8_t data)
{
    // Load data into TX register
    SPI->DR = data;

    // Wait for TX buffer empty flag
    while (SPI_GetFlagStatus(SPI_FLAG_TXE) != SET);
}

static void max7219_cmd(uint8_t address, uint8_t data)
{
    // Chip select (active low)
    MAX72XX_PORT->ODR &= ~MAX72XX_CS_PIN;

    spi_send_blocking(address);
    spi_send_blocking(data);

    // Wait for busy flag to clear
    while (SPI_GetFlagStatus(SPI_FLAG_BSY) != RESET);

    // Disable chip select
    MAX72XX_PORT->ODR |= MAX72XX_CS_PIN;
    _delay_us(10);
}

/**
 * Send complete digit/segment register configuration to the MAX72XX
 *
 * This must be called for max7219_set_digit calls to be shown on the display.
 *
 * All 8 digit (sink) registers need to be set at once as our wiring is flipped in order
 * to drive common anode displays. Each of our phsyical digits is represented by one bit
 * in each of the 8 digit registers (instead of the normal one-byte-per-digit wiring).
 */
static void max7219_write_digits()
{
    for (uint8_t i = 0; i < kNumSegments; ++i) {
        const uint8_t digitRegister = i + 1;
        max7219_cmd(digitRegister, _segmentWiseData[i]);
    }
}

/**
 * Emulate setting a digit register on the MAX72XX (mapped for common anode wiring)
 * Digit register is 1-indexed.
 */
static void max7219_set_digit(uint8_t digitRegister, uint8_t segments)
{
    // Map of logical digit index to rev 1.0 board wiring using a MAX7221
    const uint8_t digitMap[kNumSegments] = {
        /* 0: */ 0,
        /* 1: */ 4,
        /* 2: */ 3,
        /* 3: */ 1,
        /* 4: */ 5,
        /* 5: */ 2,
    };

    // Map logical digit to actual hardware wiring
    const uint8_t mappedDigit = digitMap[digitRegister - 1];

    // Create a bitmask for the 1-indexed digit register
    const uint8_t digitMask = (1<<mappedDigit);

    // Set/clear the digit's corresponding bit in each segment byte
    for (uint8_t i = 0; i < kNumSegments; ++i) {
        if (segments & 0x01) {
            _segmentWiseData[i] |= digitMask;
        } else {
            _segmentWiseData[i] &= ~digitMask;
        }

        // Next segment
        segments >>= 1;
    }
}

/**
 * Emulate writing a digit under the MAX72XX's BCD display mode (mapped for common anode wiring)
 * Digit register is 1-indexed
 */
static void max7219_set_digit_bcd(uint8_t digitRegister, uint8_t value)
{
    const uint8_t bcdMap[16] = {
        0b00111111, // 0
        0b00000110, // 1
        0b01011011, // 2
        0b01001111, // 3
        0b01100110, // 4
        0b01101101, // 5
        0b01111101, // 6
        0b00000111, // 7
        0b01111111, // 8
        0b01101111, // 9
        0b01000000, // -
        0b01111001, // E
        0b01110110, // H
        0b00111000, // L
        0b01110011, // P
        0x0,        // Blank
    };

    // Get segments that should be on
    uint8_t segments = bcdMap[value & 0xF];

    // Turn on the decimal point if the most-significant bit is set
    segments |= (value & 0x80);

    max7219_set_digit(digitRegister, segments);
}

static void max7219_init(void)
{
    // Disable binary decode mode
    // We can't use this as we're using the MAX7219 to drive common-anode displays
    max7219_cmd(0x09, 0x00);

    // Set scan mode to 8x8
    // This is the "number of digits" command, but we've wired these as the segments
    max7219_cmd(0x0B, 0x7);

    // Disable test mode
    max7219_cmd(0x0F, 0);

    // Enable display
    max7219_cmd(0x0C, 1);
}

/**
 * Modify the passed time with the current timezone offset
 */
static void apply_timezone_offset(DateTime* now)
{
    // Adjust hour for timezone
    int8_t hour = now->hour;
    hour += _timezoneOffset;

    if (hour > 23) {
        hour -= 24;
    } else if (hour < 0) {
        hour += 24;
    }

    now->hour = hour;
}

/**
 * Send the current time to the MAX7219 as 6 BCD digits
 */
static void display_update(DateTime* now)
{
    // Block interrupts during display update to avoid contention with brightness update interrupt
    disableInterrupts();

    // Send time to display
    uint8_t digit = 1;

    for (int8_t i = 0; i < 3; ++i) {

        // Manually digit into tens and ones columns
        // This saves 25 bytes vs. using the divide and modulo operators.
        uint8_t ones = ((uint8_t*) now)[i];
        uint8_t tens = 0;

        while (ones >= 10) {
            ones -= 10;
            ++tens;
        }

        max7219_set_digit_bcd(digit++, tens);
        max7219_set_digit_bcd(digit++, ones);
    }

    max7219_write_digits();

    enableInterrupts();
}

/**
 * Set all digits on the display to a value with no illuminated segments
 */
static void display_clear()
{
    for (uint8_t i = 0; i < kNumSegments; ++i) {
        _segmentWiseData[i] = 0x0;
    }
}

static void display_no_signal()
{
    static uint8_t waitIndicator = 0;

    display_clear();

    // Turn on the decimal point on one digit (digits are 1-indexed)
    max7219_set_digit_bcd(waitIndicator + 1, 0x8F);

    ++waitIndicator;
    if (waitIndicator == kNumDigits) {
        waitIndicator = 0;
    }

    max7219_write_digits();
}

void display_error_code(uint8_t code)
{
    display_clear();

    // Display error code
    max7219_set_digit_bcd(1, 11 /* E */);
    max7219_set_digit(2, 0b01010000 /* r */);
    max7219_set_digit_bcd(3, code);

    max7219_write_digits();
}

static inline uint16_t read_adc_buffer()
{
    // Load ADC reading (least-significant byte must be read first)
    uint16_t result = ADC1->DRL;
    result |= (ADC1->DRH << 8);

    return result;
}

static uint8_t _displayBrightness = 0;

void display_adjust_brightness(void)
{
    // State to obtain an average of LDR readings
    // The size of this array should  be a power of two to make division simpler
    static uint16_t averageBuffer[16];
    static uint8_t writeIndex = 0;
    static uint16_t runningTotal = 0;

    const uint16_t reading = read_adc_buffer();

    // Adjust running total with the new value
    runningTotal -= averageBuffer[writeIndex];
    runningTotal += reading;

    // Append new reading
    averageBuffer[writeIndex] = reading;
    writeIndex = (writeIndex + 1) % COUNT_OF(averageBuffer);

    const uint16_t average = runningTotal/COUNT_OF(averageBuffer);

    // Scale the 1024 ADC values to fit in the 16 brightness levels of the MAX72XX
    max7219_cmd(0x0A, average / 64);
}

int main()
{
    // Configure the clock for maximum speed on the 16MHz HSI oscillator
    // At startup the clock output is divided by 8
    // CLK->CKDIVR = (CLK_PRESCALER_CPUDIV8 & CLK_CKDIVR_CPUDIV);
    CLK->CKDIVR = 0x0;

    // Delay to prevent partial initialisation when programming (reset is only blipped low)
    _delay_ms(10);

    // Configure test points as outputs
    TEST_PIN_PORT->DDR = TEST_PIN_1 | TEST_PIN_2; // Output mode
    TEST_PIN_PORT->CR1 = TEST_PIN_1 | TEST_PIN_2; // Push-pull mode
    TEST_PIN_PORT->ODR = TEST_PIN_1 | TEST_PIN_2; // Speed up to 10Mhz

    // Interrupt outputs from GPS as inputs
    EXTI->CR1 |= 0x04; // Rising edge triggers interrupt
    GPS_PORT->DDR &= ~GPS_PIN_TIMEPULSE; // Input mode
    GPS_PORT->CR1 |= GPS_PIN_TIMEPULSE;  // Enable internal pull-up
    GPS_PORT->CR2 |= GPS_PIN_TIMEPULSE;  // Interrupt enabled

    BUTTON_PORT->DDR &= ~(BUTTON_PIN_DST | BUTTON_PIN_TIMEZONE); // Input mode
    BUTTON_PORT->CR1 |= BUTTON_PIN_DST | BUTTON_PIN_TIMEZONE; // Enable internal pull-up

    // MAX7219  chip select as output
    MAX72XX_PORT->DDR |= MAX72XX_CS_PIN; // Output mode
    MAX72XX_PORT->CR1 |= MAX72XX_CS_PIN; // Push-pull mode
    MAX72XX_PORT->CR2 |= MAX72XX_CS_PIN; // Speed up to 10Mhz
    MAX72XX_PORT->ODR |= MAX72XX_CS_PIN; // Active low: initially set high

    // Enable SPI for MAX7219 display driver
    SPI_Init(SPI_FIRSTBIT_MSB,
             SPI_BAUDRATEPRESCALER_32,
             SPI_MODE_MASTER,
             SPI_CLOCKPOLARITY_LOW,
             SPI_CLOCKPHASE_1EDGE,
             SPI_DATADIRECTION_1LINE_TX,
             SPI_NSS_SOFT, 0);

    SPI_Cmd(ENABLE);

    // Enable UART for GPS comms
    UART1_Init(9600, // Baud rate
               UART1_WORDLENGTH_8D,
               UART1_STOPBITS_1,
               UART1_PARITY_NO,
               UART1_SYNCMODE_CLOCK_DISABLE,
               UART1_MODE_TXRX_ENABLE);

    UART1_ITConfig(UART1_IT_RXNE_OR, ENABLE);
    UART1_Cmd(ENABLE);


    // Enable ADC for ambient light sensing
    // Conversion is triggered by timer 1's TRGO event
    ADC1->CSR = ADC1_CSR_EOCIE | // Enable interrupt at end of conversion
                ADC1_CHANNEL_4; // Convert on ADC channel 4 (pin D3)

    ADC1->CR2 = ADC1_CR2_ALIGN | // Place LSB in lower register
                ADC1_CR2_EXTTRIG; // Start conversion on external event (TIM1 TRGO event)

    ADC1->CR1 = ADC1_PRESSEL_FCPU_D18 | // ADC @ fcpu/18
                ADC1_CR1_ADON; // Power on the ADC


    // Configure TIM1 to trigger ADC conversion automatically
    const uint16_t tim1_prescaler = 16000; // Prescale the 16MHz system clock to a 1ms tick
    TIM1->PSCRH = (tim1_prescaler >> 8);
    TIM1->PSCRL = (tim1_prescaler & 0xFF);

    const uint16_t tim1_auto_reload = 69; // Number of milliseconds to count to
    TIM1->ARRH = (tim1_auto_reload >> 8);
    TIM1->ARRL = (tim1_auto_reload & 0xFF);

    const uint16_t tim1_compare_reg1 = 1; // Create a 1ms OC1REF pulse (PWM1 mode)
    TIM1->CCR1H = (tim1_compare_reg1 >> 8);
    TIM1->CCR1L = (tim1_compare_reg1 & 0xFF);

    // Use capture-compare channel 1 to trigger ADC conversions
    // This doesn't have any affect on pin outputs as TIM1_CCER1_CC1E and TIM1_BKR_MOE are not set
    TIM1->CCMR1 = TIM1_OCMODE_PWM1; // Make OC1REF high when counter is less than CCR1 and low when higher
    TIM1->EGR = TIM1_EGR_CC1G; // Enable compare register 1 event
    TIM1->CR2 = TIM1_TRGOSOURCE_OC1REF; // Enable TRGO event on compare match

    TIM1->EGR |= TIM1_EGR_UG; // Generate an update event to register new settings

    TIM1->CR1 = TIM1_CR1_CEN; // Enable the counter

    enableInterrupts();

    max7219_init();
    max7219_write_digits();

    max7219_cmd(0x0A, 0xA);

    // Illuminate each of the outline segments one at a time
    for (uint8_t i = 0; i < 6; ++i) {
        _segmentWiseData[i] = 0xFF;
        max7219_write_digits();

        _segmentWiseData[i] = 0x00;
        _delay_ms(50);
    }

    max7219_write_digits();

    gps_init();

    while (true) {
        // Wait for a line of text from the GPS unit
        const GpsReadStatus status = gps_read_time(&_gpsTime);

        switch (status) {
            case kGPS_Success:
                // Update the display with the new parsed time
                apply_timezone_offset(&_gpsTime);
                display_update(&_gpsTime);
                break;

            case kGPS_NoMatch:
                // Ignore partial and unknown sentences
                break;

            case kGPS_NoSignal:
                // Walk the decimal point across the display to indicate activity
                display_no_signal();
                break;

            case kGPS_InvalidChecksum:
                display_error_code(1);
                break;

            case kGPS_BadFormat:
                // This state is returned if the UART line isn't pulled high (ie. GPS unplugged)
                display_error_code(2);
                break;

            case kGPS_UnknownState:
                // This state is returned if the UART line isn't pulled high (ie. GPS unplugged)
                display_error_code(3);
                break;
        }
    }
}

volatile static CircBuf _uartBuffer;

char uart_read_byte()
{
    // Block until a character is available
    while (circbuf_is_empty(&_uartBuffer));

    return circbuf_pop(&_uartBuffer);
}

void uart1_receive_irq(void) __interrupt(ITC_IRQ_UART1_RX)
{
    const uint8_t byte = ((uint8_t) UART1->DR);

    circbuf_append(&_uartBuffer, byte);
}

void gps_irq(void) __interrupt(ITC_IRQ_PORTB)
{
    // TODO: make this volatile
    // Enable decimal point on all digits
    // _segmentWiseData[7] = (1<<2);
    // max7219_write_digits();
}

void adc_irq(void) __interrupt(ITC_IRQ_ADC1)
{
    // Clear the end of conversion bit so this interrupt can fire again
    ADC1->CSR &= ~ADC1_CSR_EOC;

    display_adjust_brightness();
}