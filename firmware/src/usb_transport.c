/* PWMVID prototype USB FS CDC transport. No heap, blocking TX or UART bridge. */
#include "usb_transport.h"
#include "usbd_desc.h"
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_cdc.h"
#include "stm32g4xx_hal_pcd.h"
#include "stm32g4xx_hal_pcd_ex.h"

#if USE_HAL_PCD_REGISTER_CALLBACKS != 0U
#error "This transport supplies the HAL weak callbacks; use callback registration = 0"
#endif
#if defined(USE_USBD_COMPOSITE)
#error "This transport is a single CDC device, not a composite stack"
#endif

#define USB_IRQ_PRIORITY 2U
#define RX_MASK (USB_TRANSPORT_RX_RING_SIZE - 1U)
#define RX_POLL_BUDGET USB_TRANSPORT_LINE_SIZE

_Static_assert((USB_TRANSPORT_RX_RING_SIZE & RX_MASK) == 0U,
               "USB RX ring size must be a power of two");
_Static_assert(CDC_DATA_FS_MAX_PACKET_SIZE == 64U, "FS endpoint buffer size changed");
_Static_assert(CDC_IN_EP == 0x81U && CDC_OUT_EP == 0x01U && CDC_CMD_EP == 0x82U,
               "Update the PMA map if endpoint numbers change");

extern void app_usb_lost_isr(bool had_session);

static USBD_HandleTypeDef device;
static PCD_HandleTypeDef pcd;
/* Natural type alignment is sufficient for the middleware's uint32_t fields. */
static USBD_CDC_HandleTypeDef cdc_storage;
static bool cdc_allocated;
static volatile bool initialized;
static volatile bool configured;
static volatile bool suspended;
static volatile bool dtr;
static volatile uint32_t loss_count;
static volatile uint32_t rx_overflow_count;
static volatile uint32_t tx_drop_count;
static uint8_t rx_packet[CDC_DATA_FS_MAX_PACKET_SIZE] __attribute__((aligned(4)));
static uint8_t tx_packet[USB_TRANSPORT_TX_SIZE] __attribute__((aligned(4)));
static uint8_t rx_ring[USB_TRANSPORT_RX_RING_SIZE];
static uint16_t rx_head;
static uint16_t rx_tail;
static uint16_t incoming_line_length;
static char partial_line[USB_TRANSPORT_LINE_SIZE];
static uint16_t partial_length;
static uint8_t line_coding[7] = {0x00U, 0xC2U, 0x01U, 0x00U, 0U, 0U, 8U};

/* Mask USB and lower-priority IRQs, preserving the priority-0 break path.
 * main.c must select NVIC_PRIORITYGROUP_4 before this module is initialized. */
static uint32_t usb_lock(void)
{
    uint32_t previous = __get_BASEPRI();
    __set_BASEPRI_MAX(USB_IRQ_PRIORITY << (8U - __NVIC_PRIO_BITS));
    __DSB();
    __ISB();
    return previous;
}

static void usb_unlock(uint32_t previous)
{
    __DMB();
    __set_BASEPRI(previous);
    __ISB();
}

static bool session_ready(void)
{
    return initialized && configured && !suspended && dtr &&
           device.dev_state == USBD_STATE_CONFIGURED && cdc_allocated;
}

static void clear_input(void)
{
    rx_head = 0U;
    rx_tail = 0U;
    incoming_line_length = 0U;
    partial_length = 0U;
}

/* Call before any USB stack recovery. The application must make the output
 * safe synchronously here; a main-loop flag alone is insufficient. */
static void session_lost(void)
{
    /* Initial USB bus reset/deinit can precede any host session. Always drop
     * physical permission, but latch USB loss only after DTR was asserted. */
    app_usb_lost_isr(dtr);
    dtr = false;
    ++loss_count;
    clear_input();
}

static void receive_fault(void)
{
    session_lost();
    ++rx_overflow_count;
}

void *USBD_static_malloc(uint32_t size)
{
    if (cdc_allocated || size > sizeof(cdc_storage)) {
        return NULL;
    }
    cdc_allocated = true;
    memset(&cdc_storage, 0, sizeof(cdc_storage));
    return &cdc_storage;
}

void USBD_static_free(void *pointer)
{
    if (pointer == &cdc_storage) {
        cdc_allocated = false;
    }
}

static int8_t cdc_init(void)
{
    clear_input();
    dtr = false;
    suspended = false;
    configured = true;
    (void)USBD_CDC_SetTxBuffer(&device, tx_packet, 0U);
    (void)USBD_CDC_SetRxBuffer(&device, rx_packet);
    return (int8_t)USBD_OK;
}

static int8_t cdc_deinit(void)
{
    session_lost();
    configured = false;
    return (int8_t)USBD_OK;
}

static int8_t cdc_control(uint8_t command, uint8_t *buffer, uint16_t length)
{
    switch (command) {
    case CDC_SET_LINE_CODING:
        if (length != sizeof(line_coding)) {
            return (int8_t)USBD_FAIL;
        }
        memcpy(line_coding, buffer, sizeof(line_coding));
        break;
    case CDC_GET_LINE_CODING:
        if (length != sizeof(line_coding)) {
            return (int8_t)USBD_FAIL;
        }
        memcpy(buffer, line_coding, sizeof(line_coding));
        break;
    case CDC_SET_CONTROL_LINE_STATE: {
        /* Cube passes the parsed setup packet for zero-length class requests. */
        const USBD_SetupReqTypedef *request = (const USBD_SetupReqTypedef *)buffer;
        if ((request->wValue & 1U) == 0U) {
            session_lost();
        } else if (configured && !suspended) {
            if (!dtr) {
                clear_input();
            }
            dtr = true;
        }
        break;
    }
    case CDC_SEND_BREAK:
        /* Virtual serial framing has no physical UART to drive. */
        break;
    default:
        return (int8_t)USBD_FAIL;
    }
    return (int8_t)USBD_OK;
}

static int8_t cdc_receive(uint8_t *buffer, uint32_t *length)
{
    if (*length > sizeof(rx_packet)) {
        receive_fault();
    } else if (session_ready()) {
        for (uint32_t i = 0U; i < *length; ++i) {
            uint8_t byte = buffer[i];
            uint16_t next = (uint16_t)((rx_head + 1U) & RX_MASK);
            bool terminator = byte == '\r' || byte == '\n';
            if (next == rx_tail ||
                (!terminator && byte != '\t' && (byte < 32U || byte > 126U)) ||
                (!terminator && incoming_line_length >= USB_TRANSPORT_LINE_SIZE - 1U)) {
                receive_fault();
                break; /* Discard the rest of this packet and require fresh DTR. */
            }
            incoming_line_length = terminator ? 0U : (uint16_t)(incoming_line_length + 1U);
            rx_ring[rx_head] = byte;
            rx_head = next;
        }
    }
    /* Always re-arm, including while DTR is low. Closed-session bytes are dropped. */
    (void)USBD_CDC_SetRxBuffer(&device, rx_packet);
    (void)USBD_CDC_ReceivePacket(&device);
    return (int8_t)USBD_OK;
}

static USBD_CDC_ItfTypeDef cdc_interface = {
    .Init = cdc_init,
    .DeInit = cdc_deinit,
    .Control = cdc_control,
    .Receive = cdc_receive
};

bool usb_transport_init(void)
{
    if (initialized) {
        return true;
    }
    clear_input();
    configured = false;
    suspended = false;
    dtr = false;
    if (USBD_Init(&device, &pwmvid_usb_descriptors, DEVICE_FS) != USBD_OK) {
        return false;
    }
    if (USBD_RegisterClass(&device, &USBD_CDC) != USBD_OK ||
        USBD_CDC_RegisterInterface(&device, &cdc_interface) != USBD_OK) {
        (void)USBD_DeInit(&device);
        return false;
    }
    /* Publish before enabling the pull-up: enumeration may interrupt Start(). */
    initialized = true;
    if (USBD_Start(&device) != USBD_OK) {
        initialized = false;
        (void)USBD_DeInit(&device);
        return false;
    }
    return true;
}

bool usb_transport_connected(void)
{
    uint32_t mask = usb_lock();
    bool result = session_ready();
    usb_unlock(mask);
    return result;
}

bool usb_transport_poll_line(char *line, size_t capacity)
{
    if (line == NULL || capacity == 0U) {
        return false;
    }
    line[0] = '\0';
    uint32_t mask = usb_lock();
    if (!session_ready()) {
        usb_unlock(mask);
        return false;
    }
    for (uint32_t work = 0U; work < RX_POLL_BUDGET && rx_tail != rx_head; ++work) {
        char byte = (char)rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1U) & RX_MASK);
        if (byte == '\r' || byte == '\n') {
            if (partial_length == 0U) {
                continue; /* CRLF and empty lines do not yield empty commands. */
            }
            if ((size_t)partial_length + 1U > capacity) {
                receive_fault();
                usb_unlock(mask);
                return false;
            }
            memcpy(line, partial_line, partial_length);
            line[partial_length] = '\0';
            partial_length = 0U;
            usb_unlock(mask);
            return true;
        }
        /* Also check here, so a future receiver change cannot cause truncation. */
        if (partial_length >= sizeof(partial_line) - 1U) {
            receive_fault();
            usb_unlock(mask);
            return false;
        }
        partial_line[partial_length++] = byte;
    }
    usb_unlock(mask);
    return false;
}

bool usb_transport_write(const char *text)
{
    size_t length = 0U;
    if (text != NULL) {
        while (length <= sizeof(tx_packet) && text[length] != '\0') {
            ++length;
        }
    }
    uint32_t mask = usb_lock();
    if (text == NULL || length > sizeof(tx_packet) || !session_ready() ||
        cdc_storage.TxState != 0U) {
        ++tx_drop_count;
        usb_unlock(mask);
        return false;
    }
    if (length == 0U) {
        usb_unlock(mask);
        return true;
    }
    memcpy(tx_packet, text, length);
    bool sent = USBD_CDC_SetTxBuffer(&device, tx_packet, (uint16_t)length) == USBD_OK &&
                USBD_CDC_TransmitPacket(&device) == USBD_OK;
    if (!sent) {
        ++tx_drop_count;
    }
    usb_unlock(mask);
    return sent;
}

uint32_t usb_transport_loss_count(void) { return loss_count; }
uint32_t usb_transport_rx_overflow_count(void) { return rx_overflow_count; }
uint32_t usb_transport_tx_drop_count(void) { return tx_drop_count; }

/* Device and string descriptors. The CDC configuration comes from ST middleware. */
static uint8_t device_descriptor[USB_LEN_DEV_DESC] __attribute__((aligned(4))) = {
    USB_LEN_DEV_DESC, USB_DESC_TYPE_DEVICE,
    0x00U, 0x02U, /* USB 2.0, full speed only */
    0x02U, 0x02U, 0x00U, USB_MAX_EP0_SIZE,
    LOBYTE(PWMVID_USB_VID), HIBYTE(PWMVID_USB_VID),
    LOBYTE(PWMVID_USB_PID), HIBYTE(PWMVID_USB_PID),
    0x01U, 0x00U, /* Draft revision 0.01 */
    USBD_IDX_MFC_STR, USBD_IDX_PRODUCT_STR, USBD_IDX_SERIAL_STR,
    USBD_MAX_NUM_CONFIGURATION
};
static uint8_t language_descriptor[USB_LEN_LANGID_STR_DESC] = {
    USB_LEN_LANGID_STR_DESC, USB_DESC_TYPE_STRING, 0x09U, 0x04U
};
static uint8_t string_descriptor[USBD_MAX_STR_DESC_SIZ] __attribute__((aligned(4)));
static char serial_ascii[25];

static uint8_t *get_device(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(device_descriptor);
    return device_descriptor;
}

static uint8_t *get_language(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(language_descriptor);
    return language_descriptor;
}

static uint8_t *make_string(const char *text, uint16_t *length)
{
    USBD_GetString((uint8_t *)text, string_descriptor, length);
    return string_descriptor;
}

static uint8_t *get_manufacturer(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    return make_string("PWMVID Prototype", length);
}

static uint8_t *get_product(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    return make_string("PWMVID G431CB Sequencer CDC", length);
}

static uint8_t *get_serial(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    static const char hex[] = "0123456789ABCDEF";
    uint32_t words[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
    for (uint32_t word = 0U; word < 3U; ++word) {
        for (uint32_t digit = 0U; digit < 8U; ++digit) {
            serial_ascii[word * 8U + digit] = hex[(words[word] >> (28U - digit * 4U)) & 0xFU];
        }
    }
    serial_ascii[24] = '\0';
    return make_string(serial_ascii, length);
}

USBD_DescriptorsTypeDef pwmvid_usb_descriptors = {
    .GetDeviceDescriptor = get_device,
    .GetLangIDStrDescriptor = get_language,
    .GetManufacturerStrDescriptor = get_manufacturer,
    .GetProductStrDescriptor = get_product,
    .GetSerialStrDescriptor = get_serial,
    /* Pinned CDC advertises iConfiguration=0 and both iInterface=0.
     * These unadvertised strings need no callbacks; core handles NULL. */
    .GetConfigurationStrDescriptor = NULL,
    .GetInterfaceStrDescriptor = NULL
};

/* HAL PCD / USB Device middleware bridge. */
static USBD_StatusTypeDef usb_status(HAL_StatusTypeDef status)
{
    if (status == HAL_OK) { return USBD_OK; }
    if (status == HAL_BUSY) { return USBD_BUSY; }
    return USBD_FAIL;
}

void HAL_PCD_MspInit(PCD_HandleTypeDef *handle)
{
    if (handle->Instance != USB) { return; }
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USB_CLK_ENABLE();
    GPIO_InitTypeDef pins = {0};
    pins.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    /* G431 USB_DM/DP are dedicated additional functions, not GPIO AF14.
     * Leave the GPIO paths analog/no-pull; the active USB PHY owns the pins. */
    pins.Mode = GPIO_MODE_ANALOG;
    pins.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &pins);
    NVIC_SetPriority(USB_LP_IRQn,USB_IRQ_PRIORITY);
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *handle)
{
    if (handle->Instance != USB) { return; }
    HAL_NVIC_DisableIRQ(USB_LP_IRQn);
    __HAL_RCC_USB_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_11 | GPIO_PIN_12);
    /* Do not disable GPIOA: the application uses other pins on the same port. */
}

void USB_LP_IRQHandler(void) { HAL_PCD_IRQHandler(&pcd); }

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *handle)
{
    USBD_SetupReqTypedef request;
    USBD_ParseSetupRequest(&request, (uint8_t *)handle->Setup);
    bool reject = false;
    if ((request.bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS) {
        /* Validate before Cube prepares EP0 receive storage. */
        bool line_write = request.bRequest == CDC_SET_LINE_CODING &&
                          request.bmRequest == 0x21U && request.wLength == 7U;
        bool line_read = request.bRequest == CDC_GET_LINE_CODING &&
                         request.bmRequest == 0xA1U && request.wLength == 7U;
        bool control = (request.bRequest == CDC_SET_CONTROL_LINE_STATE ||
                        request.bRequest == CDC_SEND_BREAK) &&
                       request.bmRequest == 0x21U && request.wLength == 0U;
        reject = request.wIndex != 0U || !(line_write || line_read || control);
    }
    /* There is no remote-wakeup implementation or descriptor capability. */
    if (request.bmRequest == 0x00U && request.bRequest == USB_REQ_SET_FEATURE &&
        request.wValue == USB_FEATURE_REMOTE_WAKEUP) {
        reject = true;
    }
    if (reject) {
        USBD_CtlError(&device, &request);
        return;
    }
    (void)USBD_LL_SetupStage(handle->pData, (uint8_t *)handle->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *handle, uint8_t endpoint)
{
    (void)USBD_LL_DataOutStage(handle->pData, endpoint, handle->OUT_ep[endpoint].xfer_buff);
}
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *handle, uint8_t endpoint)
{
    (void)USBD_LL_DataInStage(handle->pData, endpoint, handle->IN_ep[endpoint].xfer_buff);
}
void HAL_PCD_SOFCallback(PCD_HandleTypeDef *handle)
{
    (void)USBD_LL_SOF(handle->pData);
}
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *handle)
{
    session_lost();
    configured = false;
    suspended = false;
    (void)USBD_LL_SetSpeed(handle->pData, USBD_SPEED_FULL);
    (void)USBD_LL_Reset(handle->pData);
}
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *handle)
{
    session_lost(); /* Output safety precedes suspend bookkeeping or clock policy. */
    suspended = true;
    (void)USBD_LL_Suspend(handle->pData);
    /* No STOP/SLEEPDEEP here; the application owns clock and power policy. */
}
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *handle)
{
    suspended = false;
    (void)USBD_LL_Resume(handle->pData);
    /* dtr remains false until the host explicitly asserts it again. */
}
void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *handle)
{
    (void)USBD_LL_DevConnected(handle->pData);
}
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *handle)
{
    session_lost();
    configured = false;
    suspended = false;
    (void)USBD_LL_DevDisconnected(handle->pData);
}
void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *handle, uint8_t endpoint)
{
    (void)USBD_LL_IsoOUTIncomplete(handle->pData, endpoint);
}
void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *handle, uint8_t endpoint)
{
    (void)USBD_LL_IsoINIncomplete(handle->pData, endpoint);
}

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *handle)
{
    pcd.Instance = USB;
    pcd.Init.dev_endpoints = 8U;
    pcd.Init.speed = PCD_SPEED_FULL;
    pcd.Init.phy_itface = PCD_PHY_EMBEDDED;
    pcd.Init.Sof_enable = DISABLE;
    pcd.Init.low_power_enable = DISABLE;
    pcd.Init.lpm_enable = DISABLE;
    pcd.Init.battery_charging_enable = DISABLE;
    pcd.pData = handle;
    handle->pData = &pcd;
    if (HAL_PCD_Init(&pcd) != HAL_OK) { return USBD_FAIL; }
    /* Reserve 0x00..0x3f for eight endpoint buffer-table entries. PMA offsets
     * are byte addresses, and all single-buffer regions below are disjoint. */
    if (HAL_PCDEx_PMAConfig(&pcd, 0x00U, PCD_SNG_BUF, 0x040U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd, 0x80U, PCD_SNG_BUF, 0x080U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd, CDC_OUT_EP, PCD_SNG_BUF, 0x0C0U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd, CDC_IN_EP, PCD_SNG_BUF, 0x100U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd, CDC_CMD_EP, PCD_SNG_BUF, 0x140U) != HAL_OK) {
        (void)HAL_PCD_DeInit(&pcd);
        return USBD_FAIL;
    }
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *handle)
{
    session_lost();
    configured = false;
    initialized = false;
    return usb_status(HAL_PCD_DeInit(handle->pData));
}
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *handle)
{
    HAL_StatusTypeDef status = HAL_PCD_Start(handle->pData);
    if (status == HAL_OK) {
        /* CubeG4 enables SOFM unconditionally. CRS uses the hardware SOF
         * signal independently of this CPU interrupt mask. */
        CLEAR_BIT(pcd.Instance->CNTR, USB_CNTR_SOFM);
    }
    return usb_status(status);
}
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *handle)
{
    session_lost();
    configured = false;
    return usb_status(HAL_PCD_Stop(handle->pData));
}
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *handle, uint8_t address,
                                 uint8_t type, uint16_t max_packet)
{
    return usb_status(HAL_PCD_EP_Open(handle->pData, address, max_packet, type));
}
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *handle, uint8_t address)
{
    return usb_status(HAL_PCD_EP_Close(handle->pData, address));
}
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *handle, uint8_t address)
{
    return usb_status(HAL_PCD_EP_Flush(handle->pData, address));
}
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *handle, uint8_t address)
{
    return usb_status(HAL_PCD_EP_SetStall(handle->pData, address));
}
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *handle, uint8_t address)
{
    return usb_status(HAL_PCD_EP_ClrStall(handle->pData, address));
}
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *handle, uint8_t address)
{
    PCD_HandleTypeDef *controller = handle->pData;
    uint8_t endpoint = address & 0x7FU;
    return (address & 0x80U) != 0U ? controller->IN_ep[endpoint].is_stall :
                                  controller->OUT_ep[endpoint].is_stall;
}
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *handle, uint8_t address)
{
    return usb_status(HAL_PCD_SetAddress(handle->pData, address));
}
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *handle, uint8_t address,
                                   uint8_t *buffer, uint32_t length)
{
    return usb_status(HAL_PCD_EP_Transmit(handle->pData, address, buffer, length));
}
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *handle, uint8_t address,
                                         uint8_t *buffer, uint32_t length)
{
    return usb_status(HAL_PCD_EP_Receive(handle->pData, address, buffer, length));
}
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *handle, uint8_t address)
{
    return HAL_PCD_EP_GetRxCount(handle->pData, address);
}
void USBD_LL_Delay(uint32_t milliseconds) { HAL_Delay(milliseconds); }
