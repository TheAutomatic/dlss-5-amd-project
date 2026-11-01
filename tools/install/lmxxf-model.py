"""Convert a verified Mochizuki Small pack to lmxxf logical assets, offline.

Uses only Python's standard library. No DLL is loaded and no weights are shipped.
Every output must match the pinned upstream 0.44 manifest before installation.
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import importlib.util
import uuid

SMALL_SHA256 = "7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33"
MANIFEST = Path(__file__).with_name("lmxxf-small-manifest.json")
E4M3 = [(-1.0 if b & 128 else 1.0) * ((b & 7) * 2.0**-9 if not (b >> 3 & 15)
        else (1 + (b & 7) / 8) * 2.0**((b >> 3 & 15)-7)) for b in range(256)]
E4M3[127] = E4M3[255] = float('nan')
C32_ORDER = [0,1,4,5,8,9,12,13,2,3,6,7,10,11,14,15,16,17,20,21,24,25,28,29,18,19,22,23,26,27,30,31]


def read_pack(path):
    blob = Path(path).read_bytes()
    if hashlib.sha256(blob).hexdigest() != SMALL_SHA256:
        raise ValueError("Unsupported or damaged Small pack; extract dlssnr-small.bin from supported 310.9.16 first")
    count, reserved = struct.unpack_from('<II', blob, 8)
    if blob[:8] != b'NRMODEL1' or count != 255 or reserved:
        raise ValueError('Invalid model index')
    entries = {}; pos = 16
    for _ in range(count):
        n, = struct.unpack_from('<I', blob, pos); pos += 4
        name = blob[pos:pos+n].decode('utf-8'); pos += n
        off, size = struct.unpack_from('<QQ', blob, pos); pos += 16
        if name in entries or off + size > len(blob):
            raise ValueError('Invalid model entry')
        entries[name] = memoryview(blob)[off:off+size]
    return entries


def floats(raw, kind='e'):
    return array('f', (x[0] for x in struct.iter_unpack('<'+kind, raw)))


def fp8(raw):
    return array('f', (E4M3[x] for x in raw))


def concat(*parts):
    out = array('f')
    for part in parts: out.extend(part)
    return out


def reorder(raw, order):
    out = array('f', [0]) * len(order)
    for i, dest in enumerate(order): out[dest] = raw[i]
    return out


def skip(raw, c):
    return reorder(raw, C32_ORDER if c == 32 else [(i//16)*16+(i%8)*2+(i%16//8) for i in range(c)])


def axis_a(i): return (i//16)*16+(i%8)*2+i%16//8

def vit_axis(i): return (i & ~31) | (i&1) | ((i&6)<<2) | ((i&24)>>2)

def axis_b(i): return (i//32)*32+C32_ORDER[i%32]

def matrix(raw, rows, cols, row_map=axis_a, col_map=axis_a):
    out=array('f',[0])*(rows*cols)
    co=[col_map(c) for c in range(cols)]
    for r in range(rows):
        dr=row_map(r)*cols; sr=r*cols
        for c,dest in enumerate(co): out[dr+dest]=E4M3[raw[sr+c]]
    return out


def unswizzle_inverse(canonical, rows, cols):
    raw=bytearray(rows*cols)
    for r in range(rows):
        for c in range(cols):
            lane=4*(r%8)+(c%8)//2
            byte=2*(c%32//8)+c%2
            off=(r//16)*512+(c//32)*512*(rows//16)+lane*16+(r%16//8)*8+byte
            raw[off]=canonical[r*cols+c]
    return raw


def qkv(raw, c):
    packed=unswizzle_inverse(raw,3*c,c)
    d=c.bit_length()-1
    parts=[bytearray() for _ in range(3)]
    for i in range(0,len(packed),3072):
        for p in range(3): parts[p].extend(packed[i+p*1024:i+(p+1)*1024])
    return concat(*(scatter_fp8(p,[3]+list(range(6,d+5)),[1,0,4,5,2]+list(range(d+5,2*d))) for p in parts))


def bits(i, positions):
    return sum(((i >> p) & 1) << b for b, p in enumerate(positions))


def scatter_fp8(raw, rows, cols):
    # Address permutations measured by the upstream native references.
    out = array('f', [0]) * len(raw)
    width = 1 << len(cols)
    # Factor bit permutations into two small tables rather than recomputing per weight.
    def address(i):return bits(i,rows)*width+bits(i,cols)
    low=[address(i) for i in range(min(1024,len(raw)))]
    high=[address(i*1024) for i in range((len(raw)+1023)//1024)]
    for i,v in enumerate(raw): out[low[i&1023]+high[i>>10]]=E4M3[v]
    return out


def bias(raw, heads):
    src=floats(raw); out=array('f',[0])*len(src)
    for i,v in enumerate(src):
        dest=(i//4096)*4096+bits(i%4096,[5,6,10,7,1,11])*64+bits(i%4096,[0,3,8,4,2,9])
        out[dest]=v
    return out


class Converter:
    def __init__(self, entries, output, manifest):
        self.entries, self.output, self.manifest = entries, Path(output), manifest
        self.written = set()

    def get(self, block, name, group='unpacked', layer=0):
        return self.entries[f'{group}/block{block}.layer{layer}.layer.{name}.bin']

    def save(self, name, values):
        matches = [k for k in self.manifest['files'] if k.rsplit('.',1)[0] == name]
        if len(matches) != 1: raise ValueError('Unexpected output '+name)
        filename = matches[0]; expected = self.manifest['files'][filename]
        if len(values) != expected['elements'] or not all(map(math.isfinite, values)):
            raise ValueError('Invalid output shape/value '+name)
        if sys.byteorder != 'little': raise ValueError('Little-endian Python required')
        logical = values.tobytes()
        raw = (b''.join(struct.pack('<'+str(len(values[i:i+65536]))+'e', *values[i:i+65536])
                       for i in range(0,len(values),65536)) if filename.endswith('.f16') else logical)
        if hashlib.sha256(logical).hexdigest() != expected['logical_f32_sha256'] or hashlib.sha256(raw).hexdigest() != expected['sha256']:
            raise ValueError('Upstream weight hash mismatch: '+filename)
        (self.output/filename).write_bytes(raw); self.written.add(filename)
        print('verified '+filename, flush=True)

    def swin(self, block, role, c, group='unpacked', up=False):
        g = lambda name: self.get(block,name,group)
        residual_full = floats(g('residual_scale'))
        residual = residual_full
        residual = residual[:c] if (up and c != 32) or role in (0,70) else residual[8:8+c]
        w1=matrix(g('mlp_expand'),4*c,c,col_map=axis_b if c==32 else axis_a)
        w2=matrix(g('mlp_contract'),c,4*c,axis_b,axis_a) if c==32 else array('f',[0])*(4*c*c)
        if c == 32:
            first = array('f',[0])*512
            if role == 0:
                for s, v in enumerate(floats(g('input_lift'))):
                    channel=(s//64)*4+(s//32%2)+(s//4%2)*2
                    feature=(s//8%4)*4+s%4
                    first[channel*16+feature]=v
            self.save(f'{role_name(role)}-ffn',concat(first,w1,w2,skip(residual,c)))
        else:
            mid=matrix(g('mlp_mid'),c,128)
            for r in range(c):
                base=r*4*c+(r//32)*128
                w2[base:base+128]=mid[r*128:(r+1)*128]
            self.save(f'block{role}-ffn',concat(w1,w2,matrix(g('mlp_contract'),c,c),skip(residual,c)))
        self.save(f'{role_name(role)}-attention',concat(matrix(g('qkv'),3*c,c,col_map=axis_b) if c==32 else qkv(g('qkv'),c),matrix(g('attn_out_proj'),c,c,axis_b if c==32 else axis_a,axis_a),
                  bias(g('attn_pos_bias'),c//32),floats(g('scalars_b'),'f')[:c//32],skip(floats(g('attn_residual_scale'))[:c],c)))
        if role in (4,8,14,22): self.save(f'block{role}-ds',matrix(g('resample'),2*c,c,col_map=axis_b if c==32 else axis_a))
        if up:
            self.save(f'block{role}-weights',concat(matrix(g('resample'),c,2*c,axis_b if c==32 else axis_a,axis_a),skip(floats(g('upsample_gain'))[:c] if c==32 else concat(residual_full[c:],floats(g('upsample_gain'))),c)))
        if role == 70:
            self.save('post70-scales',concat(skip(floats(g('main_gain')),32),skip(floats(g('skip_gain')),32)))
            src=floats(g('out_project')); head=array('f',[0])*512
            for r in range(16):
                for col in range(32):head[axis_a(r)*32+axis_b(col)]=src[r*32+col]
            self.save('post70-head',concat(head[:32],head[64:96],head[128:160]))
            self.save('post70-history-head',head[192:224])

    def run(self):
        channels={0:32,1:32,4:32,5:64,8:64,9:128,14:128,15:256,22:256,48:256,55:256,56:128,61:128,62:64,65:64,66:32,69:32,70:32}
        for block, role in self.manifest['logical_to_legacy_role'].items():
            block=int(block)
            if role in channels:
                self.swin(block,role,channels[role], 'unpacked-preblock' if role==0 else 'unpacked-postblock' if role==70 else 'unpacked',role in (48,56,62,66))
            else: self.deep(block,role)
        if self.written != set(self.manifest['files']): raise ValueError('Incomplete model conversion')
        (self.output/'model-manifest.json').write_text(json.dumps(self.manifest,indent=2)+'\n',encoding='utf-8')

    def deep(self, block, role):
        if role in (23,30,40,47):
            raw=self.entries[f'inventory/weights/block{block}.layer0.layer.bin']
            pre=scatter_fp8(raw[:262144],[3]+list(range(6,14)),[1,0,4,5,2,14,15,16,17])
            expand=array('f'); contract=array('f')
            for g in range(8):
                expand.extend(scatter_fp8(raw[0x40000+g*16384:0x40000+(g+1)*16384],[3]+list(range(6,13)),[1,0,4,5,2,13]))
                contract.extend(scatter_fp8(raw[0x60000+g*16384:0x60000+(g+1)*16384],[3]+list(range(6,11)),[1,0,4,5,2,11,12,13]))
            self.save(f'block{role}-ffwd',concat(pre,expand,contract))
            g=lambda layer,name:self.get(block,name,'unpacked-splitswin',layer)
            self.save(f'block{role}-ffwd-projection',concat(matrix(g(1,'weight'),512,512),skip(floats(g(1,'skip_weight')),512)))
            self.save(f'block{role}-attention',concat(qkv(g(2,'qkv'),512),matrix(g(3,'weight'),512,512),bias(g(2,'attn_pos_bias'),16),floats(g(2,'tail'),'f'),skip(floats(g(3,'skip_weight')),512)))
            if role==30:self.save('head-matrix',matrix(g(4,'weight'),1024,512))
        elif 31<=role<=38:
            g=lambda layer,name:self.get(block,name,'unpacked-vit',layer)
            self.save(f'block{role}-expand',matrix(g(0,'weight'),4096,1024,vit_axis,vit_axis))
            for layer,name,inputs in ((1,'contract',4096),(4,'projection',1024)):
                self.save(f'block{role}-{name}',concat(matrix(g(layer,'weight'),1024,inputs,vit_axis,vit_axis),reorder(floats(g(layer,'skip_weight')),[vit_axis(i) for i in range(1024)])))
            raw=self.entries[f'inventory/weights/block{block}.layer2.layer.bin']
            parts=[bytearray() for _ in range(3)]
            for i in range(128,len(raw),3072):
                for part in range(3):parts[part].extend(raw[i+part*1024:i+(part+1)*1024])
            self.save(f'block{role}-qkv',concat(*(scatter_fp8(part,[6,3,9,7,8]+list(range(10,15)),[0,1,2,4,5]+list(range(15,20))) for part in parts),floats(raw[:128],'f')))
        elif role==39:
            self.save('decoder39-weights',concat(matrix(self.get(block,'weight','unpacked-splitswin'),512,1024),skip(floats(self.get(block,'skip_weight','unpacked-splitswin')),512)))
        else:raise ValueError('Unexpected deep block '+str(role))


def role_name(role): return 'post70' if role==70 else f'block{role}'


def linked(path):
    return path.is_symlink() or (path.exists() and bool(getattr(path.stat(), 'st_file_attributes', 0) & 0x400))


def verify_assets(directory):
    manifest=json.loads(MANIFEST.read_text(encoding='utf-8-sig'))
    directory=Path(directory)
    if any(linked(p) for p in (directory,*directory.parents)) or not directory.is_dir():return False
    try:
        for name,item in manifest['files'].items():
            p=directory/name
            if p.suffix=='.f16' and p.with_suffix('.f32').exists():return False
            if linked(p) or p.stat().st_size!=item['bytes'] or hashlib.sha256(p.read_bytes()).hexdigest()!=item['sha256']:return False
        text=(directory/'model-manifest.json').read_text(encoding='utf-8-sig')
        m=json.loads(text)
        return '"model": "small"' in text and m['model']=='small' and m['source_dll_sha256']==manifest['source_dll_sha256']
    except (OSError,ValueError,KeyError):return False


def convert(pack, output, replace=False):
    entries = read_pack(pack)
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8-sig'))
    output = Path(output).absolute()
    # Refuse a linked destination rather than renaming an unrelated directory.
    for p in (output,*output.parents):
        if linked(p):
            raise ValueError('Linked destination is not supported: '+str(p))
    if output.exists() and not replace:
        raise ValueError('Destination already exists; choose a new output directory or --replace')
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.lmxxf-small-',dir=output.parent) as tmp:
        staged=Path(tmp)/'small'; staged.mkdir()
        Converter(entries,staged,manifest).run()
        # Both validation and conversion finish before touching the old directory.
        backup=None
        if output.exists():
            backup=output.with_name(output.name+'.previous-'+uuid.uuid4().hex)
            output.rename(backup)
        try:staged.rename(output)
        except OSError:
            if backup is not None:backup.rename(output)
            raise
        if backup is not None:print('Previous assets preserved at '+str(backup))
    return output


def main():
    p=argparse.ArgumentParser(description=__doc__)
    src=p.add_mutually_exclusive_group(required=True)
    src.add_argument('--pack',type=Path,help='Verified dlssnr-small.bin')
    src.add_argument('--dll',type=Path,help='User-supplied nvngx_dlssnr.dll 310.9.16; extracted offline')
    src.add_argument('--verify',type=Path,help='Verify all installed Small assets')
    p.add_argument('--output',type=Path)
    p.add_argument('--replace',action='store_true',help='Install atomically; preserve the previous directory')
    args=p.parse_args()
    if args.verify:return 0 if verify_assets(args.verify) else 1
    if args.output is None:p.error('--output is required for conversion')
    try:
        if args.dll:
            here=Path(__file__).resolve().parent
            extractor=here/'mochizuki-model.py'
            if not extractor.is_file():extractor=here/'model-tools'/'mochizuki-model.py'
            spec=importlib.util.spec_from_file_location('mochizuki_model',extractor)
            module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
            vendor=here/'model-tools'
            if not vendor.is_dir():vendor=here.parents[1]/'third_party/mochizuki/linux/package/model-tools'
            with tempfile.TemporaryDirectory(prefix='lmxxf-extract-') as tmp:
                pack=Path(tmp)/'dlssnr-small.bin'
                module.extract(args.dll.resolve(),pack,vendor,models='small',temporary_parent=Path(tmp))
                convert(pack,args.output,args.replace)
        else:convert(args.pack,args.output,args.replace)
    except (OSError,ValueError,KeyError,struct.error) as exc:
        print('ERROR: '+str(exc),file=sys.stderr);return 1
    return 0


if __name__ == '__main__': raise SystemExit(main())
