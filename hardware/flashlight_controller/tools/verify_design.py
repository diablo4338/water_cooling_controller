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
    # Mechanical acceptance checks read the delivered board, not the layout spec.
    fps={next(p[2] for p in children(f,'property') if p[1]=='Reference'):f for f in children(board,'footprint')}
    boot=child(fps['SW1'],'at');reset=child(fps['SW3'],'at')
    button_pitch=math.dist(tuple(map(float,boot[1:3])),tuple(map(float,reset[1:3])))
    if any(child(fps[r],'layer')[1]!='F.Cu' for r in ['SW1','SW2','SW3']) or abs(float(boot[2])-float(reset[2]))>1e-6 or button_pitch<10:
        failures.append('BOOT and RESET must be on F.Cu in one row, at least 10 mm apart')
    legends={t[1] for t in children(board,'gr_text') if child(t,'layer')[1] in ['F.SilkS','B.SilkS']}
    if not {'BOOT','RESET','USER','BAT','LAMP','NTC','GYRO','TX','RX','GND','+','-'}.issubset(legends):
        failures.append('Required user-facing silkscreen legends are missing')
    uart_pads={p[1] for p in children(fps['J4'],'pad')}
    if uart_pads!={'1','2','3'}:failures.append('UART must have exactly three pads: GND, TX, RX')
    holes={ref:tuple(map(float,child(fps[ref],'at')[1:3])) for ref in ['H1','H2','H3','H4']}
    h1,h2,h3,h4=[holes[r] for r in ['H1','H2','H3','H4']]
    dx=h2[0]-h1[0];dy=h3[1]-h1[1]
    if not (h1[1]==h2[1] and h3[1]==h4[1] and h1[0]==h3[0] and h2[0]==h4[0] and dx>0 and dy>0 and abs(dx-round(dx))<1e-6 and abs(dy-round(dy))<1e-6):
        failures.append('Mounting holes do not form an integer-pitch rectangle')
    edgepts=[]
    for line in children(board,'gr_line'):
        if child(line,'layer',[None,''])[1]=='Edge.Cuts':
            edgepts += [tuple(map(float,child(line,k)[1:3])) for k in ['start','end']]
    xmin=min(p[0] for p in edgepts);xmax=max(p[0] for p in edgepts)
    ymin=min(p[1] for p in edgepts);ymax=max(p[1] for p in edgepts)
    fp=fps['U1'];at=child(fp,'at');cx,cy=map(float,at[1:3]);rad=math.radians(float(at[3]))
    body=[]
    for sh in children(fp,'fp_line'):
        if child(sh,'layer',[None,''])[1]!='B.Fab':continue
        for k in ['start','end']:
            x,y=map(float,child(sh,k)[1:3]);body.append((cx+x*math.cos(rad)+y*math.sin(rad),cy-x*math.sin(rad)+y*math.cos(rad)))
    if not body or any(not (xmin<x<xmax and ymin<y<ymax) for x,y in body):
        failures.append('ESP32 module body/antenna extends beyond the board outline')
    keepouts=children(fp,'zone')
    if not any(child(z,'keepout') and all(l in child(z,'layers',[None])[1:] for l in ['F.Cu','B.Cu']) and all(child(child(z,'keepout'),k,[None,''])[1]=='not_allowed' for k in ['tracks','vias','pads','copperpour']) for z in keepouts):
        failures.append('All-layer antenna copper keepout missing')
    schematic=parse((ROOT/'flashlight.kicad_sch').read_text(encoding='utf-8'))
    sheetfiles={p[2] for sh in children(schematic,'sheet') for p in children(sh,'property') if p[1]=='Sheetfile'}
    if sheetfiles!={'sense.kicad_sch','power.kicad_sch','controller.kicad_sch'}:failures.append('Expected three logical circuit sheets')
    for filename in sorted(sheetfiles):
        doc=parse((ROOT/filename).read_text(encoding='utf-8'))
        if children(doc,'sheet') or len(children(doc,'wire'))<30:failures.append('Logical block must contain a complete wired circuit: '+filename)
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
    report=dict(boot_reset_pitch_mm=button_pitch,vias=len(children(board,'via')),copper_pads=len(pads),minimum_via_pad_gap_mm=.15,mounting_holes_mm=holes,mounting_pitch_mm=[dx,dy],board_size_mm=[xmax-xmin,ymax-ymin],module_fully_supported_by_board=bool(body) and all(xmin<x<xmax and ymin<y<ymax for x,y in body),failures=failures)
    (ROOT/'verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
    if failures:raise SystemExit(1)

if __name__=='__main__':main()
