"""Index test18 full frames without loading the file; optionally extract one RGBA16F tensor."""
import argparse
import json
import struct
from pathlib import Path

HEADER=struct.Struct('<12I5fQ')

def index(path):
    rows=[]
    with Path(path).open('rb') as f:
        if f.read(8)!=b'NRFFV1\0\0':raise ValueError('Not a test18 full-frame file')
        length=Path(path).stat().st_size
        while raw:=f.read(HEADER.size):
            if len(raw)!=HEADER.size:raise ValueError('Truncated header')
            r=HEADER.unpack(raw)
            frame,stage,tile,width,height,x,y,w,h,fmt=r[:10]
            if stage not in (1,2) or tile or fmt!=10 or x or y or not (0<w<=8192 and 0<h<=8192) or (width,height)!=(w,h):raise ValueError('Invalid full-frame geometry/format')
            size=w*h*8
            if f.tell()+size>length:raise ValueError('Truncated pixels')
            rows.append(dict(frame=frame,stage=stage,width=w,height=h,offset=f.tell(),bytes=size,tick=r[-1]))
            f.seek(size,1)
    if not rows:raise ValueError('No full frames')
    if len(rows)%2 or any(a['frame']!=b['frame'] or (a['stage'],b['stage'])!=(1,2) or (a['width'],a['height'])!=(b['width'],b['height']) for a,b in zip(rows[::2],rows[1::2])):raise ValueError('Unpaired input/output')
    return rows

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);p.add_argument('--frame',type=int);p.add_argument('--stage',type=int,choices=(1,2),default=1);p.add_argument('--out',type=Path);a=p.parse_args()
    rows=index(a.capture)
    if a.out:
        selected=[r for r in rows if r['frame']==a.frame and r['stage']==a.stage]
        if len(selected)!=1:p.error('Choose a listed --frame and --stage')
        r=selected[0]
        with a.capture.open('rb') as f,a.out.open('wb') as dst:
            f.seek(r['offset']);left=r['bytes']
            while left:
                chunk=f.read(min(left,1024*1024));dst.write(chunk);left-=len(chunk)
        a.out.with_suffix(a.out.suffix+'.json').write_text(json.dumps(r,indent=2),encoding='utf-8')
    print(json.dumps(rows,indent=2))

if __name__=='__main__':main()
