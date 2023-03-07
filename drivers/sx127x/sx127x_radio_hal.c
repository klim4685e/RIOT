/*
 * Copyright (C) 2023 
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     drivers_sx127x
 * @{
 * @file
 * @brief       Radio HAL implementation for the Semtech SX126x 
 *
 * @author      Klim Evdokimov <klimevdokimov@mail.ru>
 * @}
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define ENABLE_DEBUG 0
#include "debug.h"

#include "net/ieee802154/radio.h"

#include "event/thread.h"
#include "ztimer.h"

#include "thread.h"

#include "kernel_defines.h"
#include "event.h"

#include "periph/dac.h"
#include "sx127x_registers.h"
#include "sx127x_internal.h"
//#include "sx127x_netdev.h"
#include "sx127x.h"

#define LORA_ACK_REPLY_US           1024

#ifndef SX127X_HAL_CHAN_BASE
#define SX127X_HAL_CHAN_BASE        (435000000LU)
#endif

#ifndef SX127X_HAL_CHAN_SPACING
#define SX127X_HAL_CHAN_SPACING     (200000LU)
#endif

#define SX127X_CHAN_MIN (IEEE802154_CHANNEL_MIN_SUBGHZ)

#ifndef SX127X_CHAN_MAX
#define SX127X_CHAN_MAX (8)
#endif

#define SX127X_POWER_MIN (-1)

#ifndef SX127X_POWER_MAX
#define SX127X_POWER_MAX (0)
#endif



static const ieee802154_radio_ops_t sx127x_ops;


void sx127x_hal_setup(sx127x_t *dev, ieee802154_dev_t *hal)
{
    hal->driver = &sx127x_ops;
    hal->priv = dev;

    dev->ack_timer.arg = dev;
    //dev->ack_timer.callback = ack_timer_cb;
    dev->ack_filter = false;
}

void _on_dio0_irq(ieee802154_dev_t *hal, volatile uint8_t interruptReg)
{
    sx127x_t *dev = hal->priv;

    switch (dev->settings.state) {
    case SX127X_RF_RX_RUNNING:

        if ((interruptReg & SX127X_RF_LORA_IRQFLAGS_PAYLOADCRCERROR_MASK) ==
            SX127X_RF_LORA_IRQFLAGS_PAYLOADCRCERROR) {
            sx127x_reg_write(dev, SX127X_REG_LR_IRQFLAGS,
                             SX127X_RF_LORA_IRQFLAGS_PAYLOADCRCERROR);
            //sx127x_set_state(dev, SX127X_RF_IDLE);
            hal->cb(hal, IEEE802154_RADIO_INDICATION_CRC_ERROR);
        }
        else {
            sx127x_reg_write(dev, SX127X_REG_LR_IRQFLAGS, SX127X_RF_LORA_IRQFLAGS_RXDONE);
            hal->cb(hal, IEEE802154_RADIO_INDICATION_RX_DONE);
        }
        break;
    case SX127X_RF_TX_RUNNING:
        /* Clear IRQ */
        sx127x_reg_write(dev, SX127X_REG_LR_IRQFLAGS,
                         SX127X_RF_LORA_IRQFLAGS_TXDONE);
        sx127x_set_state(dev, SX127X_RF_IDLE);

        hal->cb(hal, IEEE802154_RADIO_CONFIRM_TX_DONE);
        break;
    case SX127X_RF_IDLE:
        DEBUG("[sx127x] netdev: sx127x_on_dio0: IDLE state\n");
        break;
    default:
        DEBUG("[sx127x] netdev: sx127x_on_dio0: unknown state [%d]\n",
              dev->settings.state);
        break;
    }
}

void sx127x_hal_task_handler(ieee802154_dev_t *hal)
{
    (void)hal;
    sx127x_t *dev = hal->priv;
    uint8_t interruptReg = sx127x_reg_read(dev, SX127X_REG_LR_IRQFLAGS);

    if (interruptReg & (SX127X_RF_LORA_IRQFLAGS_TXDONE |
                        SX127X_RF_LORA_IRQFLAGS_RXDONE)) {
        _on_dio0_irq(hal, interruptReg);
    }

}

                        /*RADIO HAL FUNCTIONS*/

static int _write(ieee802154_dev_t *hal, const iolist_t *iolist){


    uint8_t size = iolist_size(iolist);
    sx127x_t *dev = hal->priv;
    /* Include CRC */
    sx127x_set_payload_length(dev, size);

    /* Full buffer used for Tx */
    sx127x_reg_write(dev, SX127X_REG_LR_FIFOTXBASEADDR, 0x00);
    sx127x_reg_write(dev, SX127X_REG_LR_FIFOADDRPTR, 0x00);
    /* Write payload buffer */
    for (const iolist_t *iol = iolist; iol; iol = iol->iol_next) {
        if (iol->iol_len > 0) {
            sx127x_write_fifo(dev, iol->iol_base, iol->iol_len);
            DEBUG("[sx127x] Wrote to payload buffer.\n");
        }
    }
    return 0;
}

static int _request_op(ieee802154_dev_t *hal, ieee802154_hal_op_t op, void *ctx){
    sx127x_t *dev = hal->priv;
    
    (void)dev;
    (void)ctx;
    switch (op) {
        case IEEE802154_HAL_OP_TRANSMIT:
        dac_set(DAC_LINE(0), 56000U);
            sx127x_reg_write(dev, SX127X_REG_LR_IRQFLAGSMASK,
                         SX127X_RF_LORA_IRQFLAGS_RXTIMEOUT |
                         SX127X_RF_LORA_IRQFLAGS_RXDONE |
                         SX127X_RF_LORA_IRQFLAGS_PAYLOADCRCERROR |
                         SX127X_RF_LORA_IRQFLAGS_VALIDHEADER |
                         /* SX127X_RF_LORA_IRQFLAGS_TXDONE | */
                         SX127X_RF_LORA_IRQFLAGS_CADDONE |
                         SX127X_RF_LORA_IRQFLAGS_FHSSCHANGEDCHANNEL |
                         SX127X_RF_LORA_IRQFLAGS_CADDETECTED);

            /* Set TXDONE interrupt to the DIO0 line */
            sx127x_reg_write(dev, SX127X_REG_DIOMAPPING1,
                         (sx127x_reg_read(dev, SX127X_REG_DIOMAPPING1) &
                          SX127X_RF_LORA_DIOMAPPING1_DIO0_MASK) |
                         SX127X_RF_LORA_DIOMAPPING1_DIO0_01);

            sx127x_set_state(dev, SX127X_RF_TX_RUNNING);
            
            /* Put chip into transfer mode */
            sx127x_set_op_mode(dev, SX127X_RF_OPMODE_TRANSMITTER );
        break;
        case IEEE802154_HAL_OP_SET_RX:
            sx127x_set_rx_timeout(dev, 0);
            dac_set(DAC_LINE(0), 0U);
            sx127x_set_rx(dev);
        break;

        case IEEE802154_HAL_OP_SET_IDLE:
            sx127x_set_standby(dev);
        break;

        case IEEE802154_HAL_OP_CCA:
            sx127x_set_standby(dev);
            //sx127x_start_cad(dev);
            hal->cb(hal, IEEE802154_RADIO_CONFIRM_CCA); //КОСТЫЛЬ!
        break;

        default:
            DEBUG("Wrong request, assertion\n");
            assert(false);
        break;

    }
    return 0;
}

static int _confirm_op(ieee802154_dev_t *hal, ieee802154_hal_op_t op, void *ctx){
    (void)op;
    (void)ctx;
    ieee802154_tx_info_t *info = ctx;
    sx127x_t *dev = hal->priv;
    bool eagain = false;
    
switch (op){
    case IEEE802154_HAL_OP_TRANSMIT:
       if (info) {
            info->status = (dev->cad_detected) ? TX_STATUS_MEDIUM_BUSY : TX_STATUS_SUCCESS;
        }
        ztimer_sleep(ZTIMER_USEC, 1024);
    break;

    case IEEE802154_HAL_OP_SET_RX:
       
    break;

    case IEEE802154_HAL_OP_SET_IDLE:
        
        break;

    case IEEE802154_HAL_OP_CCA:
            *((bool*) ctx) = !dev->cad_detected;
    break;

    default:
        eagain = false;
        break;
    
}

    if (eagain) {
       
        return -EAGAIN;
    }

    return 0;
}

static int _len(ieee802154_dev_t *hal){
    (void)hal;
    sx127x_t *dev = hal->priv;
    return sx127x_reg_read(dev, SX127X_REG_LR_RXNBBYTES);
}

static int _read(ieee802154_dev_t *hal, void *buf, size_t max_size, ieee802154_rx_info_t *info)
{
    (void)hal;
    (void)buf;
    (void)info;
    sx127x_t *dev = hal->priv;
    dev->size = sx127x_reg_read(dev, SX127X_REG_LR_RXNBBYTES);

    /* Exclude CRC */
    if (dev->size > max_size) {
        sx127x_reg_write(dev, SX127X_REG_LR_FIFORXBASEADDR, 0);
        sx127x_reg_write(dev, SX127X_REG_LR_FIFOADDRPTR, 0);
        return -ENOBUFS;
    }

    if (dev->size < 3) {
        sx127x_reg_write(dev, SX127X_REG_LR_FIFORXBASEADDR, 0);
        sx127x_reg_write(dev, SX127X_REG_LR_FIFOADDRPTR, 0);
        return -EBADMSG;
    }

    if (buf == NULL) {
        sx127x_reg_write(dev, SX127X_REG_LR_FIFORXBASEADDR, 0);
        sx127x_reg_write(dev, SX127X_REG_LR_FIFOADDRPTR, 0);
        return dev->size;
    }

    /* Read the last packet from FIFO */
    uint8_t last_rx_addr = sx127x_reg_read(dev, SX127X_REG_LR_FIFORXCURRENTADDR);
    sx127x_reg_write(dev, SX127X_REG_LR_FIFOADDRPTR, last_rx_addr);
    sx127x_read_fifo(dev, (uint8_t*)buf, dev->size);
    return dev->size;

}

static int _set_cca_threshold(ieee802154_dev_t *hal, int8_t threshold)
{
    (void)hal;
    (void)threshold;

    return 0;
}

static int _config_phy(ieee802154_dev_t *hal, const ieee802154_phy_conf_t *conf){
    (void)hal;
    (void)conf;
    sx127x_t *dev = hal->priv;
    uint8_t channel = conf->channel;
    int8_t pow = conf->pow;
    if((channel > SX127X_CHAN_MAX))
        return -EINVAL;
    
    sx127x_set_channel(dev, (channel)*SX127X_HAL_CHAN_SPACING + SX127X_HAL_CHAN_BASE);
    if((pow < SX127X_POWER_MIN) && (pow > SX127X_POWER_MAX))
        return -EINVAL;

    sx127x_set_tx_power(dev, pow);
    return 0;
}

static int _off(ieee802154_dev_t *hal)
{
    (void)hal;
   
    return 0;
}

static int _config_addr_filter(ieee802154_dev_t *hal, ieee802154_af_cmd_t cmd, const void *value)
{
    (void)cmd;
    (void)value;
    uint16_t pan_id = *(uint16_t*)value;
    sx127x_t *dev = hal->priv;
    switch(cmd) {
        case IEEE802154_AF_SHORT_ADDR:
            memcpy(dev->short_addr, value, IEEE802154_SHORT_ADDRESS_LEN);
            break;
        case IEEE802154_AF_EXT_ADDR:
            memcpy(dev->long_addr, value, IEEE802154_LONG_ADDRESS_LEN);
            break;
        case IEEE802154_AF_PANID:
            dev->pan_id = pan_id;
            break;
        case IEEE802154_AF_PAN_COORD:
            return -ENOTSUP;
    }
    return 0;
}

static int _request_on(ieee802154_dev_t *hal)
{
    (void)hal;

    return 0;
}

static int _confirm_on(ieee802154_dev_t *hal)
{
    (void)hal;

    return 0;
}

static int _set_cca_mode(ieee802154_dev_t *hal, ieee802154_cca_mode_t mode)
{
    (void)hal;
    //sx127x_t* dev = hal->priv;
    DEBUG("[sx126x] netdev: set_cca_mode \n");
    (void)mode;

    return 0;
}

static int _config_src_addr_match(ieee802154_dev_t *hal, ieee802154_src_match_t cmd, const void *value)
{
    (void)hal;
    (void)cmd;
    (void)value;

    return -ENOTSUP;
}

static int _set_frame_filter_mode(ieee802154_dev_t *hal, ieee802154_filter_mode_t mode)
{
    sx127x_t* dev = hal->priv;

    bool ackf = true;
    bool _promisc = false;

    switch (mode) {
        case IEEE802154_FILTER_ACCEPT:
            DEBUG("Filter_accept_all\n");
            break;
        case IEEE802154_FILTER_PROMISC:
            _promisc = true;
            break;
        case IEEE802154_FILTER_ACK_ONLY:
            ackf = false;
            DEBUG("Filter_ack_only\n");
            break;
        default:
            return -ENOTSUP;
    }

    dev->ack_filter = ackf;
    dev->promisc = _promisc;

    return 0;
}

static int _set_csma_params(ieee802154_dev_t *hal, const ieee802154_csma_be_t *bd, int8_t retries)
{
    (void)hal;
    (void)bd;
    (void)retries;

    return -ENOTSUP;
}


static const ieee802154_radio_ops_t sx127x_ops = {
    .caps =  IEEE802154_CAP_SUB_GHZ
          | IEEE802154_CAP_IRQ_CRC_ERROR
          | IEEE802154_CAP_IRQ_TX_DONE
          | IEEE802154_CAP_IRQ_CCA_DONE
          | IEEE802154_CAP_PHY_BPSK,
    .write = _write,
    .read = _read,
    .request_on = _request_on,
    .confirm_on = _confirm_on,
    .len = _len,
    .off = _off,
    .request_op = _request_op,
    .confirm_op = _confirm_op,
    .set_cca_threshold = _set_cca_threshold,
    .set_cca_mode = _set_cca_mode,
    .config_phy = _config_phy,
    .set_csma_params = _set_csma_params,
    .config_addr_filter = _config_addr_filter,
    .config_src_addr_match = _config_src_addr_match,
    .set_frame_filter_mode = _set_frame_filter_mode,
};
