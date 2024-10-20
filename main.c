#include <bcc/bcc.h>
#include <bcc/bcc_param.h>
#include <drv/grspw.h>
//#include <stdio.h>
#include <string.h>

//#include "CrFwTime.h" // CrFwGetCurrentTime()

//#include "AppStartUp/CrFwAppSm.h"
//#include "app/AibApp.h"

#define RX_PACKETS 16
#define TX_PACKETS 16
#define MAX_PACKET_SIZE 32




#define JUMP_ADDR 0x40011000
#define JUMP_TO_APP()  {((void (*)()) (*(uint32_t*)JUMP_ADDR)) ();}

#define RAM_ADDRESS 0x40011000

//struct osal_isr_ctx isr_ctx;

int configure_grspw (void);
void perform_spw_cycle (void);
void spw_isr (struct grspw_priv *priv, void *data);

int handle_packet (struct grspw_pkt *rx_packet);
int send_packet (uint8_t *data, size_t len);
static struct grspw_dma_priv *dma_channel;
static struct grspw_list rx_list;
static struct grspw_list tx_list;

/*
int __bcc_con_init(void) {
	// TODO: I'm not sure this address is correct
	__bcc_con_handle = 0x80000100; // 0xff900000;
	return 0;
}

void __bcc_init70 (void) {
	// TODO: What does the exit() function do in this context?
	if (bcc_timer_tick_init_period(10000) != BCC_OK) {
		exit(EXIT_FAILURE);
	}
}

int __bcc_int_init(void) {
	__bcc_int_handle = 0x80000200; // This address is for E698PM. Use 0xff904000 on GR740 (the default)
	__bcc_int_irqmp_eirq = 11; // This interrupt number is for E698PM. Use 10 on GR740 (the default)
	return 0;
}

int __bcc_timer_init(void) {
	__bcc_timer_handle = 0x80000300; // This address is for E698PM. Use 0xff908000 on GR740 (the default)
	__bcc_timer_interrupt = 6; // This interrupt number is for E698PM. Use 1 on GR740 (the default)
	return 0;
}
*/

int configure_grspw (void) {
	struct grspw_priv *spw_device;
	//struct grspw_dma_priv *dma_channel; global
	uint32_t linkcfg, clkdiv;
	//struct grspw_list lst;

	grspw_autoinit();

	spw_device = grspw_open(0);
	if (!spw_device)
		return -1; /* Failure */

	/* Start Link */
	linkcfg = LINKOPTS_ENABLE | LINKOPTS_START; /* Start Link */
	grspw_set_linkcfg(spw_device, linkcfg);
	//clkdiv = (9 << 8) | 9; /* Clock Divisor factor of 10 */
	//clkdiv = (19 << 8) | 19;
	//clkdiv = (77u << 8) | 77u; // Clock divisor of 78 -- results in a speed of 2.4 Mbits/s
	clkdiv = (78u << 8) | 78u; // Clock divisor of 79
	grspw_set_clkdiv(spw_device, clkdiv);

	const struct grspw_addr_config addr_cfg = {
		.promiscuous = 1, // Ignore address field and put all received packets to first DMA channel
		.def_addr = 10, // AIB SpaceWire node address
		// .def_mask = 0,
		// .dma_nacfg
	};
	grspw_addr_ctrl(spw_device, &addr_cfg);
	
	// Enable time-code reception and interrupt generation when time-codes are received
	grspw_set_tccfg(spw_device, TCOPTS_EN_RX | TCOPTS_EN_RXIRQ);
	
	// Use SpaceWire port 0. This call is technically unnecessary
	int port = 0;
	grspw_port_ctrl(spw_device, &port);
	
	// TODO: Setup RMAP (these two calls should be sufficient)
	//grspw_rmap_set_destkey(spw_device, 11); // TODO: Investigate
	//grspw_rmap_set_ctrl(spw_device, RMAPOPTS_EN_RMAP | RMAPOPTS_EN_BUF); // TODO: What do these flags do?
	
	/* wait until link is in run-state */
	spw_link_state_t state;
	do {
		state = grspw_link_state(spw_device);
	} while (state != SPW_LS_RUN);

	/* Open DMA channel */
	dma_channel = grspw_dma_open(spw_device, 0);
	if (!dma_channel) {
		grspw_close(spw_device);
		return -2;
	}

	// TODO: Is this call in the right place in the init sequence of calls?
	// TODO: Write the ISR.
	// Note that grspw_set_isr() isn't in the manual
	grspw_set_isr(spw_device, spw_isr, dma_channel); // Note: dma_channel is user data, just passed to the ISR

	// Start the DMA channel
	if (DRV_OK != grspw_dma_start(dma_channel)) {
		grspw_dma_close(dma_channel);
		grspw_close(spw_device);
		return -3;
	}

	/* ... */
	
	//preinited_rx_unused_buf_list0 // FIXME
	//preinited_tx_send_buf_list // FIXME
	
	// TODO: Make this global
	//static struct grspw_list rx_list; global
	//static struct grspw_list tx_list; global
	static struct grspw_pkt rx_packets[RX_PACKETS];
	static struct grspw_pkt tx_packets[TX_PACKETS];
	static char rx_buffers[RX_PACKETS][MAX_PACKET_SIZE]; // Note: on some systems the data buffers must be 32-bit word-aligned for reception
	static char tx_buffers[TX_PACKETS][MAX_PACKET_SIZE];
	
	grspw_list_clr(&rx_list);
	grspw_list_clr(&tx_list);
	
	for (int i = 0; i < RX_PACKETS; ++i) {
		rx_packets[i] = (struct grspw_pkt) {
			.data = rx_buffers[i],
			.dlen = MAX_PACKET_SIZE,
		};
		grspw_list_append(&rx_list, &rx_packets[i]);
	}
	
	for (int i = 0; i < TX_PACKETS; ++i) {
		tx_packets[i] = (struct grspw_pkt) {
			.data = tx_buffers[i],
			.dlen = MAX_PACKET_SIZE,
		};
		grspw_list_append(&tx_list, &tx_packets[i]);
	}

	/* Prepare driver with RX buffers */
	grspw_dma_rx_prepare(dma_channel, &rx_list);


	return 0;	

	#if 0

	/* Start sending a number of SpaceWire packets */
	//grspw_dma_tx_send(dma_channel, 1, &preinited_tx_send_buf_list); // FIXME

	/* Receive at least one packet */
	int count;
	do {
		/* Try to receive as many packets as possible */
		count = grspw_dma_rx_recv(dma_channel, &rx_list);
	} while (0 == count);

	/*
	if (-1 == count) {
		printf("GRSPW0.dma_channel: Receive error\n");
	} else {
		printf("GRSPW0.dma_channel: Received %d packets\n", count);
	}
	*/

	/* ... */

	grspw_dma_close(dma_channel);
	grspw_close(spw_device);
	return 0; /* success */
	
	#endif
}


int main (void) {
	configure_grspw();
	uint32_t current_time; // Current time in microseconds
	uint32_t cycle_number;
	uint32_t cycle_expiration = 0;

	for (;;) {
		do {
			current_time = bcc_timer_get_us();
		} while (current_time < cycle_expiration);
		
		// One cycle every 10 ms
		//cycle_number = current_time / 10000;
		//cycle_expiration = (cycle_number + 1) * 10000;
		
		// One cycle every second
		cycle_number = current_time / 1000000;
		cycle_expiration = (cycle_number + 1) * 1000000;
		
		
		send_packet((uint8_t*) &cycle_number, sizeof cycle_number);
		perform_spw_cycle();
		unsigned int return_address;
 		unsigned int * const RAM_pointer = (unsigned int *) RAM_ADDRESS;
//		printf("RAM pointer set to: 0x%08x \n",(unsigned int)RAM_pointer);
//		printf("jumping...\n");

		 __asm__(" nop;" //clean the pipeline
         		"jmp %1;" // jmp to programB
       			:"=r" (return_address)
       			:"r" (RAM_pointer)
     			);
	}
	return 0;
}

void perform_spw_cycle (void) {
	int recv_count;
	int reclaim_count;

	int prepare_count;
	int send_count;

	// Move received packets from RX SCHED to RX USER list
	recv_count = grspw_dma_rx_recv(dma_channel, &rx_list);

	// Move sent packets from TX SCHED to TX USER list
	reclaim_count = grspw_dma_tx_reclaim(dma_channel, &tx_list);

	// Handle each received packet
	for (struct grspw_pkt *p = rx_list.head; p != NULL; p = p->next) {
		send_count += handle_packet(p);
	}

	//
	prepare_count = grspw_dma_rx_prepare(dma_channel, &rx_list);

	(void) recv_count;
	(void) reclaim_count;
	(void) send_count;
	(void) prepare_count;

	//send_packet("cycle", 5);

	return;
}

/*
int handle_packet (struct grspw_pkt *rx_packet) {
	int ret = 0;

	// List for holding the response packet
	struct grspw_list tx_staging;
	grspw_list_clr(&tx_staging);

	// Packet structure for holding the response
	struct grspw_pkt *tx_packet = grspw_list_remove_head(&tx_list);
	grspw_list_append(&tx_staging, tx_packet);

	if (rx_packet->dlen <= MAX_PACKET_SIZE - 2) {
		// Configure response packet
		tx_packet->flags = 0;
		tx_packet->dlen = rx_packet->dlen + 2;

		// Put a known byte at the beginning and end of the response
		((uint8_t*) tx_packet->data)[0] = 0x01;
		((uint8_t*) tx_packet->data)[tx_packet->dlen - 1] = 0x02;

		// Copy the received packet buffer to the response packet
		memcpy(&((uint8_t*) tx_packet->data)[1], rx_packet->data, rx_packet->dlen);

		ret = grspw_dma_tx_send(dma_channel, &tx_staging);
	}
	return ret;
}*/

int handle_packet (struct grspw_pkt *rx_packet) {
	int ret = 0;

	if (rx_packet->dlen <= MAX_PACKET_SIZE - 2) {
		uint8_t buf[MAX_PACKET_SIZE];
		size_t len = rx_packet->dlen + 2;

		// Put 0x01 at beginning and 0x02 end of response
		buf[0] = 0x01;
		buf[len - 1] = 0x02;
		
		// Echo the received packet
		memcpy(&buf[1], rx_packet->data, rx_packet->dlen);

		send_packet(buf, len);
		
		//uint32_t the_time = bcc_timer_get_us();
		//send_packet(&the_time, sizeof the_time);
	}
	return ret;
}

int send_packet (uint8_t *data, size_t len) {
	int ret = 0;

	// List for holding the packet
	struct grspw_list tx_staging;
	grspw_list_clr(&tx_staging);

	// Struct for holding the packet
	struct grspw_pkt *tx_packet = grspw_list_remove_head(&tx_list);
	grspw_list_append(&tx_staging, tx_packet);
	
	// Truncate data if it is too long
	if (len > MAX_PACKET_SIZE) {
		len = MAX_PACKET_SIZE;
	}
	
	// Configure the packet
	tx_packet->flags = 0;
	tx_packet->dlen = 12;
	
	unsigned char helloWorld[10] = {"HelloWorld"};
	// Copy the packet data to its buffer
	memcpy(tx_packet->data, helloWorld, 12);
	
	ret = grspw_dma_tx_send(dma_channel, &tx_staging);
	return ret;
}

void spw_isr (struct grspw_priv *spw_device, void *data) {
	(void) spw_device;
	(void) data;

	return;
}





