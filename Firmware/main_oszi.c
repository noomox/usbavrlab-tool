/************************************************************************************************
 * Project: USB AVR-ISP
 * Author: Christian Ulrich
 * Contact: christian at ullihome dot de
 *
 * Creation Date: 2007-09-24
 * Copyright: (c) 2007 by Christian Ulrich
 * License: GPLv2 for private use
 *	        commercial use prohibited 
 *
 * Changes:
 ***********************************************************************************************/

#include "config.h"

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <avr/wdt.h>
#include "usbdrv.h"
#include "usbconfig.h"
#include "usb_uart.h"
#include "isp.h"
#include "led.h"
#include "timer.h"
#include "main.h"
#include "buffer.h"

#define FUNC_READ_BUFFER			1
#define FUNC_STOP   				3
#define FUNC_START					4
#define FUNC_WRITE_EEP				5
#define FUNC_READ_EEP				6
#define FUNC_SET_TRIGGER			7
#define FUNC_SET_TRIGGER_LEVEL		8
#define FUNC_SET_SAMPLERATE			9
#define FUNC_GET_STATE				10

#define FUNC_START_BOOTLOADER		30
#define FUNC_GET_TYPE				0xFE

#define STATE_IDLE					0
#define STATE_READ					1
#define STATE_WRITE					2

#define STATE_STOPPED				3
#define STATE_RUNNING				4
#define STATE_AUTO_STOPPED			5
#define STATE_TRIGGERING			6

#define TRIGGER_CONTINOUS			0
#define TRIGGER_FALLING_EDGE		1
#define TRIGGER_RISING_EDGE			2

uint8_t usb_state = STATE_IDLE;
uint8_t oszi_state = STATE_STOPPED;
uint8_t trigger_state = TRIGGER_CONTINOUS;
uint8_t trigger_level = 0;
uint16_t adc_sr = 0;
volatile uint8_t adc_sc = 1;

#ifndef USBASP_COMPATIBLE
led_t leds[] =  {{4,LED_OFF,LED_OFF},
                 {3,LED_OFF,LED_OFF},
                 {5,LED_OFF,LED_OFF}}; 
#else
led_t leds[] =  {{0,LED_OFF,LED_OFF},
                 {1,LED_OFF,LED_OFF},
                 {3,LED_OFF,LED_OFF}}; 
#endif
const uint8_t led_count = sizeof(leds)/sizeof(led_t);

#define buffer_size 900
volatile uint8_t buffer_data[buffer_size];
volatile uint16_t buffer_idx = 0;
volatile uint16_t buffer_rd_idx = 0;

uint8_t mes_count = 0;
uint16_t adcvalue = 0;

void start_oszi(void);

int main(void)
{
  extern uchar usbNewDeviceAddr;
  PORTC |= (1<<PC2);
  uint8_t i;
//Reconnect USB
  usbDeviceDisconnect();  /* enforce re-enumeration, do this while interrupts are disabled! */
  i = 0;
  while(--i)
     _delay_ms(2);
  usbDeviceConnect();
  usbInit();
  sei();
  LED_PORT |= (1<<leds[LED_GREEN].pin);
  LED_PORT &= ~(1<<leds[LED_RED].pin);
  for (i=0;i<3;i++)
    TIMER_delay(250);
  LED_PORT |= (1<<leds[LED_RED].pin);
  LED_poll();
  
  PORTC &= ~(1<<PC1);							   // Pullup aus	
                               					   // setzen auf 8 (1) und ADC aktivieren (1)
  ADMUX = 1;                     				   // Kanal waehlen
  ADMUX |= (1<<REFS1) | (1<<REFS0); 			   // interne Referenzspannung nutzen 
  ADCSRA |= (1<<ADSC)|(1<<ADEN);              			   // eine ADC-Wandlung 
  while ( ADCSRA & (1<<ADSC));

  while(1)
    {
	  if ((oszi_state == STATE_STOPPED)||(oszi_state == STATE_AUTO_STOPPED))
	    {
          if(usbNewDeviceAddr)
            LED_PORT &= ~(1<<leds[LED_BLUE].pin);
          usbPoll();
        }
      else if (oszi_state == STATE_TRIGGERING)
	    {
		  if (TIMER_timeout == 0)
		    {
		      usbPoll();
		      TIMER_start(10);
		    }
          else
		    {
              ADCSRA |= (1<<ADSC)|(1<<ADEN);              			   // eine ADC-Wandlung 
              while ( ADCSRA & (1<<ADSC));
			  if (((trigger_state == TRIGGER_FALLING_EDGE) && (ADCW < trigger_level))
			  ||  ((trigger_state == TRIGGER_RISING_EDGE)  && (ADCW > trigger_level)))
			    start_oszi();
			}
		}
	     
	}
}

ISR(ADC_vect)
{
  if (buffer_idx > buffer_size)
    {
      ADCSRA &= ~((1<<ADEN)|(1<<ADFR)|(1<<ADIE));
      oszi_state = STATE_AUTO_STOPPED;
      LED_PORT &= ~(1<<leds[LED_GREEN].pin);
      LED_PORT |= (1<<leds[LED_RED].pin);
   	}
  else
    {
	  buffer_data[buffer_idx++] = ADCW;
	}
}

void start_oszi(void)
{
  if (adc_sr == 0) return;
  LED_PORT |= (1<<leds[LED_GREEN].pin);
  LED_PORT &= ~(1<<leds[LED_RED].pin);
  oszi_state = STATE_RUNNING;
  ADCSRA |= (1<<ADEN)|(1<<ADFR)|(1<<ADIE)|(1<<ADSC);
}

static uchar replyBuffer[8];

uint8_t usbFunctionSetup(uint8_t data[8])
{
  uchar len = 0;

  if(data[1] == FUNC_GET_TYPE)
    {
	  replyBuffer[0] = 9;
	  len = 1;
	}
  else if(data[1] == FUNC_START_BOOTLOADER)
    {
      cli();
	  wdt_enable(WDTO_15MS);
      while(1);
	  len = 0;
	}
  else if(data[1] == FUNC_READ_BUFFER)
    {
	  usb_state = STATE_READ;
      len = 0xff;
	}
  else if(data[1] == FUNC_START)
    {
	  start_oszi();
      len = 0;
	}
  else if(data[1] == FUNC_STOP)
    {
	  oszi_state = STATE_STOPPED;
      LED_PORT |= (1<<leds[LED_RED].pin);
      LED_PORT |= (1<<leds[LED_GREEN].pin);
      len = 0;
	}
  else if(data[1] == FUNC_READ_EEP)
    {
	  replyBuffer[0] = eeprom_read_byte(data[2]+20);
      len = 1;
	}
  else if(data[1] == FUNC_WRITE_EEP)
    {
	  eeprom_write_byte(data[2]+20,data[3]);
      len = 0;
	}
  else if(data[1] == FUNC_SET_TRIGGER)
    {
	  trigger_state = data[2];
	  oszi_state = STATE_TRIGGERING;
      LED_PORT &= ~(1<<leds[LED_GREEN].pin);
      LED_PORT = (1<<leds[LED_RED].pin);
      len = 0;
	}
  else if(data[1] == FUNC_SET_TRIGGER_LEVEL)
    {
	  trigger_level = data[2];
      len = 0;
	}
  else if(data[1] == FUNC_SET_SAMPLERATE)
    {
	  uint16_t new_sr = data[2]+(data[3]<<8);
	  //1-256 ksps 
      ADCSRA &= ~((1<<ADPS0)|(1<<ADPS1)|(1<<ADPS2));
	  if (new_sr < 8)       
	    {
	      ADCSRA |= ((1<<ADPS0)|(1<<ADPS1)|(1<<ADPS2));	//128 = 93.950 ADC Freq = 7.211 kSps
		  adc_sr = 7;
        } 
	  else if (new_sr < 15) 
	    {
	      ADCSRA |= ((1<<ADPS1)|(1<<ADPS2));			//64 = 187.500 ADC Freq = 14.423 kSps 
		  adc_sr = 14;
        }
	  else if (new_sr < 30) 
	    {
	      ADCSRA |= ((1<<ADPS0)|(1<<ADPS2));			//32 = 375.000 ADC Freq = 28.846 kSps
		  adc_sr = 29;
        } 
	  else if (new_sr < 59) 
	    {
	      ADCSRA |= ((1<<ADPS2));						//16 = 750.000 ADC Freq = 57.692 kSps
		  adc_sr = 58;
        }
	  else if (new_sr < 116) 
	    {
	      ADCSRA |= ((1<<ADPS0)|(1<<ADPS1));			    //8 = 1500.000 ADC Freq = 115.384 kSps
		  adc_sr = 115;
        } 
	  else if (new_sr < 232) 
	    {
	      ADCSRA |= ((1<<ADPS1));						//4 = 3000.000 ADC Freq = 230.769 kSps
		  adc_sr = 231;
        } 
//      uint8_t diff = adc_sr-data[2];
      replyBuffer[0] = adc_sr & 0xff;
      replyBuffer[1] = adc_sr >> 8;
	  len = 2;
	}
  else if(data[1] == FUNC_GET_STATE)
    {
      replyBuffer[0] = oszi_state;
	  len = 1;
	}
  usbMsgPtr = replyBuffer;
  return len;
}

uint8_t usbFunctionRead( uint8_t *data, uint8_t len )
{
  if (usb_state != STATE_READ)
    return 0xff;
  uint8_t asize = 0;
  while (asize<len)
    {
	  if (buffer_rd_idx >= buffer_idx)
	    {
		  buffer_idx = 0;
		  buffer_rd_idx = 0;
	      break;
		}
      else data[asize++] = buffer_data[buffer_rd_idx++];
	}
  return asize;
}

uint8_t usbFunctionWrite( uint8_t *data, uint8_t len )
{
  if (usb_state != STATE_WRITE)
    return 0xff;
  return len;
}
