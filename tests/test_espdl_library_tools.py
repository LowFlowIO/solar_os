import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'modules/espdl'/(name+'.py'))
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result


FIX=module('fix_frames')
VERIFY=module('verify')


def fixture(encoding=0x1b):
    # One real DWARF CIE/FDE: relative PC 0xfe8 at 0x201c names code at 0x3004.
    cie=struct.pack('<II',16,0)+b'\x01zR\0\x01\x7c\x10\x01'+bytes([encoding])+bytes(3)
    fde=struct.pack('<IIII',16,24,0xfe8,16)+bytes(4)
    frames=cie+fde+bytes(4)
    strings=b'\0';symbols=bytes(16)
    for name,value in [('__module_eh_start',0x2000),('__module_eh_end',0x202c),
                       ('__module_lsda_start',0x202c),('__module_lsda_end',0x202c)]:
        index=len(strings);strings+=name.encode()+b'\0'
        symbols+=struct.pack('<IIIBBH',index,value,0,0,0,2)
    rows=[('',0,0,0,b'',0,0),('.debug_info',1,0,0,bytes(0x4000),0,0),
          ('.data.rel.ro',1,3,0x2000,frames,0,0),('.text',1,6,0x3000,bytes(32),0,0),
          ('.rela.dyn',4,2,0,b'',7,12),('.symtab',2,0,0,symbols,6,16),
          ('.strtab',3,0,0,strings,0,0),('.dynsym',11,2,0,bytes(16),8,16),
          ('.dynstr',3,2,0,b'\0',0,0)]
    names=b'\0';name_offsets=[0]
    for row in rows[1:]:name_offsets.append(len(names));names+=row[0].encode()+b'\0'
    name_offsets.append(len(names));names+=b'.shstrtab\0';rows.append(('.shstrtab',3,0,0,names,0,0))
    data=bytearray(52);sections=[]
    for index,(_,kind,flags,address,payload,link,entry) in enumerate(rows):
        data.extend(bytes((-len(data))%4));offset=len(data);data.extend(payload)
        sections.append((name_offsets[index],kind,flags,address,offset,len(payload),link,0,4,entry))
    data.extend(bytes((-len(data))%4));shoff=len(data)
    for section in sections:data.extend(struct.pack('<10I',*section))
    ident=b'\x7fELF\x01\x01\x01'+bytes(9)
    struct.pack_into('<16sHHIIIIIHHHHHH',data,0,ident,3,94,1,0x3000,0,shoff,0,52,0,0,40,len(sections),len(sections)-1)
    return data,sections,shoff


class EspdlLibraryToolsTest(unittest.TestCase):
    def test_relative_frame_pc_becomes_a_mapped_absolute_relocation(self):
        data,sections,shoff=fixture()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'library.elf';path.write_bytes(data);FIX.fix(path)
            fixed=path.read_bytes();section=struct.unpack_from('<10I',fixed,shoff+40*4)
            self.assertEqual(struct.unpack_from('<III',fixed,section[4]),(0x201c,5,0))
            self.assertEqual(fixed[sections[2][4]+16],3)
            self.assertEqual(struct.unpack_from('<I',fixed,sections[2][4]+28)[0],0x3004)
            self.assertEqual(VERIFY.verify(path,set()),[])
            FIX.fix(path)
            twice=path.read_bytes();self.assertEqual(struct.unpack_from('<I',twice,shoff+40*4+20)[0],12)

    def test_unsupported_encoding_and_truncated_elf_leave_input_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'library.elf'
            for data in (fixture(0x19)[0],bytearray(b'\x7fELF')):
                path.write_bytes(data)
                with self.assertRaises(ValueError):FIX.fix(path)
                self.assertEqual(path.read_bytes(),data)

    def test_unmapped_relocation_is_rejected_and_noops_are_removed(self):
        data,sections,shoff=fixture()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'library.elf';path.write_bytes(data);FIX.fix(path)
            fixed=bytearray(path.read_bytes());section=struct.unpack_from('<10I',fixed,shoff+40*4)
            struct.pack_into('<III',fixed,section[4],0xdeadbeec,5,0);path.write_bytes(fixed)
            with self.assertRaisesRegex(ValueError,'unmapped'):VERIFY.verify(path,set())
            struct.pack_into('<III',fixed,section[4],0,0,0);path.write_bytes(fixed)
            VERIFY.remove_noop_relocations(path)
            self.assertEqual(VERIFY.verify(path,set()),[])
