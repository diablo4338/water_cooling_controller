"""Route the specified board, reserving wide explicit paths for lamp current.

The small grid router handles only low-current interconnects. Power and switch
node paths are specified explicitly. The output is always checked by KiCad DRC.
"""
from build_project import *
import heapq
import time
from collections import defaultdict

STEP=.1
NX=751
NY=651
SIZE=NX*NY
ROUTE_W=.2
CLEAR=.165

class Router:
    def __init__(self):
        self.layout=json.loads((ROOT/'tools'/'layout.json').read_text())
        self.ids=self.layout['nets']
        self.pads=self.layout['pads']
        self.board=parse((ROOT/'unrouted.kicad_pcb').read_text(encoding='utf-8'))
        self.grid=[[0]*SIZE for _ in range(2)]
        self.vgrid=[[0]*SIZE for _ in range(2)]
        self.segments=[]
        self.vias=[]
        self.connected=defaultdict(set)
        self.padmap={}
        for p in self.pads:
            self.padmap.setdefault((p['ref'],p['pin']),p)
            net=self.ids.get(p['net'],-1)
            for layer in p['layers']:
                self.rect(self.grid[layer],p['x'],p['y'],p['w'],p['h'],CLEAR+ROUTE_W/2+.02,net)
                # Forbid vias even on same-net pads: annulus + 0.15mm gap.
                self.rect(self.vgrid[layer],p['x'],p['y'],p['w'],p['h'],.3+.15,-1)
        # Board edges and antenna clearance. Module antenna is beyond y=0.15.
        for layer in range(2):
            for x,y,w,h in [(37.5,0,75,.7),(37.5,65,75,.7),(0,32.5,.7,65),(75,32.5,.7,65),(21,-8,29,16.3)]:
                self.rect(self.grid[layer],x,y,w,h,0,-1)
                self.rect(self.vgrid[layer],x,y,w,h,.2,-1)
        for p in self.pads:
            if p['drill']:
                for layer in [0,1]:self.disk(self.vgrid[layer],p['x'],p['y'],p['drill']/2+.15+.26,-1)

    @staticmethod
    def merge(old,net):
        return net if old in (0,net) else -1

    def rect(self,g,x,y,w,h,gap,net):
        x0=max(0,math.ceil((x-w/2-gap)/STEP));x1=min(NX-1,math.floor((x+w/2+gap)/STEP))
        y0=max(0,math.ceil((y-h/2-gap)/STEP));y1=min(NY-1,math.floor((y+h/2+gap)/STEP))
        for yy in range(y0,y1+1):
            k=yy*NX
            for xx in range(x0,x1+1):
                j=k+xx;g[j]=self.merge(g[j],net)

    def disk(self,g,x,y,r,net):
        for yy in range(max(0,math.ceil((y-r)/STEP)),min(NY-1,math.floor((y+r)/STEP))+1):
            dy=yy*STEP-y
            dx=math.sqrt(max(0,r*r-dy*dy))
            for xx in range(max(0,math.ceil((x-dx)/STEP)),min(NX-1,math.floor((x+dx)/STEP))+1):
                j=yy*NX+xx;g[j]=self.merge(g[j],net)

    def raster(self,p,q):
        steps=max(1,math.ceil(math.dist(p,q)/(.07)))
        return [(p[0]+(q[0]-p[0])*t/steps,p[1]+(q[1]-p[1])*t/steps) for t in range(steps+1)]

    def idx(self,x,y,layer):
        return layer*SIZE+round(y/STEP)*NX+round(x/STEP)

    def xyz(self,k):
        layer,k=divmod(k,SIZE);y,x=divmod(k,NX)
        return x*STEP,y*STEP,layer

    def segment(self,net,p,q,width=.2,layer=1):
        if math.dist(p,q)<.00001:return
        ni=self.ids[net]
        self.segments.append(dict(net=net,p=p,q=q,width=width,layer=layer))
        for x,y in self.raster(p,q):
            self.disk(self.grid[layer],x,y,width/2+CLEAR+ROUTE_W/2+.015,ni)
            self.disk(self.vgrid[layer],x,y,width/2+CLEAR+.3+.02,ni)
            self.connected[net].add(self.idx(x,y,layer))
        self.board.append(n('segment',n('start',*[a(v) for v in p]),n('end',*[a(v) for v in q]),n('width',a(width)),n('layer','B.Cu' if layer else 'F.Cu'),n('net',a(ni)),n('uuid',uid('track'+str(len(self.segments))))))

    def via(self,net,x,y):
        if any(math.dist((x,y),(v['x'],v['y']))<.05 and v['net']==net for v in self.vias):return
        for p in self.pads:
            if abs(x-p['x']) < p['w']/2+.45-1e-6 and abs(y-p['y']) < p['h']/2+.45-1e-6:
                raise ValueError(f'Via overlaps pad: {net} {x,y} {p["ref"]}.{p["pin"]}')
        self.vias.append(dict(net=net,x=x,y=y))
        ni=self.ids[net]
        for layer in [0,1]:
            self.disk(self.grid[layer],x,y,.3+CLEAR+ROUTE_W/2+.015,ni)
            self.disk(self.vgrid[layer],x,y,.3+CLEAR+.3+.02,ni)
            self.connected[net].add(self.idx(x,y,layer))
            self.disk(self.vgrid[layer],x,y,.56,-1)
        self.board.append(n('via',n('at',a(x),a(y)),n('size',a(.6)),n('drill',a(.3)),n('layers','F.Cu','B.Cu'),n('net',a(ni)),n('uuid',uid('via'+str(len(self.vias))))))

    def pad(self,ref,pin):
        p=self.padmap[(ref,str(pin))]
        return p['x'],p['y']

    def wire(self,net,points,width=.2,layer=1):
        for p,q in zip(points,points[1:]):self.segment(net,p,q,width,layer)

    def power(self):
        # Reserve a return for the ground pin between the two analogue inputs.
        self.wire('GND',[self.pad('U4',7),(40.5,51.5)],.2)
        self.via('GND',40.5,51.5)
        # Short, wide lamp path. Narrow necks only at SOT-23 pads.
        self.wire('PACK_PLUS',[self.pad('J1',1),(62,52),self.pad('R1',1)],2)
        self.wire('SYS_PLUS',[self.pad('R1',2),(52.0375,50),(54,48),(60,48),(60,42),(58.5,40.5)],2)
        self.wire('SYS_PLUS',[(58.5,40.5),self.pad('Q1',2)],.5)
        self.wire('SYS_PLUS',[(60,45.5),self.pad('C1',1)],1.5)
        self.wire('LAMP_PLUS',[self.pad('Q1',3),(53.5,39)],.5)
        self.wire('LAMP_PLUS',[(53.5,39),(52.5,38),(52.5,35),(60,35),(62,37),self.pad('J2',1)],2)
        # High-frequency buck loop, kept entirely on B.Cu.
        self.wire('SW',[self.pad('U2',6),(26,44),(24,42),self.pad('D1',1)],.5)
        self.wire('SW',[self.pad('D1',1),(24,38),self.pad('L1',1)],.9)
        self.wire('SW',[self.pad('C7',2),(26,42),(24,42)],.3)
        self.wire('BOOST',[self.pad('U2',1),self.pad('C7',1)],.25)
        self.wire('BOOST',[self.pad('C7',1),(31,42),self.pad('D2',1)],.2)
        # The diode / input-cap ground loop is explicit, with plane vias.
        self.wire('GND',[self.pad('D1',2),(19,42),(19,47.05),self.pad('C5',2),self.pad('C4',2)],.8)
        self.wire('SYS_PLUS',[self.pad('U2',5),self.pad('U2',4),(26.8625,48.95),self.pad('C4',1),self.pad('C5',1)],.5)
        self.wire('5V',[self.pad('L1',2),(18.925,29),self.pad('C8',1)],.8)
        self.wire('5V',[self.pad('C8',1),self.pad('C9',1)],.8)
        # All current entering the buck is measured; no wire bypasses R1.

    def valid(self,k,net):
        z,j=divmod(k,SIZE)
        return self.grid[z][j] in (0,self.ids[net])

    def route(self,net,start,goals,limit=500000):
        ni=self.ids[net]
        goals={g for g in goals if 0<=g<2*SIZE and self.valid(g,net)}
        if not goals: return None
        targetlist=list(goals)
        sx,sy,sz=self.xyz(start)
        # A bounding-box heuristic remains admissible for an entire routed tree.
        coords=[self.xyz(k) for k in goals]
        xmin=min(p[0] for p in coords)/STEP;xmax=max(p[0] for p in coords)/STEP
        ymin=min(p[1] for p in coords)/STEP;ymax=max(p[1] for p in coords)/STEP
        def heuristic(k):
            z,j=divmod(k,SIZE);yy,xx=divmod(j,NX)
            dx=max(xmin-xx,0,xx-xmax);dy=max(ymin-yy,0,yy-ymax)
            return max(dx,dy)+.41421356*min(dx,dy)
        heap=[(heuristic(start),0,start)]
        cost={start:0};parent={}
        shifts=[(-1,0,1),(1,0,1),(0,-1,1),(0,1,1),(-1,-1,1.41421356),(-1,1,1.41421356),(1,-1,1.41421356),(1,1,1.41421356)]
        popped=0
        while heap:
            _,g,k=heapq.heappop(heap)
            if g>cost[k]+1e-8:continue
            if k in goals:
                result=[k]
                while result[-1]!=start:result.append(parent[result[-1]])
                return result[::-1]
            popped+=1
            if popped>limit:return None
            layer,j=divmod(k,SIZE);yy,xx=divmod(j,NX)
            grid=self.grid[layer]
            for dx,dy,w in shifts:
                x=xx+dx;y=yy+dy
                if not (4<x<NX-5 and 4<y<NY-5):continue
                j2=y*NX+x
                if grid[j2] not in (0,ni):continue
                if dx and dy and (grid[yy*NX+x] not in (0,ni) or grid[y*NX+xx] not in (0,ni)):continue
                k2=layer*SIZE+j2
                # Slight preference for bottom routing; keep F.Cu mostly ground.
                ng=g+w*(1.04 if layer==0 else 1)
                if ng<cost.get(k2,1e99):
                    cost[k2]=ng;parent[k2]=k
                    heapq.heappush(heap,(ng+heuristic(k2)*1.08,ng,k2))
            if self.vgrid[0][j] in (0,ni) and self.vgrid[1][j] in (0,ni):
                k2=(1-layer)*SIZE+j;ng=g+35
                if ng<cost.get(k2,1e99):
                    cost[k2]=ng;parent[k2]=k
                    heapq.heappush(heap,(ng+heuristic(k2)*1.08,ng,k2))
        return None

    def emit(self,net,path):
        if not path:return
        points=[self.xyz(k) for k in path]
        begin=0;prevdir=None
        for i in range(1,len(points)):
            p=points[i-1];q=points[i]
            if p[2]!=q[2]:
                self.segment(net,points[begin][:2],p[:2],ROUTE_W,p[2])
                self.via(net,p[0],p[1]);begin=i;prevdir=None
                continue
            direction=(round((q[0]-p[0])/STEP),round((q[1]-p[1])/STEP))
            if prevdir is not None and direction!=prevdir:
                self.segment(net,points[begin][:2],p[:2],ROUTE_W,p[2]);begin=i-1
            prevdir=direction
        self.segment(net,points[begin][:2],points[-1][:2],ROUTE_W,points[-1][2])

    def route_all(self):
        bynet=defaultdict(list)
        for p in self.pads:
            if p['net'] and p['net']!='GND' and not p['net'].startswith('unconnected-'):
                if not any(math.dist((p['x'],p['y']),(q['x'],q['y']))<.8 and p['ref']==q['ref'] and p['pin']==q['pin'] for q in bynet[p['net']]):
                    bynet[p['net']].append(p)
        priority=['TEMP_EXCITE','TEMP_ADC','SCL','SDA','SENSE_PLUS','SENSE_MINUS','FB','BOOST','SW','LOAD_GATE','GATE_SINK','TEMP_RAW','SYS_PLUS','PACK_PLUS','LAMP_PLUS','5V','3V3']
        order=priority+sorted(set(bynet)-set(priority),key=lambda k:(len(bynet[k]),k))
        failures=[]
        trees={}
        for net in order:
            pads=bynet.get(net,[])
            if not pads:continue
            tree=set()
            if not tree:
                p=pads.pop(0)
                for layer in p['layers']:tree.add(self.idx(p['x'],p['y'],layer))
            while pads:
                # nearest pin to current tree, recomputed after every connection
                tcoords=[self.xyz(k) for k in tree]
                p=min(pads,key=lambda p:min((p['x']-q[0])**2+(p['y']-q[1])**2 for q in tcoords))
                pads.remove(p)
                starts=[self.idx(p['x'],p['y'],z) for z in p['layers']]
                if any(s in tree for s in starts):continue
                path=None
                for start in starts:
                    path=self.route(net,start,tree)
                    if path is not None:break
                if path is None:
                    print('FAILED',net,p['ref'],p['pin'],flush=True);failures.append((net,p['ref'],p['pin']))
                    continue
                self.emit(net,path)
                tree.update(path)
                tree.update(starts)
            print('routed',net,flush=True)
            trees[net]=tree
        remaining=[]
        for net,ref,pin in failures:
            p=self.padmap[(ref,pin)]
            start=self.idx(p['x'],p['y'],1)
            path=self.route(net,start,trees[net],limit=1000000)
            if path:
                self.emit(net,path);trees[net].update(path)
                print('recovered',net,ref,pin,flush=True)
            else:remaining.append((net,ref,pin))
        return remaining

    def grounds(self):
        ni=self.ids['GND']
        # GND via next to each ground pad, connected with a short stub if needed.
        seen=set()
        for p in self.pads:
            if p['net']!='GND' or 0 in p['layers']:continue
            if (p['ref'],p['pin']) in seen:continue
            seen.add((p['ref'],p['pin']))
            start=self.idx(p['x'],p['y'],1)
            choices=[]
            for dx in range(-45,46):
                for dy in range(-45,46):
                    x=round(p['x']/STEP)+dx;y=round(p['y']/STEP)+dy
                    if not (6<x<NX-6 and 6<y<NY-6):continue
                    j=y*NX+x
                    if self.vgrid[0][j] in (0,ni) and self.vgrid[1][j] in (0,ni):
                        dist=dx*dx+dy*dy
                        if dist>=49: choices.append((dist,j))
            choices.sort()
            if choices:
                targets={j+SIZE for _,j in choices[:60]}
                path=self.route('GND',start,targets,limit=5000)
                if path:
                    self.emit('GND',path);x,y,_=self.xyz(path[-1]);self.via('GND',x,y)
        # Stitch the broad return plane, especially beside pack and lamp pads.
        for x,y in [(69,40),(71,41),(69,50),(71,50),(73,45),(67,57),(71,57),(60,58),(54,57),(45,59),(35,57),(25,57),(15,57),(6,50),(6,40),(40,32),(50,30),(62,25),(70,20),(68,8),(38,4),(17,14),(23,14),(26,15)]:
            j=round(y/STEP)*NX+round(x/STEP)
            if self.vgrid[0][j] in (0,ni) and self.vgrid[1][j] in (0,ni):self.via('GND',x,y)
        for layer in ['F.Cu','B.Cu']:
            self.board.append(n('zone',n('net',a(ni)),n('net_name','GND'),n('layer',layer),n('uuid',uid('ground'+layer)),n('hatch',a('edge'),a(.5)),n('connect_pads',n('clearance',a(.2))),n('min_thickness',a(.2)),n('fill',a('yes'),n('thermal_gap',a(.25)),n('thermal_bridge_width',a(.35))),n('polygon',n('pts',n('xy',a(.3),a(.3)),n('xy',a(74.7),a(.3)),n('xy',a(74.7),a(64.7)),n('xy',a(.3),a(64.7))))))

    def save(self,failures):
        (ROOT/'flashlight.kicad_pcb').write_text(dump(self.board)+'\n',encoding='utf-8')
        (ROOT/'tools'/'routing.json').write_text(json.dumps(dict(segments=self.segments,vias=self.vias,failures=failures),indent=2),encoding='utf-8')
        print('Saved',len(self.segments),'segments',len(self.vias),'vias; failures:',failures,flush=True)

if __name__=='__main__':
    r=Router();r.power();failures=r.route_all();r.grounds();r.save(failures)
