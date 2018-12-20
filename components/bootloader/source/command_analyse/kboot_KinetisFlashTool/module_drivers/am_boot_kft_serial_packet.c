/*******************************************************************************
*                                 AMetal
*                       ----------------------------
*                       innovating embedded platform
*
* Copyright (c) 2001-2018 Guangzhou ZHIYUAN Electronics Co., Ltd.
* All rights reserved.
*
* Contact information:
* web site:    http://www.zlg.cn/
*******************************************************************************/

/**
 * \file
 * \brief bootloader kboot KinetisFlashTool �������ݰ�����ʵ��
 *
 *
 * \internal
 * \par Modification history
 * - 1.00 18-10-25  yrh, first implementation
 * \endinternal
 */

#include "am_boot_kft.h"
#include "am_boot_kft_command_packet.h"
#include "am_boot_kft_common.h"
#include "am_boot_kft_serial_packet.h"
#include <string.h>
#include "ametal.h"
#include "am_crc_soft.h"
#include "am_crc_table_def.h"
#include "am_zlg_crc.h"
#include "am_system.h"

static status_t __write_data(am_boot_kft_packet_dev_t *p_dev,const uint8_t *p_buffer, uint32_t byte_count);
static status_t __read_data(uint8_t *p_buffer, uint32_t byte_ount, uint32_t timeout_ms);
static status_t __read_data_packet(am_boot_kft_packet_dev_t *p_dev,framing_data_packet_t *p_packet, uint8_t *p_data, packet_type_t packet_type);
static status_t __read_start_byte(framing_header_t *p_header);
static status_t __read_header(framing_header_t *p_header);
static status_t __read_length(framing_data_packet_t *p_packet);
static status_t __read_crc16(framing_data_packet_t *p_packet);
static status_t __wait_for_ack_packet(am_boot_kft_packet_dev_t *p_dev);
static status_t __send_deferred_ack(am_boot_kft_packet_dev_t *p_dev);
static uint16_t __calculate_framing_crc16(framing_data_packet_t *p_packet, const uint8_t *p_data);


/**
 * \brief ping��Ӧ
 */
const ping_response_t kft_ping_response = {
    {
        {
          KFT_SERIAL_PROTOCOL_VERSION_BUGFIX,
          KFT_SERIAL_PROTOCOL_VERSION_MINOR,
          KFT_SERIAL_PROTOCOL_VERSION_MAJOR,
          KFT_SERIAL_PROTOCOL_VERSION_NAME }
        },
        0,      /** \brief ѡ�������ֵ���ģ������¼���crc16 */
        0xeaaa  /** \brief crc16�Ŀ�ʼ�ֽڣ������ͣ��汾��ѡ��  */
                /** \brief i.e. [5a a7 00 00 01 50 00 00]�� Calculated using CRC-16/XMODEM. */
};
static struct am_boot_kft_packet_funcs __g_packet_funcs = {
    serial_packet_read,
    serial_packet_write,
    serial_packet_abort,
    serial_packet_finalize,
    serial_packet_get_max_packet_size,
    serial_packet_queue_byte,
};

static serial_data_t            __g_serial_context;
static am_boot_kft_packet_dev_t __g_packet_dev;
static am_crc_soft_t            __g_crc_dev;

am_boot_kft_packet_handle_t am_boot_kft_packet_init(am_boot_serial_handle_t serial_handle)
{
    __g_packet_dev.packet_serv.p_funcs = &__g_packet_funcs;
    __g_packet_dev.packet_serv.p_drv   = &__g_packet_dev;
    __g_packet_dev.serial_handle       = serial_handle;

    __g_packet_dev.crc_handle = am_crc_soft_init (&__g_crc_dev,
                                                  &g_crc_table_16_1021);

    am_boot_serial_int_recev_callback_enable(
        serial_handle,
        __g_packet_dev.packet_serv.p_funcs->pfn_byte_received_callback);

    return &__g_packet_dev.packet_serv;
}


void serial_packet_queue_byte(uint8_t byte)
{
    __g_serial_context.callback_buffer[__g_serial_context.write_offset++] = byte;
    __g_serial_context.write_offset &= KFT_CALLBACK_BUFFER_SIZE - 1;
}

/**
 * \brief ���а���ֹ
 */
status_t serial_packet_finalize(void *p_arg)
{
    am_boot_kft_packet_dev_t *p_dev = (am_boot_kft_packet_dev_t *)p_arg;

    return __send_deferred_ack(p_dev);
}

/**
 * \brief ���а���
 */
status_t serial_packet_read(void         *p_arg,
                            uint8_t     **pp_packet,
                            uint32_t     *p_packet_length,
                            packet_type_t packet_type)
{
    if (!pp_packet || !p_packet_length) {
        am_kprintf("Error: invalid packet\r\n");
        return KFT_STATUS_INVALID_ARGUMENT;
    }
    am_boot_kft_packet_dev_t *p_dev = (am_boot_kft_packet_dev_t *)p_arg;

    *p_packet_length = 0;
    status_t status;

    __g_serial_context.is_back_to_back_write = AM_FALSE;

    /* Send ACK if needed */
    status = __send_deferred_ack(p_dev);
    if (status != KFT_STATUS_SUCCESS) {
        return status;
    }

    framing_data_packet_t framing_packet;

    am_bool_t is_packet_ok;
    do {
        /* Clear the packet data area so unsent parameters default to zero */
        memset(__g_serial_context.data, 0, sizeof(__g_serial_context.data));

        /* Receive the framing data packet */
        is_packet_ok = AM_TRUE;
        status_t status = __read_data_packet(p_dev,
                                            &framing_packet,
                                            __g_serial_context.data,
                                            packet_type);
        if (status != KFT_STATUS_SUCCESS) {
            /* No packet available */
            *p_packet_length = 0;
            return status;
        }

        // Verify crc.
        uint16_t calculated_crc = __calculate_framing_crc16(
                &framing_packet,  __g_serial_context.data);
        if (framing_packet.crc16 != calculated_crc) {
            am_kprintf("Error: invalid crc 0x%x, expected 0x%x\r\n", framing_packet.crc16, calculated_crc);
            is_packet_ok = AM_FALSE;
        }

        // Send Nak if necessary.
        if (!is_packet_ok) {
            serial_packet_send_sync(p_dev,KFT_FRAMING_PACKET_TYPE_NAK);
        }
    } while (!is_packet_ok);

    // Indicate an ACK must be sent.
    __g_serial_context.is_ack_needed = AM_TRUE;

    // Set caller's data buffer and length
    *pp_packet       = __g_serial_context.data;
    *p_packet_length = framing_packet.length;

    return KFT_STATUS_SUCCESS;
}

/**
 * \brief ���а�д
 */
status_t serial_packet_write(void          *p_arg,
                             const uint8_t *p_packet,
                             uint32_t       byte_count,
                             packet_type_t  packet_type)
{
    if (!p_packet || (byte_count > KFT_OUTGOING_PACKET_BUFFER_SIZE)) {
        am_kprintf("Error: invalid packet or packet size %d\r\n", byte_count);
        return KFT_STATUS_INVALID_ARGUMENT;
    }

    am_boot_kft_packet_dev_t *p_dev = (am_boot_kft_packet_dev_t *)p_arg;

    // Send ACK if needed.
    status_t status = __send_deferred_ack(p_dev);
    if (status != KFT_STATUS_SUCCESS) {
        return status;
    }

    // Back-to-back writes require delay for receiver to enter peripheral read routine.
    if (__g_serial_context.is_back_to_back_write) {
        __g_serial_context.is_back_to_back_write = false;

    }

    // Initialize the framing data packet.
    serial_framing_packet_t *framing_packet      = &__g_serial_context.framing_packet;
    framing_packet->data_packet.header.start_byte  = KFT_FRAMING_PACKET_START_BYTE;
    framing_packet->data_packet.header.packet_type = KFT_FRAMING_PACKET_TYPE_COMMAND;
    if (packet_type != KFT_PACKET_TYPE_COMMAND) {
        framing_packet->data_packet.header.packet_type = KFT_FRAMING_PACKET_TYPE_DATA;
    }
    framing_packet->data_packet.length = (uint16_t)byte_count;

    // Copy the caller's data buffer into the framing packet.
    if (byte_count) {
        memcpy(framing_packet->data, p_packet, byte_count);
    }

    // Calculate and set the framing packet crc.
    framing_packet->data_packet.crc16 = __calculate_framing_crc16(
            &framing_packet->data_packet, (uint8_t *)framing_packet->data);
    // Send the framing data packet.
    status = __write_data(p_dev,
                         (uint8_t *)framing_packet,
                          sizeof(framing_data_packet_t) + byte_count);
    if (status != KFT_STATUS_SUCCESS) {
        return status;
    }

    return __wait_for_ack_packet(p_dev);
}

void serial_packet_abort(void *p_arg)
{
    //am_boot_kft_packet_dev_t *p_dev = (am_boot_kft_packet_dev_t *)p_arg;

    assert(__g_serial_context.is_ack_needed);
    __g_serial_context.is_ack_abort_needed = AM_TRUE;
    __g_serial_context.is_ack_needed = AM_FALSE;
}

uint32_t serial_packet_get_max_packet_size(void *p_arg)
{
    return AM_BOOT_KFT_MIN_PACKET_BUFFER_SIZE;
}

// See serial_packet.h for documentation on this function.
status_t serial_packet_send_sync(am_boot_kft_packet_dev_t *p_dev,uint8_t framing_packet_type)
{
    framing_sync_packet_t sync;
    sync.header.start_byte  = KFT_FRAMING_PACKET_START_BYTE;
    sync.header.packet_type = framing_packet_type;

    // Indicate last transaction was a write.
    __g_serial_context.is_back_to_back_write = true;

    status_t status = __write_data(p_dev,(uint8_t *)&sync, sizeof(sync));
    if (status != KFT_STATUS_SUCCESS) {
        am_kprintf("Error: cannot send sync packet 0x%x, status = 0x%x\r\n", framing_packet_type, status);
        return status;
    }

    return status;
}

/**
 * \brief �ȴ�һ��ack
 */
static status_t __wait_for_ack_packet(am_boot_kft_packet_dev_t *p_dev)
{
    framing_sync_packet_t sync;
    do {
        // Receive the sync packet.
        status_t status = __read_header(&sync.header);
        if (status != KFT_STATUS_SUCCESS) {
            return status;
        }

        if ((sync.header.packet_type != KFT_FRAMING_PACKET_TYPE_ACK) &&
            (sync.header.packet_type != KFT_FRAMING_PACKET_TYPE_NAK) &&
            (sync.header.packet_type != KFT_FRAMING_PACKET_TYPE_ACK_ABORT)) {

            am_kprintf("Error: Unexpected sync byte 0x%x received, expected Ack, AckAbort or Nak\r\n",
                         sync.header.packet_type);
            return KFT_STATUS_INVALID_ARGUMENT;
        }

        if (sync.header.packet_type == KFT_FRAMING_PACKET_TYPE_ACK_ABORT) {
            return KFT_STATUS_ABORT_DATA_PHASE;
        }

        if (sync.header.packet_type == KFT_FRAMING_PACKET_TYPE_NAK) {
            // Re-transmit the last packet.
            status = __write_data(p_dev,
                                 (uint8_t *)&__g_serial_context.framing_packet,
                                  sizeof(framing_data_packet_t) + __g_serial_context.framing_packet.data_packet.length);
            if (status != KFT_STATUS_SUCCESS) {
                return status;
            }
        }
    } while (sync.header.packet_type == KFT_FRAMING_PACKET_TYPE_NAK);

    return KFT_STATUS_SUCCESS;
}

/**
 * \brief ����ping��Ӧ
 */
status_t serial_send_ping_response(am_boot_kft_packet_dev_t *p_dev)
{
    assert(p_dev);

    // Only reply if we're in an idle state
    if (!__g_serial_context.is_ack_needed ||
        !__g_serial_context.is_back_to_back_write ||
        !__g_serial_context.is_ack_abort_needed) {

        const uint8_t header[] = { KFT_FRAMING_PACKET_START_BYTE, KFT_FRAMING_PACKET_TYPE_PING_RESPONSE };

        am_boot_serial_byte_send(p_dev->serial_handle,
                                (const uint8_t *)&header,
                                 sizeof(header));
        am_boot_serial_byte_send(p_dev->serial_handle,
                                (uint8_t *)&kft_ping_response,
                                 sizeof(kft_ping_response));

    }
    return KFT_STATUS_PING;
}

/**
 * \brief ����ACK�������Ҫ
 */
static status_t __send_deferred_ack(am_boot_kft_packet_dev_t *p_dev)
{
    if (__g_serial_context.is_ack_needed) {
        // Send Ack for last received packet.
        __g_serial_context.is_ack_needed = false;
        return serial_packet_send_sync(p_dev, KFT_FRAMING_PACKET_TYPE_ACK);
    }
    else if (__g_serial_context.is_ack_abort_needed) {
        // Send AckAbort for last received packet.
        __g_serial_context.is_ack_abort_needed = false;
        return serial_packet_send_sync(p_dev, KFT_FRAMING_PACKET_TYPE_ACK_ABORT);
    }
    else {
        return KFT_STATUS_SUCCESS;
    }
}

/**
 * \brief д buff ������ֱ�������ֽڱ�����
 */
static status_t __write_data(am_boot_kft_packet_dev_t *p_dev,const uint8_t *p_buffer, uint32_t byte_count)
{
    status_t ret_val;
    ret_val =  am_boot_serial_byte_send(p_dev->serial_handle, p_buffer, byte_count);
    if(ret_val == byte_count) {
        return KFT_STATUS_SUCCESS;
    }
    return KFT_STATUS_FAIL;

}

/**
 * \brief �������ȡֱ���յ��涨���ֽ���
 */
static status_t __read_data(uint8_t *p_buffer, uint32_t byte_count, uint32_t timeout_ms)
{
    // On the target we read from our interrupt buffer
    uint32_t current_bytes_read = 0;
//    uint64_t startTicks = am_sys_tick_get();
//    uint64_t timeOutTicks = am_ms_to_ticks(timeout_ms * 1000);
//    uint64_t endTicks = startTicks;
//    uint64_t deltaTicks = 0;

    while (current_bytes_read != byte_count)
    {
//        endTicks = am_sys_tick_get();
//        deltaTicks = endTicks - startTicks;

        // Check timer roll over
//        if (endTicks < startTicks)
//        {
//            deltaTicks = endTicks + (~startTicks) + 1;
//        }
//
//        if (timeOutTicks && (deltaTicks >= timeOutTicks))
//        {
//            return BL_STATUS_TIMEOUT;
//        }

        if (__g_serial_context.read_offset != __g_serial_context.write_offset)
        {
            p_buffer[current_bytes_read++] = __g_serial_context.callback_buffer[__g_serial_context.read_offset++];

            __g_serial_context.read_offset &= KFT_CALLBACK_BUFFER_SIZE - 1;
        }
    }

    return KFT_STATUS_SUCCESS;
}

/**
 * \brief �������ȡֱ��������������֡
 */
static status_t __read_data_packet(am_boot_kft_packet_dev_t *p_dev,
                                 framing_data_packet_t      *packet,
                                 uint8_t                    *data,
                                 packet_type_t               packetType)
{
    /* Read the packet header. */
    status_t status = __read_header(&packet->header);
    if (status != KFT_STATUS_SUCCESS) {
        return status;
    }
    if (packet->header.packet_type == KFT_FRAMING_PACKET_TYPE_PING) {
        return serial_send_ping_response(p_dev);
    }

    uint8_t expected_packet_type = KFT_FRAMING_PACKET_TYPE_COMMAND;

    if (packetType != KFT_PACKET_TYPE_COMMAND) {
        expected_packet_type = KFT_FRAMING_PACKET_TYPE_DATA;
    }
    if (packet->header.packet_type != expected_packet_type) {
        am_kprintf("Error: read_data_packet found unexpected packet type 0x%x\r\n", packet->header.packet_type);
        return KFT_STATUS_FAIL;
    }

    /* Read the packet length. */
    status = __read_length(packet);
    if (status != KFT_STATUS_SUCCESS) {
        return status;
    }

    /* Make sure the packet doesn't exceed the allocated buffer size. */
    packet->length = MIN(KFT_INCOMING_PACKET_BUFFER_SIZE, packet->length);

    /* Read the crc */
    status = __read_crc16(packet);
    if (status != KFT_STATUS_SUCCESS) {
        return status;
    }

    // Read the data.
    if (packet->length > 0) {
        /* Clear the data area so unsent parameters default to zero. */
        memset(data, 0, packet->length);

        status = __read_data(data, packet->length, KFT_DEFAULT_BYTE_READ_TIMEOUT_MS * packet->length);
    }

    return status;
}

/**
 * \brief �������ȡֱ����ʼ�ֽڱ��ҵ�
 */
static status_t __read_start_byte(framing_header_t *header)
{
    // Read until start byte found.
    do {
        status_t status = __read_data(&header->start_byte, 1, 0); // no timeout for first byte of packet
        if (status != KFT_STATUS_SUCCESS) {
            return status;
        }

    } while (header->start_byte != KFT_FRAMING_PACKET_START_BYTE);

    return KFT_STATUS_SUCCESS;
}

/**
 * \brief �������ȡֱ����ͷ�����ҵ�
 */
static status_t __read_header(framing_header_t *header)
{
    // Wait for start byte.
    status_t status = __read_start_byte(header);
    if (status != KFT_STATUS_SUCCESS) {
        return status;
    }

    return __read_data(&header->packet_type,
                        sizeof(header->packet_type),
                        KFT_DEFAULT_BYTE_READ_TIMEOUT_MS * sizeof(header->packet_type));
}

/**
 * \brief �������ȡֱ�������ȱ��ҵ�
 */
static status_t __read_length(framing_data_packet_t *packet)
{
    union {
        uint8_t bytes[sizeof(uint16_t)];
        uint16_t halfword;
    } buffer;

    status_t status = __read_data((uint8_t *)&buffer.bytes,
                                   sizeof(buffer),
                                   KFT_DEFAULT_BYTE_READ_TIMEOUT_MS * sizeof(buffer));

    packet->length = buffer.halfword;
    return status;
}


/**
 * \brief �������ȡֱ��crc16���ҵ�
 */
static status_t __read_crc16(framing_data_packet_t *p_packet)
{
    union {
        uint8_t bytes[sizeof(uint16_t)];
        uint16_t halfword;
    } buffer;

    status_t status = __read_data((uint8_t *)&buffer.bytes,
                                   sizeof(buffer),
                                   KFT_DEFAULT_BYTE_READ_TIMEOUT_MS * sizeof(buffer));

    p_packet->crc16 = buffer.halfword;
    return status;
}

/**
 * \brief ����֡���ݰ��ϵ�crc
 */
static uint16_t __calculate_framing_crc16(framing_data_packet_t *p_packet, const uint8_t *p_data)
{
    uint32_t crc16;

    am_crc_pattern_t crc_pattern;

    crc_pattern.width     = 16;
    crc_pattern.poly      = 0x1021;
    crc_pattern.initvalue = 0x0000;
    crc_pattern.refin     = AM_FALSE;
    crc_pattern.refout    = AM_FALSE;
    crc_pattern.xorout    = 0x0000;

    am_crc_init (__g_packet_dev.crc_handle, &crc_pattern);

    am_crc_cal (__g_packet_dev.crc_handle,
               (uint8_t *)&p_packet->header.start_byte,
                sizeof(framing_data_packet_t) - sizeof(uint16_t));

    am_crc_cal (__g_packet_dev.crc_handle,
                p_data,
                p_packet->length);

    am_crc_final (__g_packet_dev.crc_handle, &crc16);

    return crc16;
}

/* end of file */