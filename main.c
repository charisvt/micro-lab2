#include "main.h"
#include <stdio.h>
#include <string.h>

// Definitions
#define BUFF_SIZE 128
#define BUTTON_PIN PC_13
#define DIGIT_ANALYSIS_INTERVAL_MS 500
#define LED_BLINK_INTERVAL_MS 200

// Application States
typedef enum {
    APP_STATE_INIT,
    APP_STATE_IDLE,
    APP_STATE_RECEIVING_INPUT,
    APP_STATE_START_ANALYSIS,
    APP_STATE_ANALYZING_DIGIT,
    APP_STATE_CONTINUOUS_BLINK 
} AppState;

// Global state variables
static AppState current_app_state = APP_STATE_INIT;
static bool continuous_mode_active = false;
static bool lock_message_was_printed = false;

// Input Buffers
static Queue rx_queue;
static char input_buffer[BUFF_SIZE];
static uint8_t input_buffer_idx = 0;
static char processed_number[BUFF_SIZE];
static uint8_t processed_number_len = 0;
static uint8_t current_digit_idx = 0;

// LED & Button Status
static bool led_current_state_on = false;
static bool led_should_blink = false;
static uint32_t button_press_counter = 0;
static bool led_frozen = false;

// Timer Counters
static volatile uint32_t system_ms_counter = 0; // Incremented by 1ms timer ISR
static uint32_t last_digit_analysis_time = 0;
static uint32_t last_led_blink_time = 0;

// Event Flags (set by ISRs, cleared by main loop)
static volatile bool uart_char_received_flag = false;
static volatile uint8_t received_uart_char = 0;
static volatile bool button_pressed_flag = false;
static volatile bool new_input_interrupt_flag = false; // If new input during analysis

// Function Prototypes for State Handlers
static void handle_init_state(void);
static void handle_idle_state(void);
static void handle_receiving_input_state(void);
static void handle_start_analysis_state(void);
static void handle_analyzing_digit_state(void);
static void handle_continuous_blink_state(void);

// Helper Functions
static void set_led_output(bool on);
static void process_received_char(uint8_t c);
static void filter_and_prepare_number(void);
static void initiate_digit_analysis(void);
static void perform_current_digit_analysis(void);
static void reset_for_new_input(void);

// ISRs
void timer_1ms_callback(void) { // Assuming a 1ms timer is configured
    system_ms_counter++;
    // Flags for specific intervals can be set here if needed, or checked in main loop
}

void uart_rx_isr(uint8_t rx_data) {
    if (queue_is_full(&rx_queue)) { // Or however you check queue full
        // Optional: Handle queue full error, maybe log or ignore
        return;
    }
    queue_enqueue(&rx_queue, rx_data); // Enqueue the character
    uart_char_received_flag = true;    // Signal main loop

    // If analysis or blinking is active, new UART input is an interruption
    if (current_app_state == APP_STATE_ANALYZING_DIGIT || 
        current_app_state == APP_STATE_CONTINUOUS_BLINK) {
        new_input_interrupt_flag = true;
    }
}

void button_isr(int status) {
    button_pressed_flag = true;
}

int main(void) {
    
    // Initialize Peripherals
    queue_init(&rx_queue, 128); // Initialize RX queue
    uart_init(115200);          // Initialize UART
    uart_set_rx_callback(uart_rx_isr);
    uart_enable();

    leds_init(); // Initialize LEDs

    gpio_set_mode(BUTTON_PIN, PullUp);      // Button with pull-up
    gpio_set_trigger(BUTTON_PIN, Rising);  // Trigger on rising edge or we get weird bug
    gpio_set_callback(BUTTON_PIN, button_isr);
    
    NVIC_SetPriority(EXTI15_10_IRQn, 0); 

    // Initialize a 1ms system timer
    timer_init(1000); // 1000us = 1ms interval
    timer_set_callback(timer_1ms_callback);
    timer_enable();

    __enable_irq(); // Enable global interrupts

    current_app_state = APP_STATE_INIT;

    while (1) {
        if (new_input_interrupt_flag) {
            uart_print("\r\nAnalysis interrupted by new input.\r\n");
            uart_print("Enter number:");
            led_should_blink = false;
            reset_for_new_input();
            set_led_output(false); // Explicitly turn LED off on interrupt
            current_app_state = APP_STATE_IDLE;
            new_input_interrupt_flag = false;
            uint8_t temp_val;
            while(queue_dequeue(&rx_queue, &temp_val)); // Clear queue
            uart_char_received_flag = false; 
        }

        if (button_pressed_flag) {
            button_press_counter++;
            led_frozen = !led_frozen; // Toggle frozen state

            if (led_frozen) {
                uart_print("\r\nButton Press: LED functionality LOCKED. Press count: ");
            } else {
                uart_print("\r\nButton Press: LED functionality RESTORED. Press count: ");
                // When unlocking, immediately apply the current logical LED state
                // to the physical LED. set_led_output will now allow leds_set().
                set_led_output(led_current_state_on);
            }
            char temp_str[12];
            sprintf(temp_str, "%lu\r\n", button_press_counter);
            uart_print(temp_str);
            button_pressed_flag = false;
        }

        // --- State Machine Execution ---
        switch (current_app_state) { // Switch directly on current_app_state
            case APP_STATE_INIT:
                handle_init_state();
                break;
            case APP_STATE_IDLE:
                handle_idle_state();
                break;
            case APP_STATE_RECEIVING_INPUT:
                handle_receiving_input_state();
                break;
            case APP_STATE_START_ANALYSIS:
                handle_start_analysis_state();
                break;
            case APP_STATE_ANALYZING_DIGIT:
                handle_analyzing_digit_state();
                break;
            case APP_STATE_CONTINUOUS_BLINK:
                handle_continuous_blink_state();
                break;
            default:
                // Should not happen, reset to a safe state
                current_app_state = APP_STATE_IDLE;
                break;
        }
        //__WFI();
    }
}

void handle_init_state(void) {
    uart_print("\r\n*** Digit Analysis System ***\r\n");
    reset_for_new_input();
    set_led_output(false); // Explicitly turn LED off during system init
    current_app_state = APP_STATE_IDLE;
}

void handle_idle_state(void) {
    // Check for new character from UART to start receiving input
    // Only print prompt once when entering IDLE from a state that's not INIT (which prints its own welcome)
    static AppState last_state_before_idle = APP_STATE_INIT;
    if (last_state_before_idle != APP_STATE_IDLE && current_app_state == APP_STATE_IDLE) {
        uart_print("Enter number: ");
    }
    last_state_before_idle = current_app_state; // Update for next cycle

    if (uart_char_received_flag) {
        current_app_state = APP_STATE_RECEIVING_INPUT;
        // LED state is preserved from previous operation unless explicitly changed
    }
}

void handle_receiving_input_state(void) {
    if (uart_char_received_flag) {
        uint8_t c;
        if (queue_dequeue(&rx_queue, &c)) {
            process_received_char(c); // Echoes and adds to buffer
        }
        uart_char_received_flag = false; // Consume the flag

        if (c == '\r' || input_buffer_idx >= BUFF_SIZE -1) {
            uart_print("\r\n");
            filter_and_prepare_number();
            if (processed_number_len > 0) {
                current_app_state = APP_STATE_START_ANALYSIS;
            } else {
                uart_print("No valid digits entered.\r\n");
                reset_for_new_input();
                current_app_state = APP_STATE_IDLE; // Back to idle to re-prompt
            }
        }
    }
}

void handle_start_analysis_state(void) {
    uart_print("Starting analysis...\r\n");
    initiate_digit_analysis(); // Sets up current_digit_idx, calls perform_current_digit_analysis for first digit
                               // and enables timer if needed.
    current_app_state = APP_STATE_ANALYZING_DIGIT;
    last_digit_analysis_time = system_ms_counter;
    last_led_blink_time = system_ms_counter;
}

void handle_analyzing_digit_state(void) {
    // Check for digit analysis interval
    if ((system_ms_counter - last_digit_analysis_time) >= DIGIT_ANALYSIS_INTERVAL_MS) {
        current_digit_idx++;
        if (current_digit_idx < processed_number_len) {
            perform_current_digit_analysis();
            last_digit_analysis_time = system_ms_counter;
            last_led_blink_time = system_ms_counter; // Reset blink time for new digit
        } else {
            // Analysis complete
            uart_print("Analysis complete. \r\n");
            if (continuous_mode_active) {
                uart_print("Continuous mode: Restarting analysis.\r\n");

                current_digit_idx = 0; // Reset for re-analysis

                current_app_state = APP_STATE_START_ANALYSIS; 
            } else if (led_should_blink) { 
                current_app_state = APP_STATE_CONTINUOUS_BLINK;
                 uart_print("Continuous LED blinking.\r\n");
            } else {
                // Analysis of a non-continuous, non-blinking number is complete.
                // LED should remain in the state set by the last odd digit.
                timer_disable(); // Stop the SysTick timer if it's only for analysis/blinking
                // set_led_output(led_current_state_on); // LED is already in its final state from perform_current_digit_analysis
                
                // Reset necessary flags and buffers for the next input cycle, but preserve LED state.
                input_buffer_idx = 0;
                input_buffer[0] = '\0';
                processed_number_len = 0;
                processed_number[0] = '\0';
                current_digit_idx = 0; 
                // led_should_blink is already false
                // continuous_mode_active is already false
                // uart_char_received_flag and new_input_interrupt_flag will be handled by their respective logic.

                current_app_state = APP_STATE_IDLE;
                uart_print("Enter number:");
            }
            return; // Exit to avoid immediate blink check
        }
    }

    // Handle LED blinking if current digit is even
    if (led_should_blink) {
        if ((system_ms_counter - last_led_blink_time) >= LED_BLINK_INTERVAL_MS) {
            led_current_state_on = !led_current_state_on;
            set_led_output(led_current_state_on);
            last_led_blink_time = system_ms_counter;
        }
    }
}

void handle_continuous_blink_state(void) {
    // This state is active when analysis is done, and last digit was even.
    // UART or Button ISRs can interrupt this state.
    if (led_should_blink) { // Should always be true here
         if ((system_ms_counter - last_led_blink_time) >= LED_BLINK_INTERVAL_MS) {
            led_current_state_on = !led_current_state_on;
            set_led_output(led_current_state_on);
            last_led_blink_time = system_ms_counter;
        }
    } else {
        // Should not happen, safety check
        timer_disable();
        set_led_output(false);
        reset_for_new_input();
        current_app_state = APP_STATE_IDLE;
    }
}

// --- Helper Function Implementations ---
void set_led_output(bool on) {
    led_current_state_on = on; // Always update logical state
    if (!led_frozen) {    // Check if LED is NOT frozen
        leds_set(led_current_state_on, 0, 0);
    }
}

void process_received_char(uint8_t c) {
    if (c == '\b' || c == 0x7F) { // Handle backspace (ASCII DEL for some terminals)
        if (input_buffer_idx > 0) {
            input_buffer_idx--;
            uart_print("\b \b"); // Erase character on terminal
        }
    } else if (c >= 0x20 && c < 0x7F) { // Printable characters (excluding DEL)
        if (input_buffer_idx < BUFF_SIZE - 1) {
            input_buffer[input_buffer_idx++] = c;
            uart_tx(c); // Echo character
        }
    } else if (c == '\r') { // Enter key
        // Handled by main logic in RECEIVING_INPUT state
        // uart_tx(c); // Echo CR
        // uart_tx('\n'); // Echo LF
    }
    input_buffer[input_buffer_idx] = '\0'; // Null-terminate for safety
}

void filter_and_prepare_number(void) {
    processed_number_len = 0;
    continuous_mode_active = false; // Reset for current input processing

    for (uint8_t i = 0; i < input_buffer_idx; ++i) {
        // Manual check for digit instead of isdigit()
        if (input_buffer[i] >= '0' && input_buffer[i] <= '9') {
            if(processed_number_len < BUFF_SIZE -1){
                 processed_number[processed_number_len++] = input_buffer[i];
            }
        }
        // Check for trailing '-' for continuous mode
        if (input_buffer[i] == '-' && i == (input_buffer_idx - 1) && processed_number_len > 0) {
             continuous_mode_active = true;
             uart_print("Continuous mode detected ('-').\r\n");
             // Don't add '-' to processed_number
             break; // Stop processing once '-' is found at the end
        }
    }
    processed_number[processed_number_len] = '\0';
}

void initiate_digit_analysis(void) {
    current_digit_idx = 0;
    led_should_blink = false; // Reset before first digit analysis

    if (processed_number_len > 0) {
        perform_current_digit_analysis(); // Analyze the first digit
        timer_enable(); // Ensure timer is running for subsequent digits/blinking
    } else {
        reset_for_new_input();
        current_app_state = APP_STATE_IDLE;
    }
}

void perform_current_digit_analysis(void) {
    if (current_digit_idx >= processed_number_len) return; // Should be caught earlier

    char digit_char = processed_number[current_digit_idx];
    int digit = digit_char - '0';

    char msg[30];
    sprintf(msg, "Analyzing digit %c (%d)...\r\n", digit_char, digit);
    uart_print(msg);

    if (digit % 2 == 0) { // Even digit
        uart_print("Even digit - LED will blink.\r\n");
        led_should_blink = true;
        led_current_state_on = true; // Start by turning LED on for blink
        set_led_output(led_current_state_on);
    } else { // Odd digit
        uart_print("Odd digit - LED will toggle and stay.\r\n");
        led_should_blink = false;
        led_current_state_on = !led_current_state_on; // Toggle previous state
        set_led_output(led_current_state_on);
    }
}

void reset_for_new_input(void) {
    input_buffer_idx = 0;
    input_buffer[0] = '\0';
    processed_number_len = 0;
    processed_number[0] = '\0';
    current_digit_idx = 0;
    led_should_blink = false;
    continuous_mode_active = false; // Ensure continuous mode is reset
    uart_char_received_flag = false;
    new_input_interrupt_flag = false;
    uint8_t temp_char;
    while(queue_dequeue(&rx_queue, &temp_char));
}