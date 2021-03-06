/*
 
 ds1631_packet_demo.c
 --------------------
 
 Demonstration project showing packet communication routines
 with temperature sensing

 Ian Harris 2008
 imharris [at] gmail.com

 Released under the "do whatever you like with this
 but if it breaks, you get to keep both pieces" license.

*/


// System includes

#include <system.h>
#include <string.h>
#include <stdlib.h>
#include <eeprom.h>

// Local includes
#include "pic_timer.h"
#include "pic_packet.h"
#include "pic_serial.h"
#include "config.h"
#include "pic_tick.h"
#include "pic_utils.h"
#include "protocol.h"

//#include "pic_rf_24l01.h"
#include "pic_rf_2401a.h"
#include "ds1631.h"

// Local defines
#ifdef PLATFORM_TYPE == SFE_TDN_V1
	#define stat1 1
	#define stat2 3
	#define stat3 4
#endif

#define DS1631_ADDR 0x00
// Define to take out some serial_print_str statements
#define USE_LESS_FLASH
// Number of ticks to flash led for
#define LED_FLASH_TIME 200
// Global variables

// read_buffer for serial commands
char read_buffer[6]= '\0';
uns8 read_buffer_pos = 0;

// buffers for rf packet receives - here we go two deep
uns8 rf_rx_buffer [RF_RX_BUFFER_SIZE];
uns8 rf_rx_buffer2[RF_RX_BUFFER_SIZE];

// and here we define how many we have
#define MAX_PACKET_QUEUE 2

// do we have a task to do?
uns8 do_task = 0;

// task defines

#define TASK_SEND_TEMP			1
#define TASK_SEND_PACKET 		2
#define TASK_START_CONFIG_MODE 	3
#define TASK_END_CONFIG_MODE 	4

// which address should I send something to?
uns8 send_to_addr = 0;
// did I drop a packet?
uns8 dropped_packet = 0;
// are we in config mode?
uns8 config_mode = 0;
// how many packets have we received?
uns8 pkt_receive_level = 0;

uns8 temp_requested = 0;


	
typedef enum {
	rx_led,
	tx_led,
	fwd_led
} status_led;	

uns8  rx_led_on, tx_led_on, fwd_led_on;
uns16 rx_led_tick, tx_led_tick, fwd_led_tick;

void flash_led(status_led led) {
#ifdef PLATFORM_TYPE == SFE_TDN_V1

uns8 pin;
uns16 now_tick;

	now_tick = tick_get_count();
	
	switch (led) {
		case rx_led: 
			pin = stat1;
			rx_led_tick = now_tick;
			rx_led_on = 1;
			break;
		case tx_led: 
			pin = stat2;
			tx_led_tick = now_tick;
			tx_led_on = 1;
			break;
		case fwd_led: 
			pin = stat3;
			fwd_led_tick = now_tick;
			fwd_led_on = 1;
			break;
	}
	set_pin_var(PORTB, pin);
#endif
}

void process_leds() {
#ifdef PLATFORM_TYPE == SFE_TDN_V1
uns16 now_tick;

	now_tick = tick_get_count();
	if (rx_led_on) {
		if (tick_calc_diff(rx_led_tick, now_tick) > LED_FLASH_TIME) {
			clear_pin(PORTB, stat1);
			rx_led_on = 0;
		}
	}		
	if (tx_led_on) {
		if (tick_calc_diff(tx_led_tick, now_tick) > LED_FLASH_TIME) {
			clear_pin(PORTB, stat2);
			tx_led_on = 0;
		}
	}		
	if (fwd_led_on) {
		if (tick_calc_diff(fwd_led_tick, now_tick) > LED_FLASH_TIME) {
			clear_pin(PORTB, stat3);
			fwd_led_on = 0;
		}
	}		

#endif
}

void timer_0_callback() {
	handle_tick();
}	


void interrupt( void ) {

	timer_handle_0_isr();

	if ( test_bit(intcon, INTF) ) { 
		if (pkt_receive_level < MAX_PACKET_QUEUE) {	// if we're not already processing a receive
			if (pkt_receive_level == 0) {
				pic_rf_receive(rf_rx_buffer, PKT_PACKET_SIZE);
			} else {
				pic_rf_receive(rf_rx_buffer2, PKT_PACKET_SIZE);
			}
			pkt_receive_level++;
		} else {
			dropped_packet++;
		}	
		clear_bit( intcon, INTF);
	}	// pkt available interrupt

	serial_handle_tx_isr();
	serial_handle_rx_isr();
	
}




void pkt_payload_rx_callback(uns16 source_addr, uns16 pkt_id, uns8 *payload) {

uns8 count;
uns8 my_payload[PKT_PAYLOAD_SIZE];

	#ifndef USE_LESS_FLASH
	serial_print_str("<<s: ");
	serial_print_int_hex(source_addr);
	serial_print_str("p: ");
	for (count = 0; count < PKT_PAYLOAD_SIZE; count++) {
		serial_print_int_hex(payload[count]);
		serial_putc(' ');
	}	
	serial_print_str(">> ");
	#endif
	
	if ((payload[0] == PL_REQ_SENSOR) &&
	    (test_bit(payload[1], CAPS_SENSOR_TEMP))) {
	    	serial_putc('T');
	    	do_task = TASK_SEND_TEMP;
	    	send_to_addr = source_addr;
	    }

} 

// not used in this demo
void pkt_send_failed_callback(uns16 dest_addr, uns16 pkt_id) {
	serial_print_str("<S bad to ");
	serial_print_int_hex_16bit(dest_addr);
	serial_print_str(" id ");
	serial_print_int_hex_16bit(pkt_id);
	serial_print_str(">\n");
}

// not used in this demo
void pkt_send_succeeded_callback(uns16 dest_addr, uns16 pkt_id) {
	serial_print_str("<S good to ");
	serial_print_int_hex_16bit(dest_addr);
	serial_print_str(" id ");
	serial_print_int_hex_16bit(pkt_id);
	serial_print_str("> ");
}

void pkt_send_callback(uns16 dest_addr, uns16 pkt_id) {
	#ifndef USE_LESS_FLASH
		serial_print_str("<S to ");
		serial_print_int_hex_16bit(dest_addr);
		serial_print_str(" id ");
		serial_print_int_hex_16bit(pkt_id);
		serial_print_str("> ");
	#endif	
	flash_led(tx_led);
}



void process_rx() {
	uns8 len, rec;
	uns8 first_char;

	rec = serial_getc();
	
	serial_putc(rec);

	// neat trick to allow one button downloads
	if (rec == MAGIC_BOOSTBLOADER_REQUEST) {
		boostbloader();
	}	
	
	if (rec == '\r') {
		first_char = read_buffer[0];
		switch (first_char) {
		    case 'x':
				do_task = TASK_SEND_PACKET;
				break;
			case 'c':
				do_task = TASK_START_CONFIG_MODE;
				break;
			case 'n':
				do_task = TASK_END_CONFIG_MODE;
				break;
			default:
				if ((first_char == 's') && (read_buffer_pos == 3)) {
					send_to_addr = atoui_hex(&read_buffer[1]);
					serial_print_str(" S: ");
					serial_print_int(send_to_addr);
				} else if ((first_char == 'w') && (read_buffer_pos==5)) {
					uns16 eeprom = atoui_hex(&read_buffer[1]);
					uns8 e_data, e_addr;
					serial_print_str(" E:");
					e_addr = eeprom >> 8;
					serial_print_int(e_addr);
					serial_print_str(" = ");
					e_data = eeprom & 0xff;
					serial_print_int(e_data);
					eeprom_write(e_addr, e_data);
				}
		}		
		read_buffer[0] = '\0';
		read_buffer_pos = 0;
        serial_print_nl();
        serial_putc('>');			
	} else {
		read_buffer[read_buffer_pos] = rec;
		read_buffer_pos++;
		read_buffer[read_buffer_pos] = '\0';
	}	

}



// Setup the rf reception

void configure_rf() {
	rf_config my_config;
	
	my_config.payload_width_ch2 = 0;
	my_config.payload_width_ch1 = PKT_PACKET_SIZE * 8;
	// ignore ch2 address, since we don't use it
	my_config.address_ch2[0] = 0xf0;
	my_config.address_ch2[1] = 0xf0;
	my_config.address_ch2[2] = 0xf0;
	my_config.address_ch2[3] = 0xf0;
	my_config.address_ch2[4] = 0xf0;

	my_config.address_ch1[0] = 0b11100111; // addr starts here
	my_config.address_ch1[1] = 0b11100111;
	my_config.address_ch1[2] = 0b11100111;
	my_config.address_ch1[3] = 0b11100111; // only used three but fill 
	my_config.address_ch1[4] = 0b11100111; // ...for the fun of it
	my_config.address_width = 24; // (6 bits valid)
	set_bit(my_config.options, OP_LONG_CRC);
	set_bit(my_config.options, OP_ENABLE_CRC);
    clear_bit(my_config.options, OP_ENABLE_CH2);
	set_bit(my_config.options, OP_ENABLE_SHOCKBURST); 
	set_bit(my_config.options, OP_ENABLE_1_MBPS);
	my_config.crystal = 0b011; 	// (3 bits valid) -> 16Mhz
	my_config.output_power = 0b11; 	// (2 bits valid) 11 -> max power!
	my_config.channel = 2; 	// (7 bits valid);
	set_bit(my_config.options, OP_ENABLE_RECEIVE);

	pic_rf_init(&my_config);
}


void configure_rf_quick() {
  pic_rf_quick_init("\x00\xa8\xf0\xf0\xf0\xf0\xf0\xe7\xe7\xe7\xe7\xe7\x63\x6f\x05",
                    2, 1); 
}


// Configure the packe routines - set up my receiving packet address
void configure_pkt() {
	uns16 my_addr;
	uns16 last_pkt;
	
	my_addr = eeprom_read(EE_MY_ADDR_H);
	my_addr <<= 8;
	my_addr |= eeprom_read(EE_MY_ADDR_L);
	last_pkt = eeprom_read(EE_MY_LAST_PKT_ID_H);
	last_pkt <<= 8;
	last_pkt |= eeprom_read(EE_MY_LAST_PKT_ID_L);
	//serial_print_str("Addr: ");
	serial_print_int_hex(my_addr);
	serial_print_nl();
	//serial_print_str("Lpkt: ");
	//serial_print_int(last_pkt);
	//serial_print_nl();
	pkt_init(my_addr, last_pkt);
}

void configure_system() {
	    
	kill_interrupts();

    pir1 = 0;	// clear peripheral flag registers
    pir2 = 0;

	turn_analog_inputs_off();
	#ifdef PLATFORM_TYPE == SFE_TDN_V1
		make_output(PORTB, stat1);
		make_output(PORTB, stat2);
		make_output(PORTB, stat3);
		
		clear_pin(PORTB, stat1);
		clear_pin(PORTB, stat2);
		clear_pin(PORTB, stat3);
	#endif	
	pic_rf_setup();

	#if PLATFORM_TYPE == SFE_TDN_V1
		make_input(PORTB, 0);	// since we tie dr1 to BOTH rb0 and ra3
							// (you'll need to jumper this yourself)
	#endif
	
	delay_ms(250);	// Let everything settle
	delay_ms(250);	// Let everything settle
	delay_ms(250);	// Let everything settle
	delay_ms(250);	// Let everything settle
	delay_ms(250);	// Let everything settle

	#ifdef _PIC16
		timer_setup_0(TIMER_8BIT_MODE, TIMER_PRESCALER_1_TO_8, 0xff - 250 - 1);	// 1ms at 8Mhz
	#endif

	#ifdef _PIC18
		timer_setup_0(TIMER_16BIT_MODE, TIMER_PRESCALER_OFF, 0xffff - 750 + 13);	// 0.25ms at 12Mhz
	#endif	 


	serial_setup(BRGH_HIGH_SPEED, SPBRG_19200); 	// 1,25 = 19200 // 51 = 2400 // 8mhz 9600 = 0,12

    //set_bit(option_reg, INTEDG); // rising
	set_bit(intcon, INTE);	// Enable interrupts on rb0


	timer_start_0();

	configure_pkt();
	// configure_rf();
	configure_rf_quick();
	ds1631_setup();

	// 12 bit temp, one shot mode
	ds1631_set_config(DS1631_ADDR, 0b00001101);
	turn_peripheral_ints_on();
	turn_global_ints_on();

}	



void main( void )

{

uns8 pkt_rx_status;
uns16 tick, new_tick;
uns8 my_payload[PKT_PAYLOAD_SIZE];


	configure_system();

	delay_ms(250);
	delay_ms(250);
	delay_ms(250);
	delay_ms(250);

	#ifndef USE_LESS_FLASH
	serial_print_str( "\n<");
	serial_print_str(__TIME__);
	serial_print_str(">\n");
	#endif
	// We could print out some instructions here
	//print_str("x send pkt\n");
	//print_str("s05 set addr to 05\n");
	//print_str("w0004 write ee loc 00 to 04\n");
	//print_str("c config mode\n");
	//print_str("n leave config\n");
	delay_ms(100);
	
	tick = tick_get_count();
	
	while (1) {

	new_tick = tick_get_count();
	
	if (tick_calc_diff(tick, new_tick) > 250) {	// 250ms
		if (temp_requested) {	// temp requested, so lets see if it has finished
				uns8 config = ds1631_get_config(DS1631_ADDR);
				if (test_bit(config, 7)) {	// the DONE bit
					//serial_print_str("TSend:");
					temp_requested = 0;
					uns16 temp = ds1631_get_temp(DS1631_ADDR);
					my_payload[0] = PL_THIS_MY_SENSOR;
					my_payload[1] = 1 << CAPS_SENSOR_TEMP;
					my_payload[2] = temp >> 8; // temp MSB
					my_payload[3] = temp & 0xff;	// temp LSB
					my_payload[4] = 0;	// empty but clear for completeness
					my_payload[5] = 0;	// and easier debugging
					my_payload[6] = 0;
					my_payload[7] = 0;
					uns8 result = pkt_send_payload(send_to_addr, my_payload, PKT_FLAG_RESEND);
					//serial_print_int(result);
					//serial_print_spc();
				}
		}		
		tick = new_tick;
	}	

	while (pkt_receive_level) {
	    if (pkt_receive_level == 2) {
	    	pkt_rx_status = pkt_process_rf_data(rf_rx_buffer2);
	    } else {	
	    	pkt_rx_status = pkt_process_rf_data(rf_rx_buffer);
	    }	
		pkt_receive_level--;
		
		if (pkt_rx_status == PKT_STATUS_NEED_TO_REBROADCAST) {
			flash_led(fwd_led);
		} else {		
			flash_led(rx_led); 
		}	
				
		serial_print_str(" RX:");
		serial_print_int(pkt_rx_status);
		serial_print_str("\r\n");
	}	

    pkt_process_tx_queue();	// Anything to go out?

	process_leds();
	
	if (serial_rx_avail()) {
		process_rx();
	}	  
	switch (do_task) {
		case TASK_SEND_PACKET:
			do_task = 0;
			//serial_print_str("Send...");
			my_payload[0] = PL_REQ_SENSOR;
			my_payload[1] = 1 << CAPS_SENSOR_TEMP;
			my_payload[2] = 0;
			my_payload[3] = 0;
			my_payload[4] = 0;
			my_payload[5] = 0;
			my_payload[6] = 0;
			my_payload[7] = 0;
			uns8 result = pkt_send_payload(send_to_addr, my_payload, PKT_FLAG_RESEND);
			//serial_print_int(result);
			//serial_print_spc();
			break;
		case TASK_SEND_TEMP:
			ds1631_convert_temp(DS1631_ADDR);	
			temp_requested = 1;
			do_task = 0;
			break;
		case TASK_START_CONFIG_MODE:  // Go into config mode
			//config_mode = 1;
			//pkt_init(PKT_CONFIG_ADDR);
			do_task = 0;
			break;
		case TASK_END_CONFIG_MODE:  // Out of config mode
			config_mode = 0;
			configure_pkt(); // This reads from the eeprom
			do_task = 0;
			break;
	}
}	// while (1)


}


