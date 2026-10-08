"""Independent checks on the delivered PCB, schematic netlist and via geometry."""
from inspect_sources import *
import math
import xml.etree.ElementTree as ET

def main():
    board=parse((ROOT/'flashlight.kicad_pcb').read_text(encoding='utf-8'))
    pads=[]
    for fp in children(board,'footprint'):
        ref=next(p[2] for p in children(fp,'property') if p[1]=='Reference')
        at=child(fp,'at');x,y=map(float,at[1:3]);angle=float(at[3]) if len(at)>3 else 0
        rad=math.radians(angle)
        for pad in children(fp,'pad'):
            if not any(v in ['F.Cu','B.Cu','*.Cu'] for v in child(pad,'layers')[1:]):continue
            pa=child(pad,'at');px,py=map(float,pa[1:3])
            px,py=x+px*math.cos(rad)+py*math.sin(rad),y-px*math.sin(rad)+py*math.cos(rad)
            w,h=map(float,child(pad,'size')[1:3])
            # Pad angle in the saved KiCad board is absolute.
            rotation=float(pa[3]) if len(pa)>3 else 0
            pads.append(dict(ref=ref,pin=pad[1],x=px,y=py,w=w,h=h,angle=rotation,drill=child(pad,'drill')))
    failures=[]
    for via in children(board,'via'):
        x,y=map(float,child(via,'at')[1:3]);radius=float(child(via,'size')[1])/2
        for p in pads:
            rad=math.radians(p['angle']);dx=x-p['x'];dy=y-p['y']
            lx=dx*math.cos(rad)-dy*math.sin(rad);ly=dx*math.sin(rad)+dy*math.cos(rad)
            # Conservative rectangle encloses rounded/oval copper pads too.
            gap=math.hypot(max(abs(lx)-p['w']/2,0),max(abs(ly)-p['h']/2,0))-radius
            if gap < .149:
                failures.append(f'Via {x,y}: gap {gap:.4f} mm to {p["ref"]}.{p["pin"]}')
    for p in pads:
        if p['drill'] and p['ref']=='U1': failures.append('Drilled module thermal pad remains')
    expected=json.loads((ROOT/'tools'/'expected_nets.json').read_text())
    xml=ET.parse(ROOT/'netlist.xml')
    actual={}
    for net in xml.findall('.//nets/net'):
        name=net.attrib['name'].lstrip('/')
        for node in net.findall('node'):actual[(node.attrib['ref'],node.attrib['pin'])]=name
    for ref,pins in expected.items():
        for pin,net in pins.items():
            if net is not None and actual.get((ref,pin))!=net:
                failures.append(f'Netlist mismatch {ref}.{pin}: {actual.get((ref,pin))} != {net}')
    report=dict(vias=len(children(board,'via')),copper_pads=len(pads),minimum_via_pad_gap_mm=.15,failures=failures)
    (ROOT/'verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
    if failures:raise SystemExit(1)

if __name__=='__main__':main()
