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
void button_isr();
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
    uart_print("\r\nUART enabled\r\n");
    
    uart_print("\r\n*** Digit Analysis System ***\r\n");
    // Initialize LEDs and button
    leds_init();
    uart_print("LEDs initialized\r\n");
    
    // Setup the on-board button (PC_13)
    // The on-board button is active-low (connects to ground when pressed)
    gpio_set_mode(BUTTON_PIN, PullUp);  // Use pull-up since button is active-low
    uart_print("Button mode set to PullUp\r\n");
    
    // Use Falling edge trigger since button is active-low
    gpio_set_trigger(BUTTON_PIN, Falling);
    uart_print("Button trigger set to Falling\r\n");
    
    // Set the callback function
    gpio_set_callback(BUTTON_PIN, button_isr);
    uart_print("Button callback set\r\n");
    
    // Initialize timer with proper sequence to avoid race conditions
    uart_print("Initializing timer...\r\n");
    
    // Make sure is_analyzing is false to prevent premature analysis
    is_analyzing = false;
    
    // Initialize with 1ms interval
    timer_init(1000);
    
    // Set the callback before enabling interrupts
    // If we don't, we end up with a race condition where the timer interrupt
    // might occur before the callback is set.
    timer_set_callback(timer_digit_analysis_callback);
    
    uart_print("Timer initialized\r\n");
    
    // Enable global interrupts - do this only once
    uart_print("Enabling global interrupts\r\n");
    __enable_irq();
    uart_print("Global interrupts enabled\r\n");
    
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
            
            // Store and echo the received character back
            buff[buff_index++] = (char)rx_char; // Store character in buffer
            uart_tx(rx_char); // Echo character back to terminal
            
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
    
    // Analyze the first digit immediately
    analyze_current_digit();
    
    // Enable the timer to handle subsequent digits
    // The callback was already set during initialization
    uart_print("Enabling timer...\r\n");
    timer_enable();
    
    // The timer will handle subsequent digits and completion
    uart_print("\r\nTimer-based analysis started\r\n");
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
    sprintf(message, "\r\nAnalyzing digit %u: %c (%s)\r\n", 
            current_digit_index + 1, digit, (digit_value % 2 == 0) ? "even" : "odd");
    uart_print(message);
    
    // We'll use the single timer for both digit analysis and LED blinking
    
    if (led_enabled) {
        if (digit_value % 2 == 0) {
            // Even digit: LED blinks continuously every 200ms
            uart_print("LED will blink continuously every 200ms\r\n");
            // Reset blink counter for consistent timing
            blink_counter = 0;
            // The actual blinking is handled in the timer callback
        } else {
            // Odd digit: LED toggles and stays steady
            bool new_state = !led_state;
            set_led(new_state);
            sprintf(message, "LED toggled to %s\r\n", new_state ? "ON" : "OFF");
            uart_print(message);
        }
    } else {
        uart_print("LED actions disabled (button pressed)\r\n");
    }
}

// Timer interrupt for digit analysis
void timer_digit_analysis_callback(void) {
    static uint32_t ms_counter = 0;
    
    // Only process timer callback if we're actually analyzing
    // This prevents the "Analysis complete" message at startup
    if (!is_analyzing) {
        return;
    }    
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
                // Message will be printed in the main loop
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
            // Blink continuously for even digits
            // Check if it's time to toggle the LED (every LED_BLINK_MS milliseconds)
            if (ms_counter % LED_BLINK_MS == 0) {
                // Toggle LED state
                led_state = !led_state;
                set_led(led_state);
                
                // Print LED state change with timestamp
                char message[32];
                sprintf(message, "[%u ms] LED %s\r\n", ms_counter, led_state ? "ON" : "OFF");
                uart_print(message);
            }
        }
    }
}

// This function is no longer needed as blinking is handled in the timer_digit_analysis_callback

// Button interrupt service routine
void button_isr() {
    // Increment button press counter
    button_press_count++;
    
    // If we're analyzing, toggle LED functionality
    if (is_analyzing) {
        led_enabled = !led_enabled;
        
        if (!led_enabled) {
            // Disable LED actions
            uart_print("\r\nLED locked. Button press count = ");
            char count_str[16];
            sprintf(count_str, "%u\r\n", button_press_count);
            uart_print(count_str);
        } else {
            // Re-enable LED actions
            uart_print("\r\nLED functionality restored. Button press count = ");
            char count_str[16];
            sprintf(count_str, "%u\r\n", button_press_count);
            uart_print(count_str);
            
            // Re-analyze current digit to restore LED behavior
            analyze_current_digit();
        }
    } else {
        // Just print the button press count if no analysis is in progress
        uart_print("\r\nButton pressed. Count = ");
        char count_str[16];
        sprintf(count_str, "%u\r\n", button_press_count);
        uart_print(count_str);
    }
}

// Set LED state
void set_led(bool state) {
    led_state = state;
    leds_set(state, 0, 0);
}

// Interrupt Service Routine for UART receive
void uart_rx_isr(uint8_t rx) {
    // Check if the received character is a printable ASCII character
    if (rx >= 0x20 && rx <= 0x7E || rx == '\r') { // Only accept printable chars and Enter
        // Store the received character
        queue_enqueue(&rx_queue, rx);
        
        // If we're currently analyzing, signal that new input is available as soon as any key is pressed
        // This allows immediate interruption of the analysis
        if (is_analyzing) {
            new_input_available = true;
            uart_print("\r\nNew input detected, stopping current analysis...\r\n");
        }
    }
}