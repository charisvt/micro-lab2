#include "platform.h"
#include <stdio.h>
#include <stdint.h>
#include "uart.h"
#include <string.h>
#include "queue.h"

#define BUFF_SIZE 128 //read buffer length 

Queue rx_queue; // Queue for storing received characters
void uart_rx_isr(uint8_t rx);

int main(){
		uint8_t rx_char = 0;
		char buff[BUFF_SIZE];
		uint32_t buff_index;

		// Init receive queue and UART
		queue_init(&rx_queue, 128);
		uart_init(115200);
		uart_set_rx_callback(uart_rx_isr); // Set the UART receive callback function
		uart_enable(); // Enable UART mode
	
		__enable_irq(); // Enable interrupts
		uart_print("\r\n"); // Print newline
	
}


// Interrupt Service Routine for UART receive
void uart_rx_isr(uint8_t rx) {
	// Check if the received character is a printable ASCII character
	if (rx >= 0x0 && rx <= 0x7F ) {
		// Store the received character
		queue_enqueue(&rx_queue, rx);
	}
}