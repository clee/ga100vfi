#include "control.h"
#include <stdarg.h>
#include "text_format.h"
#include <string.h>

#ifndef PWMVID_INTERNAL_SOURCE_SELFTEST
#define PWMVID_INTERNAL_SOURCE_SELFTEST 0
#endif
#ifndef PWMVID_SEQUENCE_BENCH
#define PWMVID_SEQUENCE_BENCH 0
#endif
#if PWMVID_INTERNAL_SOURCE_SELFTEST || PWMVID_SEQUENCE_BENCH
#define PWMVID_SELFTEST_SUFFIX " selftest=1"
#else
#define PWMVID_SELFTEST_SUFFIX ""
#endif

static void reply(char *dst, size_t n, const char *fmt, ...) {
    if (!dst || !n) return;
    va_list ap;
    va_start(ap, fmt);
    (void)pwm_vsnprintf(dst, n, fmt, ap);
    va_end(ap);
}
const char *ctrl_state_name(pwm_state_t s) {
    switch (s) {
        case PWM_DISARMED: return "PASSIVE";
        case PWM_PREARM: return "PREARM";
        case PWM_ARMING: return "ARMING";
        case PWM_ARMED: return "ARMED";
        default: return "FAULT";
    }
}
static void off(pwm_control_t *c) {
    c->qualified = false;
    c->target_valid = false;
    c->arm_request_pending = false;
    c->arm_request_issued = false;
    c->lease_ms = 0;
    c->feedback_pending = false;
    c->sequence_mode = false;
    c->dfu_requested = false;
    if (c->state != PWM_FAULT) c->state = PWM_DISARMED;
}
static void fault(pwm_control_t *c, pwm_fault_t f) {
    if (c->fault == PWM_FAULT_NONE) c->fault = f;
    c->state = PWM_FAULT;
    off(c);
}
void ctrl_init(pwm_control_t *c) {
    memset(c, 0, sizeof(*c));
    c->state = PWM_DISARMED;
    c->requested_ccr = CTRL_CCR_MIN;
}
void ctrl_fault_isr(pwm_control_t *c) { ++c->fault_epoch; c->fault_pending = true; }
void ctrl_usb_lost(pwm_control_t *c) {
    c->session_valid = false;
    c->profile_valid = false;
    fault(c, PWM_FAULT_USB);
}
int32_t ctrl_calibrated_mv(const pwm_control_t *c) {
    if (!c->profile_valid) return -1;
    /* Exact quotient split avoids the ARM software 64-bit division helper.
     * Valid profile gain <=10,000,000 and uint16 ADC make every term <=2e9. */
    uint32_t x=c->health.adc_mv, gain=c->profile.gain_ppm;
    uint32_t remainder=gain%1000000u;
    uint32_t a=(x/1000u)*remainder;
    uint32_t value=x*(gain/1000000u) + 1000u*(a/1000000u) +
        ((a%1000000u)*1000u + (x%1000u)*remainder)/1000000u;
    return (int32_t)value + c->profile.offset_mv;
}
static bool base_health(const pwm_control_t *c, uint32_t now) {
    const pwm_health_t *h = &c->health;
    return c->health_valid && (uint32_t)(now - c->health_ms) <= CTRL_HEALTH_MAX_AGE_MS &&
        h->usb_connected && h->clocks_ok && h->adc_valid && h->adc_recent &&
        h->adc_plausible && h->gpu_capture_valid && h->gpu_capture_recent &&
        !h->break_active && !h->comparator_fault && h->option_bytes_verified &&
        h->pwm_source_ready && !c->fault_pending;
}
static bool rail_plausible(const pwm_control_t *c, bool loaded) {
    int32_t mv = ctrl_calibrated_mv(c);
    return loaded ? mv >= c->profile.load_min_mv && mv <= c->profile.load_max_mv :
                    mv >= c->profile.boot_min_mv && mv <= c->profile.boot_max_mv;
}
static bool config_ok(const pwm_control_t *c) {
    return PWMVID_ALLOW_FIXTURE == 1 && c->session_valid && c->qualified &&
           c->profile_valid && c->target_valid;
}
static bool feedback_matches(const pwm_control_t *c) {
    int32_t delta=(int32_t)c->health.vrm_ccr-c->requested_ccr;
    return delta>=-12 && delta<=12;
}
static void service(pwm_control_t *c, uint32_t now) {
    c->now_ms = now;
    if (c->fault_pending || c->fault_epoch != c->acknowledged_fault_epoch) fault(c, PWM_FAULT_ISR);
    if ((c->state == PWM_DISARMED || c->state == PWM_PREARM || c->state == PWM_FAULT) &&
        c->health_valid && c->health.run_feedback) fault(c, PWM_FAULT_UNEXPECTED_RUN);
    if (c->state == PWM_PREARM) {
        /* Hardware generator tracks the latest observed GPU duty behind the gate.
         * Decimated capture is a preview estimate, not phase matching. */
        if (!c->health_valid || !c->health.usb_connected || !c->health.clocks_ok ||
            !c->health.gpu_capture_valid || !c->health.gpu_capture_recent ||
            c->health.gpu_ccr < CTRL_CCR_MIN || c->health.gpu_ccr > CTRL_CCR_MAX) {
            fault(c, PWM_FAULT_HEALTH);
        } else {
            c->requested_ccr = c->health.gpu_ccr;
            c->target_valid = true;
        }
    }
    if (c->state == PWM_ARMING || c->state == PWM_ARMED) {
        if (!config_ok(c) || !base_health(c, now)) fault(c, PWM_FAULT_HEALTH);
        else if (ctrl_calibrated_mv(c) < 100 || ctrl_calibrated_mv(c) > 1200)
            fault(c, PWM_FAULT_HEALTH);
        else if ((uint32_t)(now - c->heartbeat_ms) >= c->lease_ms)
            fault(c, PWM_FAULT_LEASE);
        else if (c->state == PWM_ARMED && !c->health.run_feedback)
            fault(c, PWM_FAULT_HEALTH);
        else if (c->state == PWM_ARMED && c->sequence_mode && !rail_plausible(c, true))
            fault(c, PWM_FAULT_HEALTH);
        else if (c->state == PWM_ARMING &&
                 ((int32_t)c->requested_ccr-(int32_t)c->health.gpu_ccr > 2 ||
                  (int32_t)c->health.gpu_ccr-(int32_t)c->requested_ccr > 2 ||
                  !rail_plausible(c, false)))
            fault(c, PWM_FAULT_HEALTH);
        else if (c->state == PWM_ARMED && !c->sequence_mode && c->feedback_pending &&
                 (uint32_t)(now - c->feedback_started_ms) >= 3u)
            fault(c, PWM_FAULT_HEALTH);
        else if (c->state == PWM_ARMED && !c->sequence_mode &&
                 (uint32_t)(now - c->armed_ms) >= CTRL_SETTLE_MS &&
                 (!c->health.vrm_capture_valid || !c->health.vrm_capture_recent ||
                  (!c->feedback_pending && !feedback_matches(c)) || !rail_plausible(c, true)))
            fault(c, PWM_FAULT_HEALTH);
    }
}
pwm_outputs_t ctrl_tick(pwm_control_t *c, uint32_t now_ms, const pwm_health_t *h) {
    if (h) {
        c->health = *h;
        c->health_ms = now_ms;
        c->health_valid = true;
    }
    /* Only a new post-request observation can close a target transaction.
     * Repeated commands never restart its deadline. */
    if (c->feedback_pending && (uint32_t)(now_ms-c->feedback_started_ms)<3u && c->health.vrm_capture_valid && c->health.vrm_capture_recent &&
        (int32_t)(c->health.vrm_sample_ms - c->feedback_started_ms) > 0) {
        int32_t delta = (int32_t)c->health.vrm_ccr - c->requested_ccr;
        if (delta >= -12 && delta <= 12) c->feedback_pending = false;
    }
    service(c, now_ms);
    pwm_outputs_t o = { c->requested_ccr, config_ok(c), false, false };
    o.permit = o.software_config_valid && !c->fault_pending &&
        (c->state == PWM_ARMING || c->state == PWM_ARMED);
    if (o.permit && c->arm_request_pending) {
        o.arm_request = true;
        c->arm_request_pending = false;
        c->arm_request_issued = true;
    }
    return o;
}
void ctrl_arm_result(pwm_control_t *c, bool success, uint32_t now_ms) {
    service(c, now_ms);
    if (c->state != PWM_ARMING || !c->arm_request_issued) return;
    c->arm_request_issued = false;
    if (!success) fault(c, PWM_FAULT_ARM);
    else {
        c->state = PWM_ARMED;
        c->armed_ms = c->now_ms;
        /* Success certifies hardware just observed RUN high. Replace the
         * pre-arm snapshot so commands before the next sample do not mistake
         * its expected low RUN for a new loss. Every subsequent sample wins. */
        c->health.run_feedback = true;
    }
}
/* Decimal only, no signs, whitespace, wrap, or partial conversions. */
static bool u32(const char *s, uint32_t *v) {
    uint32_t x = 0;
    if (!s || !*s) return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        uint32_t d = (uint32_t)(*s++ - '0');
        if (x > (UINT32_MAX - d) / 10u) return false;
        x = x * 10u + d;
    }
    *v = x;
    return true;
}
static bool i32small(const char *s, int32_t *v) {
    bool minus = *s == '-';
    uint32_t x;
    if (!u32(s + (minus ? 1 : 0), &x) || x > 1200u) return false;
    *v = minus ? -(int32_t)x : (int32_t)x;
    return true;
}
static bool profile_parse(char **t, pwm_profile_t *p) {
    uint32_t v[9];
    unsigned ti[9] = {0, 1, 2, 3, 4, 6, 7, 8, 9};
    for (unsigned i = 0; i < 9; ++i) if (!u32(t[ti[i]], &v[i])) return false;
    if (!i32small(t[5], &p->offset_mv) ||
        v[0] < 100 || v[2] > 1200 || v[0] >= v[2] ||
        v[1] < CTRL_CCR_MIN || v[1] > CTRL_CCR_MAX ||
        v[3] < CTRL_CCR_MIN || v[3] > CTRL_CCR_MAX || v[1] == v[3] ||
        v[4] < 100000 || v[4] > 10000000 ||
        v[5] < 100 || v[6] > 1200 || v[5] > v[6] ||
        v[7] < 100 || v[8] > 1200 || v[7] > v[8]) return false;
    p->mv1 = (uint16_t)v[0]; p->ccr1 = (uint16_t)v[1];
    p->mv2 = (uint16_t)v[2]; p->ccr2 = (uint16_t)v[3];
    p->gain_ppm = v[4];
    p->boot_min_mv = (uint16_t)v[5]; p->boot_max_mv = (uint16_t)v[6];
    p->load_min_mv = (uint16_t)v[7]; p->load_max_mv = (uint16_t)v[8];
    return true;
}
enum { CMD_UNKNOWN, CMD_STATUS, CMD_DISARM, CMD_PASSIVE, CMD_DFU, CMD_RESET, CMD_PING, CMD_PREARM, CMD_QUALIFY, CMD_PROFILE, CMD_SETMV, CMD_SETCCR, CMD_ARM };
static unsigned command_id(const char *name) {
    static const char *const names[]={"STATUS","DISARM","PASSIVE","DFU","RESET","PING","PREARM","QUALIFY","PROFILE","SETMV","SETCCR","ARM"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)if(!strcmp(name,names[i]))return i+1u;
    return CMD_UNKNOWN;
}
void ctrl_command(pwm_control_t *c, const char *line, char *out, size_t n, uint32_t now) {
    char buf[CTRL_LINE_MAX + 1u], *t[16];
    size_t len = 0, nt = 0;
    service(c, now);
    if (!line) { reply(out, n, "ERR LINE"); return; }
    while (line[len] && len <= CTRL_LINE_MAX) ++len;
    if (!len || len > CTRL_LINE_MAX) { reply(out, n, "ERR LINE"); return; }
    memcpy(buf, line, len + 1);
    /* Input is one complete line WITHOUT CR/LF; transport must drain overflow. */
    char *p = buf;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (nt == sizeof(t) / sizeof(t[0])) { reply(out, n, "ERR TOKENS"); return; }
        t[nt++] = p;
        while (*p && *p != ' ') {
            if ((unsigned char)*p < 33u || (unsigned char)*p > 126u) {
                reply(out, n, "ERR ASCII"); return;
            }
            ++p;
        }
        if (*p) *p++ = 0;
    }
    if (!nt) { reply(out, n, "ERR LINE"); return; }
    if (nt == 1 && !strcmp(t[0], "STOP")) {
        off(c); reply(out, n, "OK STOP %s", ctrl_state_name(c->state)); return;
    }
    if (!strcmp(t[0], "HELLO")) {
        uint32_t nonce;
        if (nt != 2 || !u32(t[1], &nonce)) { reply(out, n, "ERR HELLO"); return; }
        off(c); c->profile_valid = false; c->session_valid = false;
        if (c->session == UINT32_MAX) { reply(out, n, "ERR SESSION_EXHAUSTED"); return; }
        ++c->session; c->last_seq = 0; c->session_valid = true;
        reply(out, n, "OK HELLO %lu %lu", (unsigned long)c->session, (unsigned long)nonce);
        return;
    }
    uint32_t sid, seq;
    if (nt < 3 || !u32(t[0], &sid) || !u32(t[1], &seq)) {
        reply(out, n, "ERR HEADER"); return;
    }
    if (!c->session_valid || sid != c->session) { reply(out, n, "ERR SESSION"); return; }
    if (!seq || seq <= c->last_seq) { reply(out, n, "ERR SEQUENCE"); return; }
    c->last_seq = seq; /* Consume fresh sequence even on rejected commands. */
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
    if (!strcmp(t[2], "SEQ") || !strcmp(t[2], "SETBG")) {
        extern void seq_runtime_command(pwm_control_t *, char **, size_t,
                                        char *, size_t, uint32_t);
        seq_runtime_command(c, t, nt, out, n, now); return;
    }
#endif
    unsigned cmd = command_id(t[2]);
    if (cmd == CMD_STATUS && nt == 3) {
        reply(out, n, "OK STATUS %s fault=%u fixture=%u qualified=%u profile=%u ccr=%u mv=%ld%s",
              ctrl_state_name(c->state), (unsigned)c->fault, PWMVID_ALLOW_FIXTURE == 1,
              c->qualified, c->profile_valid, c->requested_ccr, (long)ctrl_calibrated_mv(c),PWMVID_SELFTEST_SUFFIX);
        return;
    }
    if ((cmd == CMD_DISARM || cmd == CMD_PASSIVE) && nt == 3) { off(c); reply(out, n, "OK DISARM"); return; }
    if (cmd == CMD_DFU && nt==3) {
#if PWMVID_USB_UPDATER
        if(c->state!=PWM_DISARMED || !c->health_valid ||
           (uint32_t)(now-c->health_ms)>CTRL_HEALTH_MAX_AGE_MS ||
           !c->health.usb_connected || !c->health.usb_update_ready ||
           c->health.run_feedback || c->fault_pending) {
            reply(out,n,"ERR DFU_UNSAFE");return;
        }
        off(c);c->dfu_requested=true;reply(out,n,"OK DFU_PENDING");return;
#else
        reply(out,n,"ERR DFU_UNAVAILABLE");return;
#endif
    }
    if (cmd == CMD_RESET && nt == 3) {
        uint32_t reset_epoch = c->fault_epoch;
        if (c->state != PWM_FAULT || !c->health_valid || c->health.run_feedback ||
            c->health.break_active || c->health.comparator_fault || !c->health.usb_connected ||
            !c->health.clocks_ok ||
            (uint32_t)(now - c->health_ms) > CTRL_HEALTH_MAX_AGE_MS) {
            reply(out, n, "ERR RESET_UNSAFE"); return;
        }
        c->acknowledged_fault_epoch = reset_epoch;
        c->fault_pending = false; c->fault = PWM_FAULT_NONE;
        c->state = PWM_DISARMED; off(c); service(c, now);
        reply(out, n, c->state == PWM_FAULT ? "ERR RESET_RACED" : "OK RESET"); return;
    }
    if (cmd == CMD_PING && nt == 3) {
        if (c->state != PWM_ARMED || !base_health(c, now)) {
            reply(out, n, "ERR NOT_ARMED"); return;
        }
        c->heartbeat_ms = now; reply(out, n, "OK PING"); return;
    }
    if (cmd == CMD_PREARM && nt == 3 && c->state == PWM_DISARMED) {
        if (!c->health_valid || (uint32_t)(now-c->health_ms)>CTRL_HEALTH_MAX_AGE_MS ||
            !c->health.usb_connected || !c->health.clocks_ok || c->health.run_feedback ||
            !c->health.gpu_capture_valid || !c->health.gpu_capture_recent ||
            c->health.gpu_ccr < CTRL_CCR_MIN || c->health.gpu_ccr > CTRL_CCR_MAX) {
            reply(out,n,"ERR PREARM_HEALTH"); return;
        }
        c->state=PWM_PREARM; c->requested_ccr=c->health.gpu_ccr; c->target_valid=true;
        reply(out,n,"OK PREARM preview=RECONSTRUCTED ccr=%u",c->requested_ccr); return;
    }
    if (c->state != PWM_DISARMED && c->state != PWM_PREARM &&
        !(c->state == PWM_ARMED && (cmd == CMD_SETMV || cmd == CMD_SETCCR))) {
        reply(out, n, "ERR STATE"); return;
    }
    if (cmd == CMD_QUALIFY && nt == 4 &&
        (!strcmp(t[3], "FIXTURE_ISOLATED")
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
         || !strcmp(t[3], "GPU_EXPERIMENT")
#endif
        )) {
        if (PWMVID_ALLOW_FIXTURE != 1) { reply(out, n, "ERR LOCKED_BUILD"); return; }
#if PWMVID_INTERNAL_SOURCE_SELFTEST
        /* Fixed board-only envelope, not a GPU voltage calibration. Target
         * commands are disabled in this temporary build. A real 0.8 V sense
         * reading remains mandatory for ARM and throughout the lease. */
        c->profile=(pwm_profile_t){
            .mv1=750u,.ccr1=63u,.mv2=850u,.ccr2=65u,
            .gain_ppm=1000000u,.offset_mv=0,
            .boot_min_mv=740u,.boot_max_mv=860u,
            .load_min_mv=740u,.load_max_mv=860u};
        c->profile_valid=true;
#endif
        c->qualified = true; reply(out, n, "OK QUALIFY"); return;
    }
    if (cmd == CMD_PROFILE && nt == 13) {
#if PWMVID_INTERNAL_SOURCE_SELFTEST
        reply(out,n,"ERR SELFTEST_FIXED_PROFILE");return;
#endif
        pwm_profile_t temp;
        if (!profile_parse(&t[3], &temp)) { reply(out, n, "ERR PROFILE"); return; }
        c->profile = temp; c->profile_valid = true; c->target_valid = false;
        reply(out, n, "OK PROFILE"); return;
    }
    if ((cmd == CMD_SETMV || cmd == CMD_SETCCR) && nt == 4) {
#if defined(PWMVID_SEQUENCE) && PWMVID_SEQUENCE
        if (c->state == PWM_ARMED) { reply(out,n,"ERR USE_SETBG"); return; }
#endif
#if PWMVID_INTERNAL_SOURCE_SELFTEST
        reply(out,n,"ERR SELFTEST_FIXED_CCR");return;
#endif
        uint32_t value;
        if (c->state == PWM_PREARM) { reply(out,n,"ERR PREARM_TRACKING"); return; }
        if (c->state == PWM_ARMED && c->feedback_pending) { reply(out,n,"ERR TARGET_PENDING"); return; }
        if (!c->profile_valid || !u32(t[3], &value)) { reply(out, n, "ERR TARGET"); return; }
        int32_t ccr;
        if (cmd == CMD_SETMV) {
            if (value < c->profile.mv1 || value > c->profile.mv2) {
                reply(out, n, "ERR RANGE"); return;
            }
            /* Signed arithmetic permits either measured monotonic direction.
             * Nearest integer CCR; no extrapolation and no extrapolated mV promise. */
            int32_t numer = (int32_t)(value - c->profile.mv1) *
                            ((int32_t)c->profile.ccr2 - c->profile.ccr1);
            int32_t denom = (int32_t)c->profile.mv2 - c->profile.mv1;
            ccr = c->profile.ccr1 + (numer + (numer >= 0 ? denom / 2 : -denom / 2)) / denom;
        } else {
            if (!c->qualified || value < CTRL_CCR_MIN || value > CTRL_CCR_MAX) {
                reply(out, n, "ERR RAW_QUALIFICATION"); return;
            }
            ccr = (int32_t)value;
            uint16_t lo = c->profile.ccr1 < c->profile.ccr2 ? c->profile.ccr1 : c->profile.ccr2;
            uint16_t hi = c->profile.ccr1 > c->profile.ccr2 ? c->profile.ccr1 : c->profile.ccr2;
            if (ccr < lo || ccr > hi) { reply(out, n, "ERR RANGE"); return; }
        }
        if (ccr < (int32_t)CTRL_CCR_MIN || ccr > (int32_t)CTRL_CCR_MAX) { reply(out, n, "ERR RANGE"); return; }
        if (c->state == PWM_ARMED && c->requested_ccr != (uint16_t)ccr) {
            c->feedback_pending = true; c->feedback_started_ms = now;
        }
        c->requested_ccr = (uint16_t)ccr; c->target_valid = true;
        reply(out, n, "OK TARGET ccr=%u", c->requested_ccr); return;
    }
    if (cmd == CMD_ARM && nt == 4) {
#if PWMVID_INTERNAL_SOURCE_SELFTEST
        if(c->requested_ccr!=64u){reply(out,n,"ERR SELFTEST_FIXED_CCR");return;}
#endif
        uint32_t lease;
        if (!u32(t[3], &lease) || lease < 10u || lease > CTRL_LEASE_MAX_MS) {
            reply(out, n, "ERR LEASE"); return;
        }
        if (c->state != PWM_PREARM) { reply(out,n,"ERR PREARM_REQUIRED"); return; }
        if (!config_ok(c)) { reply(out, n, "ERR UNQUALIFIED"); return; }
        uint16_t low = c->profile.ccr1 < c->profile.ccr2 ? c->profile.ccr1 : c->profile.ccr2;
        uint16_t high = c->profile.ccr1 > c->profile.ccr2 ? c->profile.ccr1 : c->profile.ccr2;
        if(c->requested_ccr < low || c->requested_ccr > high) {reply(out,n,"ERR MATCH_OUTSIDE_PROFILE");return;}
        if (!base_health(c, now) || c->health.run_feedback || !rail_plausible(c, false)) {
            reply(out, n, "ERR HEALTH"); return;
        }
        if(c->requested_ccr > 96u) {reply(out,n,"ERR ARM_LOW_PHASE_WINDOW");return;}
        c->lease_ms = lease; c->heartbeat_ms = now;
        c->state = PWM_ARMING; c->arm_request_pending = true;
        reply(out, n, "OK ARM_PENDING"); return;
    }
    reply(out, n, "ERR COMMAND");
}
