#include "platform.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "uart.h"
#include <string.h>
#include "queue.h"
#include "timer.h"
#include "gpio.h"
#include "leds.h"

#define BUFF_SIZE 128       // Read buffer length
#define BUTTON_PIN PC_13      // Button pin (adjust as needed for your board)
#define DIGIT_ANALYSIS_MS 500  // Time between digit analysis (0.5s)
#define LED_BLINK_MS 200    // LED blink period for even digits

// Global variables
Queue rx_queue;             // Queue for storing received characters
char input_number[BUFF_SIZE]; // Store the input number
uint32_t current_digit_index = 0; // Current digit being analyzed
uint32_t input_length = 0;   // Length of the input number
bool led_state = false;     // Current LED state (on/off)
bool led_enabled = true;    // Whether LED actions are enabled
uint32_t button_press_count = 0; // Count of button presses
bool is_analyzing = false;  // Flag to indicate if analysis is in progress
uint32_t blink_counter = 0; // Counter for LED blinking
bool continuous_mode = false; // Flag for continuous analysis mode (number ending with '-')
bool new_input_available = false; // Flag to indicate new input is available

// Function prototypes
void uart_rx_isr(uint8_t rx);
void button_isr(int status);
void timer_digit_analysis_callback(void);
void start_digit_analysis(void);
void analyze_current_digit(void);
void set_led(bool state);

int main() {
    // Variables to help with UART read
    uint8_t rx_char = 0;
    char buff[BUFF_SIZE];
    uint32_t buff_index;

    // Initialize the receive queue and UART
    queue_init(&rx_queue, 128);
    uart_init(115200);
    uart_set_rx_callback(uart_rx_isr); // Set the UART receive callback function
    uart_enable(); // Enable UART module
    
    // Initialize LEDs and button
    leds_init();
    gpio_set_mode(BUTTON_PIN, Input);
    gpio_set_trigger(BUTTON_PIN, Falling);
    gpio_set_callback(BUTTON_PIN, button_isr);
    
    // Initialize timers
    timer_init(1000); // Initialize with 1ms base time
    
    // Enable global interrupts
    __enable_irq();
    
    uart_print("\r\n*** Digit Analysis System ***\r\n");
    
    while(1) {
        // Reset for new input
        buff_index = 0;
        led_enabled = true;
        is_analyzing = false;
        continuous_mode = false;
        new_input_available = false;
        
        uart_print("\r\nEnter number: ");
        
        // Get input from UART
        do {
            // Wait until a character is received in the queue
            while (!queue_dequeue(&rx_queue, &rx_char))
                __WFI(); // Wait for Interrupt

            if (rx_char == 0x7F) { // Handle backspace character
                if (buff_index > 0) {
                    buff_index--; // Move buffer index back
                    uart_tx(rx_char); // Send backspace character to erase on terminal
                }
            } else {
                // Store and echo the received character back
                buff[buff_index++] = (char)rx_char; // Store character in buffer
                uart_tx(rx_char); // Echo character back to terminal
            }
        } while (rx_char != '\r' && buff_index < BUFF_SIZE); // Continue until Enter key or buffer full
        
        // Replace the last character with null terminator to make it a valid C string
        buff[buff_index - 1] = '\0';
        uart_print("\r\n"); // Print newline
        
        // Check if buffer overflow occurred
        if (buff_index >= BUFF_SIZE) {
            uart_print("Buffer overflow detected! Please enter a shorter number.\r\n");
            continue;
        }
        
        // Process the input, filtering out non-numeric characters except for trailing '-'
        uint32_t valid_index = 0;
        for (uint32_t i = 0; i < buff_index - 1; i++) {
            // Check for trailing '-' which indicates continuous mode
            if (buff[i] == '-' && i == buff_index - 2) {
                continuous_mode = true;
                break;
            }
            // Only keep numeric characters
            else if (buff[i] >= '0' && buff[i] <= '9') {
                input_number[valid_index++] = buff[i];
            }
            // Ignore other characters
        }
        
        // Null-terminate the filtered input
        input_number[valid_index] = '\0';
        input_length = valid_index;
        
        if (input_length == 0) {
            uart_print("Invalid input. Please enter at least one digit.\r\n");
            continue;
        }
        
        // Start the digit analysis
        uart_print("Starting digit analysis...\r\n");
        if (continuous_mode) {
            uart_print("Continuous mode activated (number ends with '-').\r\n");
            uart_print("Enter a new number to stop.\r\n");
        }
        
        start_digit_analysis();
        
        // Wait for analysis to complete or new input
        while (is_analyzing && !new_input_available) {
            __WFI(); // Wait for Interrupt
        }
        
        if (new_input_available) {
            uart_print("Analysis interrupted by new input.\r\n");
            // Stop the timer to halt the current analysis
            timer_disable();
            is_analyzing = false;
        } else if (!continuous_mode) {
            uart_print("Analysis complete.\r\n");
        }
    }
}

// Start the digit analysis process
void start_digit_analysis(void) {
    current_digit_index = 0;
    button_press_count = 0;
    led_enabled = true;
    is_analyzing = true;
    
    // Turn off LED initially
    set_led(false);
    
    // Set up and start the timer with our callback
    timer_set_callback(timer_digit_analysis_callback);
    timer_enable();
    
    // Analyze the first digit immediately
    analyze_current_digit();
}

// Analyze the current digit
void analyze_current_digit(void) {
    if (current_digit_index >= input_length) {
        // Analysis complete
        timer_disable(); // Stop timer
        is_analyzing = false;
        return;
    }
    
    char digit = input_number[current_digit_index];
    int digit_value = digit - '0';
    
    char message[64];
    sprintf(message, "\r\nAnalyzing digit %lu: %c (%s)\r\n", 
            current_digit_index + 1, digit, (digit_value % 2 == 0) ? "even" : "odd");
    uart_print(message);
    
    // We'll use the single timer for both digit analysis and LED blinking
    
    if (led_enabled) {
        if (digit_value % 2 == 0) {
            // Even digit: LED blinks every 200ms
            uart_print("LED will blink every 200ms\r\n");
            blink_counter = 0;
            // We'll handle blinking in the main loop
        } else {
            // Odd digit: LED toggles and stays steady
            led_state = !led_state;
            set_led(led_state);
            sprintf(message, "LED toggled to %s\r\n", led_state ? "ON" : "OFF");
            uart_print(message);
        }
    } else {
        uart_print("LED actions disabled (button pressed)\r\n");
    }
}

// Timer interrupt for digit analysis
void timer_digit_analysis_callback(void) {
    static uint32_t ms_counter = 0;
    
    // Check if new input is available - if so, disable timer and return
    if (new_input_available) {
        timer_disable();
        is_analyzing = false;
        return;
    }
    
    ms_counter++;
    
    // Check if 500ms have elapsed for digit analysis
    if (ms_counter >= DIGIT_ANALYSIS_MS) {
        ms_counter = 0;
        current_digit_index++;
        
        // Handle continuous mode (loop back to the beginning)
        if (current_digit_index >= input_length) {
            if (continuous_mode) {
                current_digit_index = 0;
                uart_print("\r\nRestarting analysis (continuous mode)...\r\n");
                analyze_current_digit();
            } else {
                // Analysis complete
                timer_disable();
                is_analyzing = false;
                uart_print("\r\nAnalysis complete.\r\n");
                return;
            }
        } else {
            // Move to next digit
            analyze_current_digit();
        }
    }
    
    // Handle LED blinking for even digits (every 200ms)
    if (led_enabled && current_digit_index < input_length) {
        char digit = input_number[current_digit_index];
        int digit_value = digit - '0';
        
        if (digit_value % 2 == 0) {
            // Only blink if the current digit is even
            if ((ms_counter % LED_BLINK_MS) == 0) {
                led_state = !led_state;
                set_led(led_state);
                
                // Only print LED state changes when they occur
                if (led_state) {
                    uart_print("LED ON\r\n");
                } else {
                    uart_print("LED OFF\r\n");
                }
            }
        }
    }
}

// This function is no longer needed as blinking is handled in the timer_digit_analysis_callback

// Button interrupt service routine
void button_isr(int status) {
    // The status parameter contains which pin triggered the interrupt
    // We only care about our button pin
    
    button_press_count++;
    
    if (is_analyzing) {
        led_enabled = !led_enabled;
        
        char message[64];
        if (!led_enabled) {
            // Disable LED actions
            sprintf(message, "\r\nLED locked. Count = %u\r\n", button_press_count);
        } else {
            // Re-enable LED actions
            sprintf(message, "\r\nLED functionality restored. Count = %u\r\n", button_press_count);
            // Re-analyze current digit to restore LED behavior
            analyze_current_digit();
        }
        uart_print(message);
    }
}

// Set LED state and handle hardware control
void set_led(bool state) {
    led_state = state;
    // Use red LED only for our blinking/toggling
    leds_set(state, 0, 0);
}

// Interrupt Service Routine for UART receive
void uart_rx_isr(uint8_t rx) {
    // Check if the received character is a printable ASCII character
    if (rx >= 0x0 && rx <= 0x7F) {
        // Store the received character
        queue_enqueue(&rx_queue, rx);
        
        // If we're currently analyzing, signal that new input is available
        if (is_analyzing && rx == '\r') {
            new_input_available = true;
        }
    }
}