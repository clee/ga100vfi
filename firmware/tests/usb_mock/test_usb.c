static void host_dtr(unsigned value) {
    USBD_SetupReqTypedef request = {.wValue = (uint16_t)value};
    assert(cdc_control(CDC_SET_CONTROL_LINE_STATE, (uint8_t *)&request, 0U) == USBD_OK);
}
static void fresh(void) {
    (void)pcd;
    initialized = true; configured = false; suspended = false; dtr = false;
    cdc_allocated = false;
    assert(USBD_static_malloc(sizeof(cdc_storage)) == &cdc_storage);
    assert(USBD_static_malloc(1) == NULL);
    assert(cdc_init() == USBD_OK);
    device.dev_state = USBD_STATE_CONFIGURED;
    host_dtr(1);
    assert(usb_transport_connected());
}
static void feed(const void *buffer, size_t length) {
    const uint8_t *bytes = buffer;
    for (size_t i=0; i<length;) {
        uint32_t n = (uint32_t)((length-i > 64) ? 64 : length-i);
        assert(cdc_receive((uint8_t *)(bytes+i), &n) == USBD_OK); i+=n;
    }
}
int main(void) {
    char out[256];
    /* A reset during first enumeration drops permission but is not a lost
     * client session and must not light the latched-fault indication. */
    session_lost(); assert(safety_calls==1 && fault_calls==0);
    fresh();
    uint8_t coding[7];
    assert(cdc_control(CDC_GET_LINE_CODING,coding,7)==USBD_OK);
    assert(coding[0]==0 && coding[1]==0xC2 && coding[2]==1 && coding[6]==8);
    assert(cdc_control(CDC_SET_LINE_CODING,coding,6)==USBD_FAIL);
    assert(cdc_control(0xFF,coding,7)==USBD_FAIL);
    /* Exercise many ring wraps while consuming the stream. */
    for (unsigned i=0;i<2000;++i) {
        feed("wrap\n",5);
        assert(usb_transport_poll_line(out,sizeof out) && !strcmp(out,"wrap"));
    }
    feed("status\r\nhello\n", 14);
    assert(usb_transport_poll_line(out,sizeof out) && !strcmp(out,"status"));
    assert(usb_transport_poll_line(out,sizeof out) && !strcmp(out,"hello"));
    assert(!usb_transport_poll_line(out,sizeof out));
    feed("frag",4); assert(!usb_transport_poll_line(out,sizeof out));
    feed("mented\n",7); assert(usb_transport_poll_line(out,sizeof out) && !strcmp(out,"fragmented"));
    feed("old fragment",12); assert(!usb_transport_poll_line(out,sizeof out));
    uint32_t epoch=loss_count; unsigned safe=safety_calls, faults=fault_calls;
    host_dtr(0); assert(loss_count==epoch+1 && safety_calls==safe+1 && fault_calls==faults+1 && !usb_transport_connected());
    feed("ignored\n",8); host_dtr(1); feed("new\n",4);
    assert(usb_transport_poll_line(out,sizeof out) && !strcmp(out,"new"));
    assert(!usb_transport_poll_line(out,sizeof out));
    char maximal[256]; memset(maximal,'x',255); maximal[255]='\n';
    feed(maximal,sizeof maximal); assert(usb_transport_poll_line(out,sizeof out) && strlen(out)==255);
    uint32_t overflow=rx_overflow_count;
    char excessive[257]; memset(excessive,'x',256); excessive[256]='\n';
    feed(excessive,sizeof excessive); assert(rx_overflow_count==overflow+1 && !usb_transport_connected());
    assert(!usb_transport_poll_line(out,sizeof out)); host_dtr(1);
    feed("valid\n",6); assert(usb_transport_poll_line(out,sizeof out) && !strcmp(out,"valid"));
    uint8_t bad[]={'A',0,'B','\n'}; overflow=rx_overflow_count;
    feed(bad,sizeof bad); assert(rx_overflow_count==overflow+1 && !usb_transport_connected()); host_dtr(1);
    feed("cannot truncate\n",16); overflow=rx_overflow_count;
    assert(!usb_transport_poll_line(out,4) && rx_overflow_count==overflow+1 && !usb_transport_connected());
    host_dtr(1); overflow=rx_overflow_count;
    char flood[1200]; for(unsigned i=0;i<sizeof flood;++i) flood[i] = i%2 ? '\n':'x';
    feed(flood,sizeof flood); assert(rx_overflow_count==overflow+1 && !usb_transport_connected());
    host_dtr(1); assert(!usb_transport_poll_line(out,sizeof out));
    assert(usb_transport_write("test")); assert(!memcmp(tx_packet,"test",4));
    cdc_storage.TxState=1; uint32_t drops=tx_drop_count;
    assert(!usb_transport_write("overwrite") && tx_drop_count==drops+1 && !memcmp(tx_packet,"test",4));
    cdc_storage.TxState=0; char txmax[USB_TRANSPORT_TX_SIZE+1]; memset(txmax,'y',USB_TRANSPORT_TX_SIZE); txmax[USB_TRANSPORT_TX_SIZE]=0;
    assert(usb_transport_write(txmax)); assert(!memcmp(tx_packet,txmax,USB_TRANSPORT_TX_SIZE));
    char txlong[USB_TRANSPORT_TX_SIZE+2]; memset(txlong,'z',USB_TRANSPORT_TX_SIZE+1); txlong[USB_TRANSPORT_TX_SIZE+1]=0;
    drops=tx_drop_count; assert(!usb_transport_write(txlong) && tx_drop_count==drops+1);
    assert(usb_transport_write("")); assert(!usb_transport_write(NULL));
    /* The real callback additionally forwards suspend/resume to the USB stack. */
    feed("pre",3); assert(!usb_transport_poll_line(out,sizeof out));
    session_lost(); suspended=true; assert(!usb_transport_connected());
    suspended=false; assert(!usb_transport_connected()); host_dtr(1);
    feed("post\n",5); assert(usb_transport_poll_line(out,sizeof out) && !strcmp(out,"post"));
    /* Lock restores a pre-existing stronger mask, and never enables IRQs itself. */
    fake_basepri=16; assert(usb_transport_connected()); assert(fake_basepri==16); fake_basepri=0;
    assert(cdc_deinit()==USBD_OK && !usb_transport_connected());
    USBD_static_free(&cdc_storage); assert(!cdc_allocated);
    assert(USBD_static_malloc(sizeof(cdc_storage)+1)==NULL);
    puts("USB queue/session mock tests PASS");
    return 0;
}
