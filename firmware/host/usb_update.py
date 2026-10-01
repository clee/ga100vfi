#!/usr/bin/env python3
"""Validated custom-loader USB update. Never uses the STM32 factory ROM loader."""
from __future__ import annotations
import argparse
import binascii
from pathlib import Path
import struct
import re
import shutil
import subprocess
import sys
import tempfile
import time

def load_contract():
    text=(Path(__file__).resolve().parents[1]/'include/boot_contract_cb.h').read_text()
    values={}
    for key in ('APP_BASE','FLASH_END','FOOTER_BASE','BOOT_COOKIE_ADDRESS','IMAGE_MAGIC','IMAGE_FORMAT','COMMIT_MAGIC','BOARD_ID'):
        match=re.search(r'^#define\s+PWMVID_'+key+r'\s+(0x[0-9a-fA-F]+|\d+)[uU]?\b',text,re.M)
        if not match:raise ValueError('Missing literal boot contract field: '+key)
        values[key]=int(match[1],0)
    if not 0x08000000<values['APP_BASE']<values['FOOTER_BASE']<values['FLASH_END']==0x08020000:
        raise ValueError('Unsafe application partition in bundled contract')
    if values['APP_BASE']%2048 or values['FLASH_END']%2048 or values['FLASH_END']-values['FOOTER_BASE']!=32:
        raise ValueError('Unsupported application page or footer layout')
    return values

def configure_contract():
    global CONTRACT, APP_BASE, FLASH_END, IMAGE_SIZE, FOOTER_SIZE, MAGIC, COMMIT, BOARD_ID, ALT_NAME, LOADER_PRODUCT, APP_PRODUCT
    CONTRACT=load_contract()
    APP_BASE=CONTRACT['APP_BASE']
    FLASH_END=CONTRACT['FLASH_END']
    IMAGE_SIZE=FLASH_END-APP_BASE
    FOOTER_SIZE=FLASH_END-CONTRACT['FOOTER_BASE']
    MAGIC=CONTRACT['IMAGE_MAGIC']
    COMMIT=CONTRACT['COMMIT_MAGIC']
    BOARD_ID=CONTRACT['BOARD_ID']
    ALT_NAME=f"@PWMVID-P8P9-APP /0x{APP_BASE:08X}/{IMAGE_SIZE//2048}*002Kg"
    LOADER_PRODUCT='PWMVID G431CB USB Updater'
    APP_PRODUCT='PWMVID G431CB Sequencer CDC'
configure_contract()
DFU_ID=",0483:df11"


def validate_image(data: bytes) -> dict:
    """Reject combined/SWD images, truncation, corruption and bad vector bounds."""
    if len(data) != IMAGE_SIZE:
        raise ValueError(f"USB app image must be exactly {IMAGE_SIZE} bytes; never upload a combined first-flash image over USB")
    magic, length, crc, version, commit, inverse, board, board_inverse = struct.unpack('<8I', data[-FOOTER_SIZE:])
    if (magic, version, commit, inverse, board, board_inverse) != (MAGIC, CONTRACT['IMAGE_FORMAT'], COMMIT, (~COMMIT) & 0xffffffff, BOARD_ID, (~BOARD_ID) & 0xffffffff):
        raise ValueError('Missing or unsupported custom-updater footer')
    if not 8 <= length <= IMAGE_SIZE-FOOTER_SIZE or length % 8:
        raise ValueError('Invalid aligned payload length')
    if any(b != 0xff for b in data[length:-FOOTER_SIZE]):
        raise ValueError('Unexpected non-erased bytes outside payload')
    if binascii.crc32(data[:length]) & 0xffffffff != crc:
        raise ValueError('Payload CRC mismatch; image is corrupt')
    stack, reset = struct.unpack_from('<II', data)
    if stack % 8 or not 0x20000000 < stack <= CONTRACT['BOOT_COOKIE_ADDRESS']:
        raise ValueError('Initial stack is outside STM32G431 SRAM or unaligned')
    if not reset & 1 or not APP_BASE <= (reset & ~1) < APP_BASE + length:
        raise ValueError('Reset vector is outside the app payload or not Thumb')
    return {'length': length, 'crc32': crc, 'app_base': APP_BASE, 'stack': stack, 'reset': reset}


def select_loader(listing: str, serial: str) -> str:
    """Require exact custom-loader memory descriptor and explicit serial, not VID/PID alone."""
    if not re.fullmatch(r'[0-9A-F]{24}', serial):
        raise ValueError('Specify the exact USB serial without wildcards or commas')
    matches=[]
    for line in listing.splitlines():
        if ('Found DFU: [0483:df11]' in line and
            re.search(r'\balt=0(?:,|\s)', line) and
            f'name="{ALT_NAME}"' in line and f'serial="{serial}"' in line):
            matches.append(line)
    if len(matches)!=1:
        raise ValueError('Expected exactly one custom PWMVID loader with matching serial and app-only memory descriptor; no flash attempted')
    return matches[0]


def download_command(executable, serial, filename):
    if not re.fullmatch(r'[0-9A-F]{24}', serial):
        raise ValueError('Invalid exact USB serial')
    return [executable,'-d',DFU_ID,'-S',serial,'-a',ALT_NAME,'-t','256','-s',f'0x{APP_BASE:08X}','-D',str(filename)]


def upload_command(executable, serial, filename):
    if not re.fullmatch(r'[0-9A-F]{24}', serial):
        raise ValueError('Invalid exact USB serial')
    return [executable,'-d',DFU_ID,'-S',serial,'-a',ALT_NAME,'-t','256',
            '-s',f'0x{APP_BASE:08X}:{IMAGE_SIZE}','-U',str(filename)]


def enter_custom_loader(client, *, clear_isolated_arm_fault=False,
                        clear_isolated_test_fault=False):
    # Explicit updater invocation authorizes a safe HELLO/disarm, never arming.
    client.hello()
    initial=client.exchange('TELEM')
    fields=dict(token.split('=',1) for token in initial.split()[2:] if '=' in token)
    fault_status=(client.command('STATUS') if fields.get('state')=='FAULT' and
                  fields.get('run')=='0' else '')
    disconnect_fault=fault_status.startswith('OK STATUS FAULT fault=2 ')
    isolated_arm_fault=(
        ((clear_isolated_arm_fault and
          fault_status.startswith('OK STATUS FAULT fault=5 ')) or
         (clear_isolated_test_fault and
          (fault_status.startswith('OK STATUS FAULT fault=1 ') or
           fault_status.startswith('OK STATUS FAULT fault=5 ')))) and
        fields.get('selftest')=='1' and fields.get('break')=='0' and
        fields.get('breaks')=='0')
    if disconnect_fault or isolated_arm_fault:
        # Clear only the expected CDC disconnect fault, or an explicitly
        # selected, non-running fault 1/5 on the temporary isolated test image.
        # Keep this session open through the subsequent custom-loader request.
        if client.command('RESET')!='OK RESET':
            raise RuntimeError('Known non-running fault could not be cleared')
        client.hello()
    deadline=time.monotonic()+0.5
    while time.monotonic()<deadline:
        response=client.exchange('TELEM')
        fields=dict(token.split('=',1) for token in response.split()[2:] if '=' in token)
        if fields.get('state')=='PASSIVE' and fields.get('run')=='0':
            break
        time.sleep(0.01)
    else:
        raise RuntimeError('Output-disabled PASSIVE state not confirmed; custom loader was not requested')
    response=client.command('DFU')
    if response!='OK DFU_PENDING':
        raise RuntimeError('Custom loader request not acknowledged; no retry: '+response)
    # Allow the acknowledged, delayed reset; no STOP or renewed command follows.
    time.sleep(0.2)


def wait_for_app(serial: str, timeout: float = 10.0) -> str:
    from serial.tools import list_ports
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        for port in list_ports.comports():
            if (port.vid,port.pid,port.serial_number,port.product)==(
                0x0483,0x5740,serial,APP_PRODUCT):
                return port.device
        time.sleep(0.1)
    raise RuntimeError('Verified USB transfer completed, but the PWMVID application CDC did not re-enumerate')


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--validate-only',action='store_true')
    parser.add_argument('--serial',help='Exact serial printed by the custom loader in dfu-util -l')
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--port',help='Explicit running PWMVID CDC port to enter the custom updater once')
    mode.add_argument('--loader-ready',action='store_true',help='The custom loader is already enumerated, including recovery after interruption')
    parser.add_argument('--confirm-update',action='store_true',help='Authorize replacing only the application; GPU/load disconnected, PASSIVE with RUN low, stable power. JP1 may remain bridged')
    parser.add_argument('--clear-isolated-arm-fault',action='store_true',help='Clear only non-running fault 5 on temporary isolated self-test firmware in the same USB update session')
    parser.add_argument('--clear-isolated-test-fault',action='store_true',help='Clear only non-running fault 1 or 5 on temporary isolated self-test firmware with zero break events')
    args=parser.parse_args(argv)
    try:
        data=args.image.read_bytes();info=validate_image(data)
        print(f"Validated {info['length']} payload bytes; CRC32 {info['crc32']:08x}. Integrity check, not firmware authentication.")
        print(f"Bundled partition: base 0x{APP_BASE:08X}, USB image {IMAGE_SIZE} bytes; required interface {ALT_NAME}")
        if args.validate_only:return 0
        if not args.confirm_update or not args.serial or not (args.port or args.loader_ready):
            raise ValueError('Update requires --confirm-update, exact --serial, and --port or --loader-ready. Validation-only: --validate-only')
        if (args.clear_isolated_arm_fault or args.clear_isolated_test_fault) and not args.port:
            raise ValueError('Clearing an isolated test fault requires a running application --port')
        if not re.fullmatch(r'[0-9A-F]{24}',args.serial):raise ValueError('Invalid exact serial')
        executable=shutil.which('dfu-util')
        if not executable:raise ValueError('Install dfu-util first (macOS: brew install dfu-util). No device action performed.')
        if args.port:
            import serial
            from pwmvid import Client
            with serial.Serial(args.port,115200,timeout=0.75,write_timeout=0.75) as port:
                port.reset_input_buffer();enter_custom_loader(Client(port),
                    clear_isolated_arm_fault=args.clear_isolated_arm_fault,
                    clear_isolated_test_fault=args.clear_isolated_test_fault)
        deadline=time.monotonic()+10
        while True:
            result=subprocess.run([executable,'-l','-d',DFU_ID,'-S',args.serial],capture_output=True,text=True,timeout=3,check=False)
            try:select_loader(result.stdout+'\n'+result.stderr,args.serial);break
            except ValueError:
                if time.monotonic()>=deadline:raise
                time.sleep(0.25)
        # Use the bytes already validated, not a potentially changed input path.
        with tempfile.TemporaryDirectory(prefix='pwmvid-update-') as temp:
            image=Path(temp)/'validated-app.bin';image.write_bytes(data)
            command=download_command(executable,args.serial,image)
            completed=subprocess.run(command,timeout=360,check=False)
            if completed.returncode:
                raise RuntimeError('USB update did not report success. No automatic retry; inspect custom-loader recovery or use ST-Link.')
            readback=Path(temp)/'readback-app.bin'
            completed=subprocess.run(upload_command(executable,args.serial,readback),timeout=180,check=False)
            if completed.returncode or not readback.exists() or readback.read_bytes()!=data:
                raise RuntimeError('USB Flash readback did not match the reviewed app image. Leave request withheld; inspect loader state.')
            print(f'USB Flash readback matches all {IMAGE_SIZE} image bytes; requesting application boot.',flush=True)
            from dfu_manifest import finish
            finish(args.serial,LOADER_PRODUCT)
        app_port=wait_for_app(args.serial)
        print(f'Verified app-only USB update complete; PWMVID CDC re-enumerated at {app_port}. No hardware qualification is implied.')
        return 0
    except (OSError,ValueError,RuntimeError,ImportError,subprocess.SubprocessError) as exc:
        print(str(exc),file=sys.stderr);return 1


if __name__=='__main__':raise SystemExit(main())
