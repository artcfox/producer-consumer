/* Name: main.c
 * Author: Matthew T. Pandina
 *
 * Copyright (c) 2010 Matthew T. Pandina
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name(s) of the above
 * copyright holders shall not be used in advertising or otherwise
 * to promote the sale, use or other dealings in this Software
 * without prior written authorization.
 */

#include <stdint.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdio.h>
#include <util/delay.h>
#include <stdlib.h>

// Note that this is only used for atomic calls to USART_TransmitString()
#include <util/atomic.h>

#define NELEMS(x) (sizeof(x)/sizeof((x)[0]))

// In this example our queue will hold RGB triplets
struct _RGB {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};
typedef struct _RGB RGB;

// It is a good idea to make your queue length be a power of 2
volatile RGB queue[128];

// No mutex necessary, since this is a uint8_t,
// and the ISR is the only place it is ever modified
volatile uint8_t head = 0;

// No mutex necessary, since this is a uint8_t,
// and main() is the only place it is ever modified
volatile uint8_t tail = 0;

// Initially start with the consumer disabled, since we want
// the queue to fill up before it starts consuming.
// Don't worry, the producer will enable the consumer as soon
// as it notices the queue is full
volatile uint8_t enable_consumer = 0;

// This allows us to manually increase the time between consumption
volatile uint8_t consume_every_modifier = 0;

// Returns 1 if the queue is empty, 0 otherwise
static inline uint8_t empty() {
	return (head == tail);
}

// Returns 1 if the queue is full, 0 otherwise
static inline uint8_t full() {
	return (head == (tail + 1) % NELEMS(queue));
}

// enqueue() should never be called on a full queue
// and should only be called by main()
static inline void enqueue(RGB *rgb) {
	queue[tail] = *rgb;
	tail = (tail + 1) % NELEMS(queue);
}

// dequeue() should never be called on an empty queue
// and should only be called by the ISR
static inline void dequeue(RGB *rgb) {
	*rgb = queue[head];
	head = (head + 1) % NELEMS(queue);
}

// Standard USART initialization, except we only enable the transmitter
static void USART_Init(void) {
	// Enable transmitter only
	UCSR0B |= (1 << TXEN0);
}

// Standard way to set the baud rate using avr-libc's helper 'bacros'
static void USART_115200(void) {
#undef BAUD  // avoid potential compiler warning
#define BAUD 115200
#include <util/setbaud.h>
	UBRR0H = UBRRH_VALUE;
	UBRR0L = UBRRL_VALUE;
#if USE_2X
	UCSR0A |= (1 << U2X0);
#else
	UCSR0A &= ~(1 << U2X0);
#endif
}

// Standard way to transmit a character over the USART
static void USART_Transmit(unsigned char data) {
	// Wait for empty transmit buffer
	while (!(UCSR0A & (1 << UDRE0)));
	
	// Put data into buffer, sends the data
	UDR0 = data;
}

// Standard way to transmist a string over the USART, though since strings are printed from
// both the ISR and main() This is wrapped in an ATOMIC_BLOCK, which will disable interrupts
// if enabled, print the string, and then restore interrupts if they were enabled. This is
// done solely to prevent the strings printed from the ISR and main() from getting mixed
// together in the middle of a line. If we did not print from both main() and the ISR, we
// would not have to disable interrupts!
static void USART_TransmitString(char *data) {
		while (*data)
			USART_Transmit(*data++);
}

// This sets up Timer 0 to be called every CLK_io / 256 / 256 cycles.
// The second / 256 is there because we are only called on 8-bit overflow.
static void Timer0_Init(void) {
	// Normal port operation, OC0A disconnected; Normal
	TCCR0A = 0;
	
	// CLK_io / 256 (3.556 ms period at 18.432MHz)
	TCCR0B |= (1 << CS02);
	
	// Enable overflow interrupt
	TIMSK0 |= (1 << TOIE0);
	
	// Enable global interrupts
	sei();
}

// Here is the routine that is called whenever timer 0 overflows
// Note that we can also use this interrupt for debouncing buttons, though depending on
// the prescale you choose, you might want to wait for multiple timer cycles to debounce,
// using the same trick, but a different cycle variable to count debouncing timer cycles
ISR(TIMER0_OVF_vect) {
	// Cycle counter for knowing when we should attempt a dequeue
	static uint8_t cycle = 0;
	
	// Normally the delays will be measured in clock cycles (due to actual code that main() is
	// running, not a random delay in ms), so once the optimal consume_every value is found
	// experimentally (be sure to exercise the long code paths) we can hard code that value
	// below so as to never drain the buffer completely. This lets us change the clock speed
	// without having to recompile or recalculate anything.
	
	// Timer cycles that need to pass before we dequeue. Set to 1 to auto-calibrate.
	static uint8_t consume_every = 1;
	
	// If enough timer cycles have passed to attempt a dequeue.
	// The >= is used in case consume_every_modifier is ever increased and then decreased
	if (enable_consumer && ++cycle >= (consume_every + consume_every_modifier)) {
		cycle = 0;				// reset the cycle counter

		char buf[128] = { 0 };	// just a buffer for our strings
		
		if (!empty()) {			// Is there something on the queue?
			RGB rgb;
			dequeue(&rgb);		// Hooray, let's see what it is!

			// Do something interesting with it...
			// Here we just stuff it into a string which will later be printed
			snprintf(buf, NELEMS(buf),
					 "<<<<< Consumed: (%d, %d, %d) consuming every: %d\n",
					 rgb.r, rgb.g, rgb.b, consume_every + consume_every_modifier);
		} else {
			// If we get here it means that we are consuming too fast
			consume_every++;		// wait an additional cycle next time
			enable_consumer = 0;	// wait for the queue to fill up before we try again
			
			// Complain that the queue was empty, and let us know what consume_every has
			// increased to. Once your code is stable, you'll want to remove this line
			// and probably hard code the maximum value you saw as the initial value for
			// consume_every above (instead of always starting at 1)
			snprintf(buf, NELEMS(buf),
					 "Queue is empty! Increased consume_every to: %d\n", 
					 consume_every);
		}
		
		// Output the string we prepared
		USART_TransmitString(buf);
	}
}

int main(void) {
	// Initialize the USART, set the baud rate
	USART_Init();
	USART_115200();
	USART_TransmitString("Producer/Consumer Example\n\n");
	
	// Configure and start the timer which is used to consume
	Timer0_Init();
	
	// Uncomment the following line to force the consumer to consume slower than its max rate.
	// In this example it is hard coded, but be creative, increase or decrease it with a button
	// press, or with a knob.
	
	//consume_every_modifier = 10;
	
	// herein lies the producer
	for (;;) {
		// Today we're producing RGB triplets!
		RGB rgb = { 0 };
		
		// Let's keep it simple and just make 3 nested loops
		// that will eventually loop over all 2^24 colors.
		do {
			do {
				do {
					// Inside here we have our RGB triplet
					while (full())				// stop producing if the queue is full
						enable_consumer = 1;	// enable the consumer when the queue is full

					enqueue(&rgb);				// copy our color onto the queue!
					
					// If you want to see when we are producing an RGB triplet, uncomment
					// the following block of code. If we did not wrap the call to
					// USART_TransmitString() in an ATOMIC_BLOCK, the ISR could interrupt
					// us in the middle of printing a string, which would make the output
					// hard to read.

					/*
					char buf[128] = { 0 };
					snprintf(buf, NELEMS(buf),
							 ">>>>> Produced: (%d, %d, %d)\n",
							 rgb.r, rgb.g, rgb.b);
					
					ATOMIC_BLOCK(ATOMIC_FORCEON) {
						USART_TransmitString(buf);
					}
					*/
					
					// Choose a random delay between 0 and 15 ms. This will simulate different
					// code paths, or operations that take a different amount of time to execute.
					// Even though we are producing these colors at a non-uniform rate, the
					// consumer will consume them at a steady rate that can be faster than the
					// maximum delay between individual colors, since they are buffered in the
					// queue. Note that if the consumer is configured to auto-calibrate its rate,
					// it might pause and increase its buffer a few times before the consumption
					// rate becomes steady.
					uint8_t delay_in_ms = random() % 16;
					while (delay_in_ms--)
						_delay_ms(1); // this avoids pulling in floating point code
					
				} while (++rgb.b != 0);
			} while (++rgb.g != 0);
		} while (++rgb.r != 0);		
	}
	
	return 0;
}
