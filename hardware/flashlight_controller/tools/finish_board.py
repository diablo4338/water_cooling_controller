"""Place side-aware assembly legends for the two-sided compact board."""
from build_project import *

WIDTH=42
HEIGHT=48
TOP=-8

def overlaps(a,b,gap=.15):
    return a[0]<b[2]+gap and a[2]>b[0]-gap and a[1]<b[3]+gap and a[3]>b[1]-gap

def main():
    path=ROOT/'flashlight.kicad_pcb'
    b=parse(path.read_text(encoding='utf-8'))
    b[:]=[z for z in b if not (isinstance(z,list) and z[0]=='gr_text')]
    # Remove an unused signal escape via if routing took a different layer change.
    # Ground vias may connect through filled planes, so they are not included.
    for via in list(children(b,'via')):
        if tuple(map(float,child(via,'at')[1:3])) not in [(24.,8.8),(24.,9.5),(24.,6.7)]:continue
        net=child(via,'net')[1];x,y=map(float,child(via,'at')[1:3]);radius=float(child(via,'size')[1])/2
        attached=False
        for tr in children(b,'segment'):
            if child(tr,'layer')[1]!='F.Cu' or child(tr,'net')[1]!=net:continue
            px,py=map(float,child(tr,'start')[1:3]);qx,qy=map(float,child(tr,'end')[1:3]);dx=qx-px;dy=qy-py
            t=max(0,min(1,((x-px)*dx+(y-py)*dy)/max(dx*dx+dy*dy,1e-12)))
            if math.hypot(x-px-t*dx,y-py-t*dy)<=radius+float(child(tr,'width')[1])/2:attached=True;break
        if not attached:b.remove(via)
    module=next(f for f in children(b,'footprint') if any(p[1]=='Reference' and p[2]=='U1' for p in children(f,'property')))
    for escape,origin,pin in [((24.,8.8),(22.25,8.75),'6'),((24.,9.5),(22.25,8.75),'6'),((24.,6.7),(22.25,7.25),'5')]:
        if any(tuple(map(float,child(v,'at')[1:3]))==escape for v in children(b,'via')):continue
        pad=next(p for p in children(module,'pad') if p[1]==pin);net=child(pad,'net')[1]
        manual=[];attached=False
        for tr in children(b,'segment'):
            if child(tr,'layer')[1]!='B.Cu' or child(tr,'net')[1]!=net:continue
            ends={tuple(map(float,child(tr,k)[1:3])) for k in ['start','end']}
            if ends=={origin,escape}:manual.append(tr)
            elif any(math.dist(pt,escape)<.01 for pt in ends):attached=True
        if attached and not manual:
            b.append(n('segment',n('start',*[a(v) for v in origin]),n('end',*[a(v) for v in escape]),n('width',a(.18)),n('layer','B.Cu'),n('net',net),n('uuid',uid('retained-escape-'+str(escape)))))
        elif not attached:
            for tr in manual:b.remove(tr)
    occupied={'F':[],'B':[]}; components=[]
    for fp in children(b,'footprint'):
        side=child(fp,'layer')[1][0]
        ref=next(p[2] for p in children(fp,'property') if p[1]=='Reference')
        at=child(fp,'at');cx,cy=map(float,at[1:3]);angle=float(at[3]) if len(at)>3 else 0
        rad=math.radians(angle)
        def world(x,y):return cx+x*math.cos(rad)+y*math.sin(rad),cy-x*math.sin(rad)+y*math.cos(rad)
        pts=[]
        for shape in fp:
            if isinstance(shape,list) and child(shape,'layer',[None,''])[1]==side+'.CrtYd':
                if shape[0]=='fp_circle':
                    ctr=child(shape,'center');end=child(shape,'end');px,py=map(float,ctr[1:3]);radius=math.dist((px,py),tuple(map(float,end[1:3])))
                    pts.extend([world(px-radius,py-radius),world(px+radius,py+radius)])
                    continue
                for key in ['start','end','xy']:
                    for pt in walk(shape,key):pts.append(world(*map(float,pt[1:3])))
        if not pts:pts=[(cx-1.7,cy-1.7),(cx+1.7,cy+1.7)]
        box=(min(p[0] for p in pts),min(p[1] for p in pts),max(p[0] for p in pts),max(p[1] for p in pts))
        if not ref.startswith('J'):occupied[side].append(box)
        for pad in children(fp,'pad'):
            layers=child(pad,'layers')[1:]
            if '*.Cu' not in layers and not ref.startswith('J'):continue
            pa=child(pad,'at');x,y=world(*map(float,pa[1:3]));w,h=map(float,child(pad,'size')[1:3])
            for ss in (['F','B'] if '*.Cu' in layers else [side]):occupied[ss].append((x-w/2,y-h/2,x+w/2,y+h/2))
        prop=next(p for p in children(fp,'property') if p[1]=='Reference')
        setnode(prop,'hide',a('yes'))
        setnode(prop,'layer',side+'.Fab')
        # Antenna body is supported by FR4; preserve the fabrication outline.
        if ref=='U1':fp[:]=[z for z in fp if not (isinstance(z,list) and z[0] in ['fp_line','fp_poly','fp_rect'] and child(z,'layer',[None,''])[1]==side+'.SilkS')]
        if not ref.startswith('H'):components.append((ref,side,cx,cy,box))
    # Visible actuator frames between the pads; the reference footprint has no silk body.
    marker_ids={uid('button-frame-'+r) for r in ['SW1','SW2','SW3']}|{uid('button-actuator-'+r) for r in ['SW1','SW2','SW3']}
    b[:]=[z for z in b if not (isinstance(z,list) and child(z,'uuid',[None,None])[1] in marker_ids)]
    for fp in children(b,'footprint'):
        ref=next(p[2] for p in children(fp,'property') if p[1]=='Reference')
        if ref not in ['SW1','SW2','SW3']:continue
        cx,cy=map(float,child(fp,'at')[1:3])
        b.append(n('gr_rect',n('start',a(cx-2),a(cy-4.3)),n('end',a(cx+2),a(cy+.5)),n('stroke',n('width',a(.12)),n('type',a('default'))),n('fill',a('none')),n('layer','F.SilkS'),n('uuid',uid('button-frame-'+ref))))
        b.append(n('gr_circle',n('center',a(cx),a(cy-1.9)),n('end',a(cx+1.1),a(cy-1.9)),n('stroke',n('width',a(.12)),n('type',a('default'))),n('fill',a('none')),n('layer','F.SilkS'),n('uuid',uid('button-actuator-'+ref))))
    texts={'F':[],'B':[]}
    def label(txt,x,y,side,size=.8):
        w=len(txt)*size*.8;box=(x-w/2,y-size*.7,x+w/2,y+size*.7)
        if box[0]<.25 or box[1]<TOP+.25 or box[2]>WIDTH-.25 or box[3]>HEIGHT-.25:return False
        if any(overlaps(box,z,.2) for z in occupied[side]+texts[side]):return False
        eff=n('effects',n('font',n('size',a(size),a(size)),n('thickness',a(.12))))
        if side=='B':eff.append(n('justify',a('mirror')))
        b.append(n('gr_text',txt,n('at',a(x),a(y),a(0)),n('layer',side+'.SilkS'),n('uuid',uid('rev04-label-'+side+txt+str((x,y)))),eff))
        texts[side].append(box);return True
    # Functional markings are mandatory and placed before assembly references.
    required=[('FLASH',14.5,3.8,'F',1.1),('BOOT',8,6.4,'F',1.0),
              ('RESET',20,6.4,'F',1.0),('USER',34,.2,'F',1.0),
              ('UART',14.5,12.9,'F',1.0),('GND',18,14.6,'F',.9),
              ('TX',15.46,14.6,'F',.9),('RX',12.92,14.6,'F',.9),
              ('SIG',5,36.2,'F',.9),('GND',8,36.2,'F',.9),('GYRO',10.9,34,'F',1.0)]
    for side in ['F','B']:
        required.extend([('BAT',33.5,44.5,side,1.1),('+',26.3,40,side,1.4),
                         ('-',40.7,40,side,1.4),('LAMP',31.5,12.6,side,1.0),
                         ('+',24.3,17,side,1.4),('-',38.7,17,side,1.4),
                         ('NTC',9.5,28,side,1.0),('T',3,30.7,side,.9),('GND',6.5,30.7,side,.9)])
    missing=[]
    for txt,x,y,side,size in required:
        if not label(txt,x,y,side,size):missing.append((txt,x,y,side))
    if missing:raise ValueError('Required functional silk does not fit: '+str(missing))
    for ref,side,cx,cy,box in components:
        if ref.startswith(('J','SW')):continue
        found=False
        for off in [.7,1.2,1.7,2.2,3.2]:
            for delta in [0,-1,1,-2,2]:
                for x,y in [(cx+delta,box[3]+off),(cx+delta,box[1]-off),(box[2]+off+len(ref)*.24,cy+delta),(box[0]-off-len(ref)*.24,cy+delta)]:
                    if label(ref,x,y,side):found=True;break
                if found:break
            if found:break
    for z in children(b,'zone'):
        fill=child(z,'fill')
        if fill:setnode(fill,'island_removal_mode',a(0))
        z[:]=[s for s in z if not (isinstance(s,list) and s[0]=='filled_polygon')]
    path.write_text(dump(b)+'\n',encoding='utf-8')

if __name__=='__main__':main()
