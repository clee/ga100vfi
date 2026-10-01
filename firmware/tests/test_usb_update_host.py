import binascii
import importlib.util
from pathlib import Path
import struct
import unittest
SPEC=importlib.util.spec_from_file_location('usb_update',Path(__file__).parents[1]/'host/usb_update.py')
u=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(u)
def image():
    data=bytearray(b'\xff'*u.IMAGE_SIZE)
    struct.pack_into('<II',data,0,0x200057f8,u.APP_BASE+9)
    length=16
    struct.pack_into('<8I',data,u.IMAGE_SIZE-32,u.MAGIC,length,binascii.crc32(data[:length])&0xffffffff,1,u.COMMIT,(~u.COMMIT)&0xffffffff,u.BOARD_ID,(~u.BOARD_ID)&0xffffffff)
    return data
class UpdateTests(unittest.TestCase):
    def test_valid_image(self):self.assertEqual(u.validate_image(image())['length'],16)
    def test_wrong_size(self):
        for b in [b'',bytes(131072),image()[:-1],image()+b'\xff']:
            with self.assertRaises(ValueError):u.validate_image(b)
    def test_corrupt_payload_footer_and_padding(self):
        for offset in [0,8,100,u.IMAGE_SIZE-32,u.IMAGE_SIZE-4]:
            b=image();b[offset]^=1
            with self.assertRaises(ValueError):u.validate_image(b)
    def test_footer_partition_length_and_board_checks(self):
        for word,value in [(1,7),(1,17),(1,u.IMAGE_SIZE),(3,2),(4,0),(5,0),(6,0),(7,0)]:
            b=image();struct.pack_into('<I',b,u.IMAGE_SIZE-32+word*4,value)
            with self.assertRaises(ValueError):u.validate_image(b)
    def test_bad_vectors_even_with_valid_crc(self):
        for stack,reset in [(0x20005808,u.APP_BASE+9),(0x20000000,u.APP_BASE+9),(0x200057f8,0x08000001),(0x200057f8,u.APP_BASE+8),(0x200057f8,u.APP_BASE+17)]:
            b=image();struct.pack_into('<II',b,0,stack,reset);struct.pack_into('<I',b,u.IMAGE_SIZE-24,binascii.crc32(b[:16])&0xffffffff)
            with self.assertRaises(ValueError):u.validate_image(b)


SERIAL='0123456789ABCDEF01234567'
class TransportTests(unittest.TestCase):
    def listing(self):return f'Found DFU: [0483:df11] ver=0200, devnum=2, cfg=1, intf=0, path="1-2", alt=0, name="{u.ALT_NAME}", serial="{SERIAL}"'
    def test_exact_custom_identity(self):
        self.assertEqual(u.select_loader(self.listing(),SERIAL),self.listing())
        for listing in ['',self.listing().replace(u.ALT_NAME,'@Internal Flash /0x08000000/16*002Kg'),self.listing().replace(SERIAL,'A'*24),self.listing()+'\n'+self.listing(),self.listing().replace('alt=0','alt=1')]:
            with self.assertRaises(ValueError):u.select_loader(listing,SERIAL)
    def test_no_wildcard_serial_or_unsafe_download_flags(self):
        for serial in ['', '*', SERIAL+',', SERIAL.lower()]:
            with self.assertRaises(ValueError):u.download_command('dfu-util',serial,'file.bin')
        cmd=u.download_command('dfu-util',SERIAL,'file.bin')
        self.assertEqual(cmd,['dfu-util','-d',',0483:df11','-S',SERIAL,'-a',u.ALT_NAME,'-t','256','-s',f'0x{u.APP_BASE:08X}','-D','file.bin'])
        self.assertEqual(u.upload_command('dfu-util',SERIAL,'readback.bin')[-3:],
                         [f'0x{u.APP_BASE:08X}:{u.IMAGE_SIZE}','-U','readback.bin'])
    def test_enter_loader_acknowledgement_no_retry(self):
        from unittest.mock import patch
        class Client:
            def __init__(self,reply):self.calls=[];self.reply=reply
            def hello(self):self.calls.append('HELLO')
            def exchange(self,line):self.calls.append(line);return 'OK TELEM state=PASSIVE run=0'
            def command(self,line):self.calls.append(line);return self.reply
        with patch.object(u.time,'sleep'):
            c=Client('OK DFU_PENDING');u.enter_custom_loader(c);self.assertEqual(c.calls,['HELLO','TELEM','TELEM','DFU'])
            c=Client('ERR UNSAFE')
            with self.assertRaises(RuntimeError):u.enter_custom_loader(c)
            self.assertEqual(c.calls,['HELLO','TELEM','TELEM','DFU'])
    def test_enter_loader_clears_only_disconnect_fault_in_same_session(self):
        from unittest.mock import patch
        class Client:
            def __init__(self,fault):self.calls=[];self.fault=fault
            def hello(self):self.calls.append('HELLO')
            def exchange(self,line):
                self.calls.append(line)
                if self.fault:
                    self.fault=False
                    return 'OK TELEM state=FAULT fault=2 run=0'
                return 'OK TELEM state=PASSIVE run=0'
            def command(self,line):
                self.calls.append(line)
                return {'STATUS':'OK STATUS FAULT fault=2 fixture=0',
                        'RESET':'OK RESET','DFU':'OK DFU_PENDING'}[line]
        with patch.object(u.time,'sleep'):
            c=Client(True);u.enter_custom_loader(c)
            self.assertEqual(c.calls,['HELLO','TELEM','STATUS','RESET','HELLO','TELEM','DFU'])
    def test_isolated_arm_fault_requires_explicit_same_session_clear(self):
        from unittest.mock import patch
        class Client:
            def __init__(self):self.calls=[];self.fault=True
            def hello(self):self.calls.append('HELLO')
            def exchange(self,line):
                self.calls.append(line)
                return ('OK TELEM state=FAULT run=0 break=0 breaks=0 selftest=1' if self.fault
                        else 'OK TELEM state=PASSIVE run=0 selftest=1')
            def command(self,line):
                self.calls.append(line)
                if line=='STATUS':return 'OK STATUS FAULT fault=5 fixture=1 selftest=1'
                if line=='RESET':self.fault=False;return 'OK RESET'
                if line=='DFU':return 'OK DFU_PENDING'
                raise AssertionError(line)
        with patch.object(u.time,'sleep'):
            c=Client()
            with self.assertRaises(RuntimeError):u.enter_custom_loader(c)
            self.assertNotIn('RESET',c.calls)
            c=Client();u.enter_custom_loader(c,clear_isolated_arm_fault=True)
            self.assertEqual(c.calls,['HELLO','TELEM','STATUS','RESET','HELLO','TELEM','DFU'])
    def test_isolated_isr_fault_requires_explicit_same_session_clear(self):
        from unittest.mock import patch
        class Client:
            def __init__(self, *, breaks='0', selftest='1'):
                self.calls=[];self.fault=True;self.breaks=breaks;self.selftest=selftest
            def hello(self):self.calls.append('HELLO')
            def exchange(self,line):
                self.calls.append(line)
                return (f'OK TELEM state=FAULT run=0 break=0 breaks={self.breaks} selftest={self.selftest}'
                        if self.fault else 'OK TELEM state=PASSIVE run=0')
            def command(self,line):
                self.calls.append(line)
                if line=='STATUS':return 'OK STATUS FAULT fault=1 fixture=1 selftest=1'
                if line=='RESET':self.fault=False;return 'OK RESET'
                if line=='DFU':return 'OK DFU_PENDING'
                raise AssertionError(line)
        with patch.object(u.time,'sleep'):
            for kwargs in ({},{'clear_isolated_arm_fault':True},
                           {'clear_isolated_test_fault':True,'breaks':'1'},
                           {'clear_isolated_test_fault':True,'selftest':'0'}):
                c=Client(breaks=kwargs.pop('breaks','0'),selftest=kwargs.pop('selftest','1'))
                with self.assertRaises(RuntimeError):u.enter_custom_loader(c,**kwargs)
                self.assertNotIn('RESET',c.calls)
            c=Client();u.enter_custom_loader(c,clear_isolated_test_fault=True)
            self.assertEqual(c.calls,['HELLO','TELEM','STATUS','RESET','HELLO','TELEM','DFU'])
    def test_no_serial_or_subprocess_on_missing_confirmation_or_bad_image(self):
        from unittest.mock import patch
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'app.bin';p.write_bytes(image())
            with patch.object(u.subprocess,'run') as call:
                self.assertEqual(u.main(['--image',str(p)]),1);call.assert_not_called()
                p.write_bytes(b'bad');self.assertEqual(u.main(['--image',str(p),'--loader-ready','--confirm-update','--serial',SERIAL]),1);call.assert_not_called()

    def test_verified_download_and_mismatch_never_manifests(self):
        from unittest.mock import patch
        from types import SimpleNamespace
        import tempfile
        import sys
        for outcome in ('success','download_error','readback_error','mismatch'):
            with tempfile.TemporaryDirectory() as d:
                p=Path(d)/'input.bin';p.write_bytes(image());calls=[]
                def run(cmd,**kwargs):
                    calls.append(cmd)
                    if '-l' in cmd:return SimpleNamespace(returncode=0,stdout=self.listing(),stderr='')
                    if '-D' in cmd:
                        self.assertEqual(Path(cmd[-1]).read_bytes(),image())
                        self.assertNotEqual(Path(cmd[-1]),p)
                        return SimpleNamespace(returncode=int(outcome=='download_error'))
                    if outcome!='readback_error':
                        b=image();b[0]^=int(outcome=='mismatch');Path(cmd[-1]).write_bytes(b)
                    return SimpleNamespace(returncode=int(outcome=='readback_error'))
                from unittest.mock import Mock
                manifest=Mock()
                with patch.object(u.shutil,'which',return_value='/usr/local/bin/dfu-util'),\
                     patch.object(u.subprocess,'run',side_effect=run),\
                     patch.object(u,'wait_for_app',return_value='/dev/cu.usbmodem101'),\
                     patch.dict(sys.modules,{'dfu_manifest':Mock(finish=manifest)}):
                    self.assertEqual(u.main(['--image',str(p),'--loader-ready','--confirm-update','--serial',SERIAL]),int(outcome!='success'))
                self.assertEqual(len(calls),2 if outcome=='download_error' else 3)
                self.assertEqual(sum('-D' in c for c in calls),1)
                self.assertEqual(manifest.call_count,int(outcome=='success'))

if __name__=='__main__':unittest.main()
