"""Compile the actual diagnostics() function with maximal-width mock snapshots."""
import pathlib, re, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class TelemetryTests(unittest.TestCase):
    def test_complete_bounded_record(self):
        source = (ROOT/'src/main.c').read_text()
        function = source[source.index('static void diagnostics(void) {'):source.index('\nint main(void)')]
        harness = r'''
#include "control.h"
#include "text_format.h"
#include <stdint.h>
#include <stdio.h>
#define ADC_STALE_MS 10u
#define CAPTURE_STALE_MS 5u
typedef struct {uint32_t period_ticks,high_ticks,sample_count,last_ms,errors;bool valid;} capture_summary_t;
typedef struct {uint16_t raw_sense,raw_vref,vdda_mv,rail_mv;uint32_t last_ms,errors;bool valid;} adc_summary_t;
static char response[768];static pwm_control_t control;
static struct {uint32_t IDCODE;} debug={UINT32_MAX};
#define DBGMCU (&debug)
static uint32_t HAL_GetTick(void){return UINT32_MAX;}
static void hw_capture_summary(unsigned stream,capture_summary_t *s){(void)stream;*s=(capture_summary_t){UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,true};}
static void hw_adc_summary(adc_summary_t *s){*s=(adc_summary_t){65535,65535,65535,65535,UINT32_MAX,UINT32_MAX,true};}
#define MOCK(name) static uint32_t name(void){return UINT32_MAX;}
MOCK(hw_option_word) MOCK(hw_reset_flags) MOCK(hw_break_events) MOCK(hw_threshold_code)
MOCK(usb_transport_loss_count) MOCK(usb_transport_rx_overflow_count) MOCK(usb_transport_tx_drop_count)
static bool hw_run(void){return true;}
static bool hw_break_active(void){return true;}
static bool hw_pwm_ready(void){return true;}
'''
        harness += function
        harness += '\nint main(void){control.requested_ccr=65535;control.state=PWM_ARMED;diagnostics();fputs(response,stdout);return 0;}\n'
        with tempfile.TemporaryDirectory() as d:
            c=pathlib.Path(d)/'telemetry.c';exe=pathlib.Path(d)/'telemetry';c.write_text(harness)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'lib/control'),str(c),str(ROOT/'lib/control/control.c'),'-o',str(exe)],check=True,capture_output=True)
            output=subprocess.check_output([str(exe)]).decode('ascii')
        self.assertLess(len(output),768)
        self.assertTrue(output.endswith('tx_drop=4294967295\n'))
        for field in ['adc_ok','adc_ms','gpu_ms','vrm_ms','source_ready','gpu_ok','vrm_ok']:
            self.assertRegex(output,rf'\b{field}=\d+')
        self.assertIn('preview=RECONSTRUCTED',output)
