/*
 * Copyright (C) 2015 Freie Universität Berlin
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     examples
 * @{
 *
 * @file
 * @brief       Example application for demonstrating the RIOT network stack
 *
 * @author      Hauke Petersen <hauke.petersen@fu-berlin.de>
 *
 * @}
 */

#include <stdio.h>
#include <stdlib.h>

#include "shell.h"
#include "msg.h"
#include "thread.h"
#include "board.h"
#include "ztimer.h"
#include "net/sock/udp.h"
#include "net/sock/tcp.h"
#include "periph/i2c.h"
#include "luid.h"
#define MAIN_QUEUE_SIZE     (8)
#define SOCK_QUEUE_LEN  (1U)

#define _TEST_PORT_REMOTE   (0xa615)
#define _TEST_ADDR_REMOTE   { 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 }



static msg_t _main_msg_queue[MAIN_QUEUE_SIZE];

char udp_stack[THREAD_STACKSIZE_DEFAULT];
char tcp_stack[THREAD_STACKSIZE_DEFAULT];
char i2c_stack[THREAD_STACKSIZE_DEFAULT];
kernel_pid_t main_thread, udp_thread, tcp_thread, i2c_thread;
 
sock_tcp_t sock_queue[SOCK_QUEUE_LEN];
volatile uint16_t vbat, light;
void* udp_handler(void *arg);
void* tcp_handler(void *arg);
void* i2c_handler(void *arg);

int main(void)
{
    udp_thread = thread_create(udp_stack, sizeof(udp_stack), THREAD_PRIORITY_MAIN - 1,
                                 THREAD_CREATE_WOUT_YIELD | THREAD_CREATE_STACKTEST,
                                 udp_handler, NULL, "UDP_Custom" );
    tcp_thread = thread_create(tcp_stack, sizeof(tcp_stack), THREAD_PRIORITY_MAIN - 1,
                                 THREAD_CREATE_WOUT_YIELD | THREAD_CREATE_STACKTEST,
                                 tcp_handler, NULL, "TCP_Custom" );
    i2c_thread = thread_create(i2c_stack, sizeof(i2c_stack), THREAD_PRIORITY_MAIN - 1,
                                 THREAD_CREATE_WOUT_YIELD | THREAD_CREATE_STACKTEST,
                                 i2c_handler, NULL, "i2c_Custom" );    
    /* we need a message queue for the thread running the shell in order to
     * receive potentially fast incoming networking packets */
    msg_init_queue(_main_msg_queue, MAIN_QUEUE_SIZE);
    puts("RIOT network stack example application");

    /* start shell */
    puts("All up, running the shell now");
    char line_buf[SHELL_DEFAULT_BUFSIZE];
    shell_run(NULL, line_buf, SHELL_DEFAULT_BUFSIZE);

    /* should be never reached */
    return 0;
}
void* udp_handler(void *arg){
    (void)arg;
    eui64_t ext_addr;
    luid_get_eui64(&ext_addr);
    char ext_addr_str[32];
    l2util_addr_to_str(ext_addr.uint8, IEEE802154_LONG_ADDRESS_LEN, ext_addr_str);
    while (1) {
        char text[127];
        sprintf(text, "LoRa LabKit %s:\n Sun = %d\n Bat = %d mV\n", ext_addr_str, light, vbat);
    static const sock_udp_ep_t remote = { .addr = { .ipv6 = _TEST_ADDR_REMOTE },
                                          .family = AF_INET6,
                                          .port = _TEST_PORT_REMOTE };

    sock_udp_send(NULL, text, strlen(text), &remote);
        ztimer_sleep(ZTIMER_MSEC, 10000);
    }
    return NULL;
}



void* tcp_handler(void *arg){
    (void)arg;
     sock_tcp_ep_t local = SOCK_IPV6_EP_ANY;
    sock_tcp_queue_t queue;
    uint8_t tcp_buf[128];
    local.port = 12345;
 
    if (sock_tcp_listen(&queue, &local, sock_queue, SOCK_QUEUE_LEN, 0) < 0) {
        puts("Error creating listening queue");
        return NULL;
    }
    puts("Listening on port 12345");
    while (1) {
        sock_tcp_t *sock;
        char text[127];
        if (sock_tcp_accept(&queue, &sock, SOCK_NO_TIMEOUT) < 0) {
            puts("Error accepting new sock");
        }
        else {
            int read_res = 0;
 
            puts("Reading data");
            sock_tcp_write(sock, "\n> ", 2);
            while (read_res >= 0) {
                for(uint8_t t = 0; t< sizeof(tcp_buf);t++)
                    tcp_buf[t] = 0;
                read_res = sock_tcp_read(sock, &tcp_buf, sizeof(tcp_buf),
                                         SOCK_NO_TIMEOUT);
                if (read_res <= 0) {
                    puts("Disconnected");
                    break;
                }
                else {
                    printf("Read: \"");
                    for (int i = 0; i < read_res; i++) {
                        printf("%c", tcp_buf[i]);
                    }
                    puts("\"");
            if(strncmp((char*)tcp_buf, "test", 4) == 0){
                uint16_t c = 0;
                uint16_t i = 0;
                uint8_t s = 0;
                uint8_t current_char = 5;
                while(current_char < 124)
                {
                    if(tcp_buf[current_char] == '-')
                        {
                            switch (tcp_buf[current_char+1]){
                                case 'c':
                                    c = atoi((char*)(tcp_buf + current_char+3));
                                    break;
                                case 's':
                                    s = atoi((char*)(tcp_buf + current_char+3));
                                    break;
                                case 'i':
                                    i = atoi((char*)(tcp_buf + current_char+3));
                                    break;

                            }
                        }
                        current_char++;
                }
                if(c == 0)
                    {
                        c = 50;
                    }
                if(i == 0)
                    {
                        i = 5;
                    }
                    uint32_t packet_count = 0;
                    while(packet_count < c)
                        {
                            sprintf(text, " LoraLAB_TEST_SENDING = %ld\n", packet_count);
                            if(s >= strlen(text) || s == 0)
                                {
                                    if (sock_tcp_write(sock, text, strlen(text)) < 0) {
                                    puts("Error sending\n");
                                    goto exit_tcp;
                                }
                                }
                                else 
                                if (sock_tcp_write(sock, text, s) < 0) {
                                    puts("Error sending\n");
                                    goto exit_tcp;
                            }
                            
                        packet_count++;
                        ztimer_sleep(ZTIMER_MSEC, i);
                        }
            }
            else if(strncmp((char*)tcp_buf, "led_on", 6) == 0){
                gpio_set(LED_USB_LINK);
                sprintf(text, " Светодиод включен\n");
                sock_tcp_write(sock, text, strlen(text));
            }
            else if(strncmp((char*)tcp_buf, "led_off", 7) == 0){
                gpio_clear(LED_USB_LINK);
                sprintf(text, " Светодиод выключен\n");
                sock_tcp_write(sock, text, strlen(text));
            }
            else if(strncmp((char*)tcp_buf, "sensors", 4) == 0){
                sprintf(text, "Датчики на LoRa LabKit:\n Датчик освещения = %d\n Напряжение батареи = %d mV\n", light, vbat);
                sock_tcp_write(sock, text, strlen(text));
            }
            else if(strncmp((char*)tcp_buf, "help", 4) == 0){
                sprintf(text, "Доступные команды:\n\nhelp\nled_on\nled_off\ntest\nsensors\n");
                sock_tcp_write(sock, text, strlen(text));
            }
            sock_tcp_write(sock, "\n> ", 2);
                    }
    }
    exit_tcp:
            sock_tcp_disconnect(sock);
        }
    }
    sock_tcp_stop_listen(&queue);
    return NULL;
}
void* i2c_handler(void *arg){
    (void)arg;
    int dev = 0;
    i2c_init(dev);
    while(1){
        i2c_acquire(dev);
        vbat = 0;
        le_uint16_t reg_value;
        le_uint16_t light_value;
        i2c_read_regs(0, 0x40, 0x02, &reg_value.u16, 2, 0);
        vbat = byteorder_ltobs(reg_value).u16;
        vbat = (vbat>>3)*4;
        i2c_write_byte(0,0x23, 0b00100000, 0);
        ztimer_sleep(ZTIMER_MSEC, 180);
        i2c_read_bytes(0, 0x23, &light_value.u16,2, 0);
        i2c_write_byte(0,0x23, 0b00000001, 0);
        light = byteorder_ltobs(light_value).u16;
        light = (light>>3)*4;
        i2c_release(dev);
        ztimer_sleep(ZTIMER_MSEC, 1000);
    }
    return NULL;
}

