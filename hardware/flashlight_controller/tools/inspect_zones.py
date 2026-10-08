from build_project import *
from collections import defaultdict

def inside(p,poly):
    x,y=p;res=False
    for q,r in zip(poly,poly[1:]+poly[:1]):
        if (q[1]>y)!=(r[1]>y) and x<(r[0]-q[0])*(y-q[1])/(r[1]-q[1])+q[0]:res=not res
    return res

b=parse((ROOT/'flashlight.kicad_pcb').read_text())
vias=[tuple(map(float,child(v,'at')[1:3])) for v in children(b,'via') if child(v,'net')[1]=='GND']
polys=[]
for z in children(b,'zone'):
    if child(z,'net')[1]!='GND':continue
    layer=child(z,'layer')[1]
    for f in children(z,'filled_polygon'):
        pts=[tuple(map(float,p[1:3])) for p in children(child(f,'pts'),'xy')]
        ids=[i for i,v in enumerate(vias) if inside(v,pts)]
        box=[min(p[0] for p in pts),min(p[1] for p in pts),max(p[0] for p in pts),max(p[1] for p in pts)]
        print(layer,'box',box,'vias',ids)
        polys.append((layer,pts,ids))
print('VIAS',list(enumerate(vias)))
