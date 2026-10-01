#include "control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static pwm_health_t good(void) {
    pwm_health_t h = {0};
    h.usb_connected = h.clocks_ok = h.adc_valid = h.adc_recent = h.adc_plausible = true;
    h.gpu_capture_valid = h.gpu_capture_recent = h.option_bytes_verified = h.pwm_source_ready = true;
    h.adc_mv = 800; h.gpu_ccr=80; h.vrm_ccr=80;
    return h;
}
static char response[256];
static void cmd(pwm_control_t *c, uint32_t now, const char *body) {
    char line[256];
    (void)snprintf(line, sizeof(line), "%lu %lu %s", (unsigned long)c->session,
                   (unsigned long)(c->last_seq + 1), body);
    ctrl_command(c, line, response, sizeof(response), now);
}
static void ok(pwm_control_t *c, uint32_t now, const char *body) {
    cmd(c, now, body);
    if (strncmp(response, "OK ", 3)) fprintf(stderr, "%s -> %s\n", body, response);
    assert(!strncmp(response, "OK ", 3));
}
static void bad(pwm_control_t *c, uint32_t now, const char *body) {
    cmd(c, now, body);
    assert(!strncmp(response, "ERR ", 4));
}
static void start(pwm_control_t *c, uint32_t now) {
    pwm_health_t h = good();
    ctrl_init(c);
    assert(c->state == PWM_DISARMED);
    pwm_outputs_t o = ctrl_tick(c, now, &h);
    assert(!o.permit && !o.arm_request && !o.software_config_valid);
    ctrl_command(c, "HELLO 12345", response, sizeof(response), now);
    assert(!strcmp(response, "OK HELLO 1 12345"));
}
static void profile(pwm_control_t *c, uint32_t now) {
    /* SYNTHETIC TEST VALUES ONLY: these are not board calibration. */
    ok(c, now, "PROFILE 200 16 1100 224 1000000 0 700 900 100 1200");
}
static void ready(pwm_control_t *c, uint32_t now) {
    start(c, now); profile(c, now);
#if PWMVID_ALLOW_FIXTURE == 1
    ok(c, now, "QUALIFY FIXTURE_ISOLATED");
#else
    bad(c, now, "QUALIFY FIXTURE_ISOLATED");
#endif
    ok(c, now, "SETMV 650");
}
#if PWMVID_ALLOW_FIXTURE == 1
static void arm(pwm_control_t *c, uint32_t now) {
    pwm_health_t h = good();
    ready(c, now); ok(c,now,"PREARM"); ok(c, now, "ARM 300");
    assert(c->state == PWM_ARMING);
    pwm_outputs_t o = ctrl_tick(c, now, &h);
    assert(o.permit && o.arm_request);
    assert(!ctrl_tick(c, now, &h).arm_request);
    ctrl_arm_result(c, true, now);
    assert(c->state == PWM_ARMED);
}
#endif
static void test_parser(void) {
    pwm_control_t c; start(&c, 100);
    const char *invalid[] = { "", " ", "HELLO", "HELLO -1", "HELLO 4294967296", "HELLO 1 x",
        "1 1", "1 -1 STATUS", "1 4294967296 STATUS", "1 1 STATUS\n", "1\t1 STATUS",
        "2 1 STATUS", "1 0 STATUS", "1 1 STATUS extra", "1 2 NOPE", "1 3 PROFILE 1" };
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        ctrl_command(&c, invalid[i], response, sizeof(response), 100);
        assert(!strncmp(response, "ERR ", 4));
        assert(c.state == PWM_DISARMED);
    }
    ctrl_command(&c, "1 4 STATUS", response, sizeof(response), 100);
    assert(!strncmp(response, "OK STATUS", 9));
    ctrl_command(&c, "1 4 STATUS", response, sizeof(response), 100);
    assert(!strcmp(response, "ERR SEQUENCE"));
    char longline[300]; memset(longline, 'X', sizeof(longline)-1); longline[299]=0;
    ctrl_command(&c, longline, response, sizeof(response), 100);
    assert(!strcmp(response, "ERR LINE"));
    ctrl_command(&c, NULL, response, sizeof(response), 100);
    assert(!strcmp(response, "ERR LINE"));
    char tiny[1]={'x'};
    ctrl_command(&c, "1 5 STATUS", tiny, sizeof(tiny), 100); assert(tiny[0] == 0);
    ctrl_command(&c, "1 6 STATUS", NULL, 0, 100);
    ctrl_command(&c, "1 4294967295 STATUS", response, sizeof(response), 100);
    assert(!strncmp(response, "OK STATUS", 9));
    ctrl_command(&c, "1 1 STATUS", response, sizeof(response), 100);
    assert(!strcmp(response, "ERR SEQUENCE"));
    ctrl_command(&c, "HELLO 2", response, sizeof(response), 100);
    assert(c.session == 2 && c.last_seq == 0);
    ctrl_command(&c, "1 7 ARM 300", response, sizeof(response), 100);
    assert(!strcmp(response, "ERR SESSION"));
    c.session = UINT32_MAX;
    ctrl_command(&c, "HELLO 2", response, sizeof(response), 100);
    assert(!strcmp(response, "ERR SESSION_EXHAUSTED") && !c.session_valid);
}
static void test_calibration_exact_math(void) {
    pwm_control_t c; start(&c,1);profile(&c,1);
    for(uint32_t gain=100000;gain<=10000000;gain+=7919) {
        c.profile.gain_ppm=gain;
        for(uint32_t x=0;x<=65535;x+=97) {
            c.health.adc_mv=(uint16_t)x;
            assert(ctrl_calibrated_mv(&c)==(int32_t)(((uint64_t)x*gain)/1000000u));
        }
    }
    c.health.adc_mv=65535;c.profile.gain_ppm=10000000;
    assert(ctrl_calibrated_mv(&c)==655350);
}
static void test_profile_mapping(void) {
    pwm_control_t c; ready(&c, 10);
    const char *bad_profiles[] = {
        "PROFILE 99 16 1100 224 1000000 0 700 900 100 1200",
        "PROFILE 200 16 1201 224 1000000 0 700 900 100 1200",
        "PROFILE 200 16 200 224 1000000 0 700 900 100 1200",
        "PROFILE 200 15 1100 224 1000000 0 700 900 100 1200",
        "PROFILE 200 16 1100 225 1000000 0 700 900 100 1200",
        "PROFILE 200 16 1100 16 1000000 0 700 900 100 1200",
        "PROFILE 200 16 1100 224 99999 0 700 900 100 1200",
        "PROFILE 200 16 1100 224 10000001 0 700 900 100 1200",
        "PROFILE 200 16 1100 224 1000000 1201 700 900 100 1200",
        "PROFILE 200 16 1100 224 1000000 0 900 700 100 1200",
        "PROFILE 200 16 1100 224 1000000 0 700 900 900 700",
        "PROFILE 200 16 1100 224 1000000 0 700 900 99 1200",
        "PROFILE 200 65552 1100 224 1000000 0 700 900 100 1200"
    };
    for (unsigned i=0; i<sizeof(bad_profiles)/sizeof(bad_profiles[0]); ++i) {
        bad(&c,10,bad_profiles[i]); assert(c.profile.ccr1 == 16);
    }
    bad(&c,10,"SETMV 199"); bad(&c,10,"SETMV 1101");
    bad(&c,10,"SETMV -1"); bad(&c,10,"SETMV 4294967296");
    ok(&c,10,"SETMV 200"); assert(c.requested_ccr==16);
    ok(&c,10,"SETMV 1100"); assert(c.requested_ccr==224);
    ok(&c,10,"SETMV 650"); assert(c.requested_ccr==120);
    ok(&c,10,"PROFILE 200 224 1100 16 1000000 -10 700 900 100 1200");
    assert(ctrl_calibrated_mv(&c)==790);
    ok(&c,10,"SETMV 200"); assert(c.requested_ccr==224);
    ok(&c,10,"SETMV 1100"); assert(c.requested_ccr==16);
    ok(&c,10,"SETMV 650"); assert(c.requested_ccr==120);
    bad(&c,10,"SETCCR 0"); bad(&c,10,"SETCCR 15"); bad(&c,10,"SETCCR 225");
#if PWMVID_ALLOW_FIXTURE == 1
    ok(&c,10,"SETCCR 16"); ok(&c,10,"SETCCR 224");
    ok(&c,10,"DISARM"); bad(&c,10,"SETCCR 16");
#else
    bad(&c,10,"SETCCR 16"); bad(&c,10,"ARM 300");
    assert(!ctrl_tick(&c,10,NULL).permit);
#endif
}
#if PWMVID_ALLOW_FIXTURE == 1
static void test_arming_requirements(void) {
    pwm_control_t c; pwm_health_t h;
    start(&c,100); bad(&c,100,"ARM 300");
    profile(&c,100); ok(&c,100,"SETMV 650"); bad(&c,100,"ARM 300");
    ok(&c,100,"QUALIFY FIXTURE_ISOLATED");
    bad(&c,100,"ARM 0"); bad(&c,100,"ARM 501"); bad(&c,100,"ARM 4294967296");
    ready(&c,100); c.profile_valid=false; bad(&c,100,"ARM 300");
    ready(&c,100); c.target_valid=false; bad(&c,100,"ARM 300");
    ready(&c,100); bad(&c,121,"ARM 300");
    for (unsigned i=0; i<14; ++i) {
        ready(&c,100); h=good();
        switch(i) {
            case 0:h.usb_connected=false;break; case 1:h.clocks_ok=false;break;
            case 2:h.adc_valid=false;break; case 3:h.adc_recent=false;break;
            case 4:h.adc_plausible=false;break; case 5:h.gpu_capture_valid=false;break;
            case 6:h.gpu_capture_recent=false;break; case 7:h.break_active=true;break;
            case 8:h.comparator_fault=true;break; case 9:h.option_bytes_verified=false;break;
            case 10:h.pwm_source_ready=false;break; case 11:h.run_feedback=true;break;
            case 12:h.adc_mv=699;break; case 13:h.adc_mv=901;break;
        }
        ctrl_tick(&c,100,&h); bad(&c,100,"ARM 300");
        assert(!ctrl_tick(&c,100,&h).permit);
    }
    ready(&c,100); ctrl_fault_isr(&c); bad(&c,100,"ARM 300"); assert(c.state==PWM_FAULT);
    ready(&c,100); ok(&c,100,"PREARM"); ok(&c,100,"ARM 300"); ctrl_arm_result(&c,true,100);
    assert(c.state==PWM_ARMING); /* No success accepted before request delivered. */
    h=good(); assert(ctrl_tick(&c,100,&h).arm_request); ctrl_arm_result(&c,false,100);
    assert(c.state==PWM_FAULT && c.fault==PWM_FAULT_ARM);
}
static void test_early_run_loss(void) {
    pwm_control_t c; arm(&c,100);
    /* The hardware success acknowledgement supersedes the old low-RUN
     * pre-arm snapshot even before a fresh health sample arrives. */
    ok(&c,100,"STATUS"); assert(c.state==PWM_ARMED);
    pwm_health_t h=good(); h.run_feedback=false;
    assert(!ctrl_tick(&c,101,&h).permit);
    assert(c.state==PWM_FAULT && c.fault==PWM_FAULT_HEALTH);
}
static void test_runtime(void) {
    pwm_control_t c; arm(&c,100); pwm_health_t h=good(); h.run_feedback=true;
    assert(ctrl_tick(&c,149,&h).permit);
    assert(!ctrl_tick(&c,150,&h).permit); /* Capture grace ends exactly at 50ms. */
    assert(c.state==PWM_FAULT);
    arm(&c,100); h=good(); h.run_feedback=h.vrm_capture_valid=h.vrm_capture_recent=true;
    assert(ctrl_tick(&c,150,&h).permit);
    ok(&c,150,"SETMV 1100"); assert(c.requested_ccr==224);
    bad(&c,150,"SETMV 1101"); assert(c.requested_ccr==224 && c.state==PWM_ARMED);
    bad(&c,150,"SETCCR 16"); /* Await fresh confirmation of preceding target. */
    h.vrm_ccr=224;h.vrm_sample_ms=151;assert(ctrl_tick(&c,151,&h).permit);
    ok(&c,151,"SETCCR 16"); assert(c.requested_ccr==16);
    h.vrm_ccr=16;h.vrm_sample_ms=152;assert(ctrl_tick(&c,152,&h).permit);
    bad(&c,152,"PROFILE 200 16 1100 224 1000000 0 700 900 100 1200");
    bad(&c,152,"QUALIFY FIXTURE_ISOLATED");
    assert(c.heartbeat_ms==100); /* Settings are not keepalives. */
    ok(&c,152,"PING"); assert(c.heartbeat_ms==152);
    assert(ctrl_tick(&c,451,&h).permit); assert(!ctrl_tick(&c,452,&h).permit);
    assert(c.fault==PWM_FAULT_LEASE); bad(&c,452,"PING"); bad(&c,452,"ARM 300");
    arm(&c,UINT32_MAX-100u); h=good(); h.run_feedback=h.vrm_capture_valid=h.vrm_capture_recent=true;
    assert(ctrl_tick(&c,198,&h).permit); assert(!ctrl_tick(&c,199,&h).permit);
    assert(c.fault==PWM_FAULT_LEASE);
    arm(&c,100); h=good(); h.adc_mv=1300; assert(!ctrl_tick(&c,101,&h).permit);
    arm(&c,100); h=good(); h.run_feedback=h.vrm_capture_valid=h.vrm_capture_recent=true;
    h.gpu_capture_recent=false; assert(!ctrl_tick(&c,101,&h).permit);
    arm(&c,100); ctrl_usb_lost(&c); assert(c.state==PWM_FAULT && !c.session_valid);
    assert(!ctrl_tick(&c,101,NULL).permit);
    arm(&c,100); ctrl_fault_isr(&c); assert(!ctrl_tick(&c,100,NULL).permit);
    assert(c.fault==PWM_FAULT_ISR);
    arm(&c,100); ctrl_command(&c,"HELLO 22",response,sizeof(response),100);
    assert(c.state==PWM_DISARMED && !c.qualified && !c.profile_valid);
    bad(&c,100,"ARM 300");
    arm(&c,100); ok(&c,100,"DISARM"); assert(c.state==PWM_DISARMED && !c.qualified);
    bad(&c,100,"ARM 300");
}
static void test_reset_before_option_qualification(void) {
    pwm_control_t c; start(&c,100); pwm_health_t h=good();
    h.option_bytes_verified=false; ctrl_fault_isr(&c);ctrl_tick(&c,101,&h);
    ok(&c,101,"RESET");assert(c.state==PWM_DISARMED && !c.qualified && !c.target_valid);
    profile(&c,101);ok(&c,101,"QUALIFY FIXTURE_ISOLATED");ok(&c,101,"PREARM");
    bad(&c,101,"ARM 300");assert(!ctrl_tick(&c,101,&h).permit);
}
static void test_fault_reset(void) {
    pwm_control_t c; arm(&c,100); pwm_health_t h=good();
    ctrl_fault_isr(&c); ctrl_tick(&c,101,&h);
    assert(c.state==PWM_FAULT && !c.qualified);
    bad(&c,101,"ARM 300");
    h.break_active=true; ctrl_tick(&c,102,&h); bad(&c,102,"RESET");
    h.break_active=false; h.run_feedback=true; ctrl_tick(&c,103,&h); bad(&c,103,"RESET");
    h.run_feedback=false; ctrl_tick(&c,104,&h); ok(&c,104,"RESET");
    assert(c.state==PWM_DISARMED && c.fault==PWM_FAULT_NONE && !c.fault_pending);
    bad(&c,104,"ARM 300"); ok(&c,104,"QUALIFY FIXTURE_ISOLATED");
    ok(&c,104,"SETMV 650"); ok(&c,104,"PREARM"); ok(&c,104,"ARM 300");
    assert(ctrl_tick(&c,104,&h).arm_request); ctrl_fault_isr(&c); ctrl_arm_result(&c,true,104);
    assert(c.state==PWM_FAULT); /* Late success may never override fault. */
    ctrl_command(&c,"STOP",response,sizeof(response),105);
    assert(c.state==PWM_FAULT);
    ctrl_command(&c,"HELLO 5",response,sizeof(response),105);
    assert(c.state==PWM_FAULT);
}
#endif
int main(void) {
    test_parser(); test_profile_mapping(); test_calibration_exact_math();
#if PWMVID_ALLOW_FIXTURE == 1
    test_arming_requirements(); test_early_run_loss(); test_runtime(); test_fault_reset(); test_reset_before_option_qualification();
#endif
    puts(PWMVID_ALLOW_FIXTURE == 1 ? "fixture core tests passed" : "locked core tests passed");
    return 0;
}
