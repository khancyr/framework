#include <omd_uavcan/omd_uavcan.h>
#include <common/timing.h>
#include <string.h>

#if CH_CFG_USE_MUTEXES_RECURSIVE != TRUE
#error "CH_CFG_USE_MUTEXES_RECURSIVE required"
#endif

#ifndef OMD_UAVCAN_CANARD_MEMORY_POOL_SIZE
#define OMD_UAVCAN_CANARD_MEMORY_POOL_SIZE 1024
#endif

#ifndef OMD_UAVCAN_RX_THREAD_STACK_SIZE
#define OMD_UAVCAN_RX_THREAD_STACK_SIZE 256
#endif

#ifndef OMD_UAVCAN_TX_THREAD_STACK_SIZE
#define OMD_UAVCAN_TX_THREAD_STACK_SIZE 128
#endif

#ifndef OMD_UAVCAN_TRANSFER_ID_MAP_SIZE
#define OMD_UAVCAN_TRANSFER_ID_MAP_SIZE 32
#endif

static THD_FUNCTION(omd_uavcan_rx_thd_func, arg);
static THD_FUNCTION(omd_uavcan_tx_thd_func, arg);

static bool omd_uavcan_should_accept_transfer(const CanardInstance* canard, uint64_t* out_data_type_signature, uint16_t data_type_id, CanardTransferType transfer_type, uint8_t source_node_id);

static void omd_uavcan_on_transfer_rx(CanardInstance* canard, CanardRxTransfer* transfer);

static CanardCANFrame convert_CANRxFrame_to_CanardCANFrame(const CANRxFrame* chibios_frame);
static CANTxFrame convert_CanardCANFrame_to_CANTxFrame(const CanardCANFrame* canard_frame);

MEMORYPOOL_DECL(tx_thread_pool, OMD_UAVCAN_TX_THREAD_STACK_SIZE, chCoreAllocAligned);
MEMORYPOOL_DECL(rx_thread_pool, OMD_UAVCAN_RX_THREAD_STACK_SIZE, chCoreAllocAligned);

void omd_uavcan_init(struct omd_uavcan_instance_s* instance, CANDriver* can_dev, void* message_heap_mem, size_t message_heap_size) {
    if (!instance || !can_dev || !message_heap_mem) {
        return;
    }

    memset(instance, 0, sizeof(struct omd_uavcan_instance_s));

    instance->canard_memory_pool = chHeapAlloc(NULL, OMD_UAVCAN_CANARD_MEMORY_POOL_SIZE);
    if (!instance->canard_memory_pool) { goto fail; }

    instance->can_dev = can_dev;

    chMtxObjectInit(&instance->canard_mtx);
    chMtxObjectInit(&instance->tx_mtx);

    canardInit(&instance->canard, instance->canard_memory_pool, OMD_UAVCAN_CANARD_MEMORY_POOL_SIZE, omd_uavcan_on_transfer_rx, omd_uavcan_should_accept_transfer, instance);

    chBSemObjectInit(&instance->tx_thread_semaphore, true);

    instance->tx_thread = chThdCreateFromHeap(NULL, OMD_UAVCAN_TX_THREAD_STACK_SIZE, "omd_uavcan_tx", HIGHPRIO-2, omd_uavcan_tx_thd_func, instance);
    if (!instance->tx_thread) { goto fail; }
    chThdRelease(instance->tx_thread);

    instance->rx_thread = chThdCreateFromHeap(NULL, OMD_UAVCAN_RX_THREAD_STACK_SIZE, "omd_uavcan_rx", HIGHPRIO-2, omd_uavcan_rx_thd_func, instance);
    if (!instance->rx_thread) { goto fail; }
    chThdRelease(instance->rx_thread);

    // tell threads to start
    chMsgSend(instance->tx_thread, (msg_t)instance);
    chMsgSend(instance->rx_thread, (msg_t)instance);

    return;

fail:
    if (instance->canard_memory_pool) {
        chHeapFree(instance->canard_memory_pool);
        instance->canard_memory_pool = NULL;
    }

    if (instance->rx_thread) {
        // tell thread to terminate
        chMsgSend(instance->tx_thread, (msg_t)NULL);
        instance->rx_thread = NULL;
    }

    if (instance->tx_thread) {
        // tell thread to terminate
        chMsgSend(instance->tx_thread, (msg_t)NULL);
        instance->tx_thread = NULL;
    }
}

void omd_uavcan_add_sub(struct omd_uavcan_instance_s* instance, struct omd_uavcan_message_subscription_s* sub) {
    sub->next = NULL;

    struct omd_uavcan_message_subscription_s** next_ptr = &instance->message_subscription_list;
    while (*next_ptr) {
        next_ptr = &((*next_ptr)->next);
    }
    *next_ptr = sub;
}

void omd_uavcan_broadcast(struct omd_uavcan_instance_s* instance, const struct omd_uavcan_message_descriptor_s* msg_descriptor, uint16_t data_type_id, uint8_t priority, void* msg_data) {
    if (!instance || !msg_descriptor || !msg_descriptor->serializer_func || !msg_data) {
        return;
    }

    void* serialized_msg_data = chHeapAlloc(instance->outgoing_message_heap, msg_descriptor->max_serialized_size);
    if (!serialized_msg_data) {
        return;
    }

    msg_descriptor->serializer_func(msg_data, serialized_msg_data);
    canardBroadcast(instance->canard, msg_descriptor->data_type_signature, data_type_id, );
    chHeapFree(serialized_msg_data);
}

void omd_uavcan_request(struct omd_uavcan_instance_s* instance, uint8_t destination_node_id, uint64_t data_type_signature, uint16_t data_type_id, uint8_t* transfer_id, uint8_t priority, void* payload, uint16_t payload_len) {

}

void omd_uavcan_respond(struct omd_uavcan_instance_s* instance, uint8_t destination_node_id, uint64_t data_type_signature, uint16_t data_type_id, uint8_t* transfer_id, uint8_t priority, void* payload, uint16_t payload_len) {

}

static THD_FUNCTION(omd_uavcan_rx_thd_func, arg) {
    (void)arg;
    struct omd_uavcan_instance_s* instance;

    // wait for start message
    {
        thread_t* tp = chMsgWait();
        instance = (struct omd_uavcan_instance_s*)chMsgGet(tp);
        chMsgRelease(tp, MSG_OK);
    }

    if (!instance) {
        chThdExit(0);
    }

    while (true) {
        chBSemWait(&instance->tx_thread_semaphore);
        omd_uavcan_transmit_sync(instance);
    }

    chThdExit(0);
}

static THD_FUNCTION(omd_uavcan_rx_thd_func, arg) {
    (void)arg;
    struct omd_uavcan_instance_s* instance;

    // wait for start message
    {
        thread_t* tp = chMsgWait();
        instance = (struct omd_uavcan_instance_s*)chMsgGet(tp);
        chMsgRelease(tp, MSG_OK);
    }

    if (!instance) {
        chThdExit(0);
    }

    while (true) {
        CANRxFrame chibios_frame;
        if (canReceiveTimeout(instance->can_dev, CAN_ANY_MAILBOX, &chibios_frame, TIME_INFINITE)) {
            uint64_t timestamp = micros64();
            CanardCANFrame canard_frame = convert_CANRxFrame_to_CanardCANFrame(&chibios_frame);

            chMtxLock(&instance->canard_mtx);
            canardHandleRxFrame(&instance->canard, &canard_frame, timestamp);
            chMtxUnlock(&instance->canard_mtx);
        }
    }
}

void omd_uavcan_transmit_async(struct omd_uavcan_instance_s* instance) {
    if (!instance) {
        return;
    }

    // TODO: transmit thread should inherit calling thread priority
    chBSemSignal(&instance->tx_thread_semaphore);
}

void omd_uavcan_transmit_sync(struct omd_uavcan_instance_s* instance) {
    if (!instance) {
        return;
    }

    chMtxLock(&instance->tx_mtx);
    const CanardCANFrame* canard_frame;
    while (true) {
        chMtxLock(&instance->canard_mtx);
        canard_frame = canardPeekTxQueue(&instance->canard);
        chMtxUnlock(&instance->canard_mtx);

        if (!canard_frame) {
            break;
        }

        CANTxFrame chibios_frame = convert_CanardCANFrame_to_CANTxFrame(canard_frame);

        if (canTransmitTimeout(instance->can_dev, CAN_ANY_MAILBOX, &chibios_frame, TIME_INFINITE) == MSG_OK) {
            chMtxLock(&instance->canard_mtx);
            canardPopTxQueue(&instance->canard);
            chMtxUnlock(&instance->canard_mtx);
        }
    }
    chMtxUnlock(&instance->tx_mtx);
}

static CanardCANFrame convert_CANRxFrame_to_CanardCANFrame(const CANRxFrame* chibios_frame) {
    CanardCANFrame ret;
    if (chibios_frame->IDE) {
        ret.id = chibios_frame->EID | CANARD_CAN_FRAME_EFF;
    } else {
        ret.id = chibios_frame->SID;
    }

    if (chibios_frame->RTR) {
        ret.id |= CANARD_CAN_FRAME_RTR;
    }

    ret.data_len = chibios_frame->DLC;
    memcpy(ret.data, chibios_frame->data8, ret.data_len);
    return ret;
}

static CANTxFrame convert_CanardCANFrame_to_CANTxFrame(const CanardCANFrame* canard_frame) {
    CANTxFrame ret;
    ret.IDE = (canard_frame->id & CANARD_CAN_FRAME_EFF) != 0;
    ret.RTR = (canard_frame->id & CANARD_CAN_FRAME_RTR) != 0;
    if (ret.IDE) {
        ret.EID = canard_frame->id & CANARD_CAN_EXT_ID_MASK;
    } else {
        ret.SID = canard_frame->id & CANARD_CAN_STD_ID_MASK;
    }
    ret.DLC = canard_frame->data_len;
    memcpy(ret.data8, canard_frame->data, ret.DLC);
    return ret;
}

struct omd_uavcan_message_writer_func_args {
    struct omd_uavcan_instance_s* omd_uavcan_instance;
    CanardRxTransfer* transfer;
    omd_uavcan_deserializer_func_ptr deserializer_func;
};

static void omd_uavcan_message_writer_func(size_t msg_size, void* write_buf, void* ctx) {
    (void)msg_size;
    struct omd_uavcan_message_writer_func_args* args = ctx;
    struct omd_uavcan_deserialized_message_s* deserialized_message = write_buf;
    deserialized_message->omd_uavcan_instance = args->omd_uavcan_instance;
    deserialized_message->data_type_id = args->transfer->data_type_id;
    deserialized_message->transfer_id = args->transfer->transfer_id;
    deserialized_message->priority = args->transfer->priority;
    deserialized_message->source_node_id = args->transfer->source_node_id;
    args->deserializer_func(args->transfer, deserialized_message->msg);
}

static void omd_uavcan_on_transfer_rx(CanardInstance* canard, CanardRxTransfer* transfer) {
    if (!canard || !transfer) {
        return;
    }

    struct omd_uavcan_instance_s* instance = canardGetUserReference(canard);

    struct omd_uavcan_message_subscription_s* message_subscription = instance->message_subscription_list;
    while (message_subscription) {
        if (transfer->data_type_id == message_subscription->data_type_id && transfer->transfer_type == message_subscription->transfer_type && message_subscription->deserializer_func) {
            struct omd_uavcan_message_writer_func_args writer_args = { instance, transfer, message_subscription->deserializer_func };
            pubsub_publish_message(message_subscription->pubsub_topic, message_subscription->deserialized_size, omd_uavcan_message_writer_func, &writer_args);
        }

        message_subscription = message_subscription->next;
    }
}

static bool omd_uavcan_should_accept_transfer(const CanardInstance* canard, uint64_t* out_data_type_signature, uint16_t data_type_id, CanardTransferType transfer_type, uint8_t source_node_id) {
    (void)source_node_id;
    if (!canard || !out_data_type_signature) {
        return false;
    }

    struct omd_uavcan_instance_s* instance = canardGetUserReference((CanardInstance*)canard);

    struct omd_uavcan_message_subscription_s* message_subscription = instance->message_subscription_list;
    while (message_subscription) {
        if (data_type_id == message_subscription->data_type_id && transfer_type == message_subscription->transfer_type && !memcmp(&message_subscription->data_type_signature, out_data_type_signature, sizeof(uint64_t))) {
            return true;
        }

        message_subscription = message_subscription->next;
    }

    return false;
}