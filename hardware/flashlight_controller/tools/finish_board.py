"""Add assembly labels and LDO heat spreading to the routed PCB."""
from build_project import *

def bbox(points):
    return min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)

def overlaps(a,b,gap=.15):
    return a[0]<b[2]+gap and a[2]>b[0]-gap and a[1]<b[3]+gap and a[3]>b[1]-gap

def main():
    path=ROOT/'flashlight.kicad_pcb'
    b=parse(path.read_text(encoding='utf-8'))
    modern=int(child(b,'version')[1])>=20260206
    generated_ids={uid('ldo-thermal-'+layer) for layer in ['B.Cu','F.Cu']}
    generated_ids.update(uid('thermal-via'+str(y)) for y in [18.8,20,21.2])
    b[:]=[s for s in b if not (isinstance(s,list) and child(s,'uuid',[None,None])[1] in generated_ids)]
    ids={x[2]:x[1] for x in children(b,'net')}
    if modern:ids={x[1]:x[1] for x in walk(b,'net')}
    fps=children(b,'footprint')
    boxes={}
    for fp in fps:
        ref=next(p[2] for p in children(fp,'property') if p[1]=='Reference')
        pos=child(fp,'at');x,y=map(float,pos[1:3]);ang=math.radians(float(pos[3]) if len(pos)>3 else 0)
        def world(px,py):return x+px*math.cos(ang)+py*math.sin(ang),y-px*math.sin(ang)+py*math.cos(ang)
        pts=[]
        for shape in fp:
            if isinstance(shape,list) and shape[0] in ['fp_line','fp_rect','fp_poly','fp_circle'] and child(shape,'layer',[None,''])[1]=='B.CrtYd':
                for key in ['start','end','xy']:
                    for p in walk(shape,key):pts.append(world(*map(float,p[1:3])))
        if not pts:
            pts=[(x-2,y-2),(x+2,y+2)]
        boxes[ref]=bbox(pts)
        # The antenna physically overhangs the board: don't print that part of its outline.
        if ref=='U1':
            fp[:]=[s for s in fp if not (isinstance(s,list) and s[0] in ['fp_line','fp_poly','fp_rect'] and child(s,'layer',[None,''])[1]=='B.SilkS')]
    # Move connector/UI legends clear of all solder pads and component outlines.
    b[:]=[s for s in b if not (isinstance(s,list) and s[0]=='gr_text' and (str(s[1]).startswith('UART:') or str(s[1]).startswith('ANTENNA:') or s[1] in ['C1 +','C1 -','+','-']))]
    legend=[('UART: G TX RX EN BOOT 3V3',39,29),('+',61.5,45.5),('-',71.5,45.5)]
    for txt,x,y in legend:
        b.append(n('gr_text',txt,n('at',a(x),a(y),a(0)),n('layer','B.SilkS'),n('uuid',uid('final-label'+txt)),n('effects',n('font',n('size',a(.85),a(.85)),n('thickness',a(.12))),n('justify',a('mirror')))))
    textboxes=[]
    for t in children(b,'gr_text'):
        x,y=map(float,child(t,'at')[1:3]);fs=float(child(child(child(t,'effects'),'font'),'size')[1])
        w=len(t[1])*fs*.68
        textboxes.append((x-w/2,y-fs*.65,x+w/2,y+fs*.65))
    for fp in fps:
        prop=next(p for p in children(fp,'property') if p[1]=='Reference');ref=prop[2]
        if ref.startswith('H'):continue
        pos=child(fp,'at');cx,cy=map(float,pos[1:3]);angle=float(pos[3]) if len(pos)>3 else 0
        box=boxes[ref];w=len(ref)*.8;h=1.1
        options=[(21,15.6)] if ref=='U1' else []
        for off in [.5,1,1.5,2,3,4]:
            for t in [0,-1,1,-2,2]:
                options.extend([(cx+t,box[3]+off+h/2),(cx+t,box[1]-off-h/2),(box[2]+off+w/2,cy+t),(box[0]-off-w/2,cy+t)])
        selected=None
        for x,y in options:
            box2=(x-w/2,y-h/2,x+w/2,y+h/2)
            if box2[0]<.5 or box2[1]<.5 or box2[2]>74.5 or box2[3]>64.5:continue
            if any(overlaps(box2,bb,.12) for bb in boxes.values()):continue
            if any(overlaps(box2,bb,.2) for bb in textboxes):continue
            selected=(x,y);textboxes.append(box2);break
        if selected:
            x,y=selected;dx=x-cx;dy=y-cy;rad=math.radians(angle)
            lx=dx*math.cos(rad)-dy*math.sin(rad);ly=dx*math.sin(rad)+dy*math.cos(rad)
            setnode(prop,'at',a(lx),a(ly),a(-angle))
            prop[:]=[p for p in prop if not (isinstance(p,list) and p[0]=='hide')]
            setnode(prop,'effects',n('font',n('size',a(.85),a(.85)),n('thickness',a(.12))),n('justify',a('mirror')))
            setnode(prop,'layer','B.SilkS')
        else:print('No silk location for',ref)
    for z in children(b,'zone'):
        fill=child(z,'fill')
        if fill:setnode(fill,'island_removal_mode',a(0))
        z[:]=[s for s in z if not (isinstance(s,list) and s[0]=='filled_polygon')]
    # TPS7A02 in SOT-23-5 has no exposed tab or thermal via array.
    path.write_text(dump(b)+'\n',encoding='utf-8')

if __name__=='__main__':main()
