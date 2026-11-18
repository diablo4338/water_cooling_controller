"""Three logical sheets, each containing complete wired circuits.
Keep physical component UUIDs/net names; only hierarchy and presentation change.
"""
from build_project import ROOT,n,a,uid,effects,child,children,walk,dump
from wired_circuits import generate as wired_template
import copy
from collections import defaultdict

G=1.27
# Bounds in the shared placement grid, paper, translated left/top margins (mm).
SHEETS={
 'sense':dict(bounds=(10,10,155,178),paper=['A4',a('portrait')],margin=(12.7,15.24),title='Battery, lamp switch and current measurement',name='Battery and lamp - INA226',page='2'),
 'power':dict(bounds=(160,10,445,106),paper=['A3'],margin=(20.32,15.24),title='On-board power: battery -> 5 V -> 3.3 V',name='Power supply - 5V and 3V3',page='3'),
 'controller':dict(bounds=(190,105,445,310),paper=['A3'],margin=(20.32,15.24),title='ESP32-C3, reset/BOOT/USER, UART, NTC and buzzer',name='ESP32 and interfaces',page='4'),
}

def xy(node,key='at'):return tuple(round(float(v)/G,6) for v in child(node,key)[1:3])
def contains(p,b):return b[0]-1e-6<=p[0]<=b[2]+1e-6 and b[1]-1e-6<=p[1]<=b[3]+1e-6

def on(p,u,v):
 return min(u[0],v[0])-1e-6<=p[0]<=max(u[0],v[0])+1e-6 and min(u[1],v[1])-1e-6<=p[1]<=max(u[1],v[1])+1e-6 and abs((v[0]-u[0])*(p[1]-u[1])-(v[1]-u[1])*(p[0]-u[0]))<1e-6

def clip(u,v,b):
 if abs(u[0]-v[0])<1e-6:
  if u[0]<b[0] or u[0]>b[2]:return None
  lo=max(min(u[1],v[1]),b[1]);hi=min(max(u[1],v[1]),b[3])
  return ((u[0],lo),(u[0],hi)) if hi-lo>1e-6 else None
 if u[1]<b[1] or u[1]>b[3]:return None
 lo=max(min(u[0],v[0]),b[0]);hi=min(max(u[0],v[0]),b[2])
 return ((lo,u[1]),(hi,u[1])) if hi-lo>1e-6 else None

def generate(components):
 source=wired_template(components,write=False)
 comps={c['ref']:c for c in components};instances=children(source,'symbol')
 symbols={};pinmap={};pinids=defaultdict(list);outward={}
 # All real pins/rotations are taken from the symbol library, not hardcoded offsets.
 libs={s[1]:s for s in children(child(source,'lib_symbols'),'symbol')}
 import math
 for inst in instances:
  ref=next(p[2] for p in children(inst,'property') if p[1]=='Reference');symbols[ref]=inst
  if ref not in comps:continue
  x,y=xy(inst);rad=math.radians(float(child(inst,'at')[3]));c=comps[ref]
  for pin in walk(libs[child(inst,'lib_id')[1]],'pin'):
   no=child(pin,'number')[1];pa=child(pin,'at');px,py=map(float,pa[1:3])
   pos=(round(x+(px*math.cos(rad)-py*math.sin(rad))/G,6),round(y-(px*math.sin(rad)+py*math.cos(rad))/G,6))
   net=c['pins'][no]
   if net is not None:
    pinmap[pos]=net;pinids[net].append((ref,no,pos))
    phi=math.radians(float(pa[3])+float(child(inst,'at')[3]));outward[pos]=(round(-math.cos(phi)),round(math.sin(phi)))
 # Infer each wire's net from the validated connected flat circuit.
 raw=[]
 for wire in children(source,'wire'):
  pts=children(child(wire,'pts'),'xy');u=tuple(round(float(v)/G,6) for v in pts[0][1:3]);v=tuple(round(float(v)/G,6) for v in pts[1][1:3]);raw.append((u,v))
 adj=defaultdict(set)
 for u,v in raw:adj[u].add(v);adj[v].add(u)
 seeds=defaultdict(set)
 for p,net in pinmap.items():seeds[p].add(net)
 for lab in children(source,'global_label'):seeds[xy(lab)].add(lab[1])
 for ref,inst in symbols.items():
  if child(inst,'lib_id')[1]=='power:GND':seeds[xy(inst)].add('GND')
 left=set(adj);net_at={}
 while left:
  todo=[left.pop()];node_set=set(todo);names=set()
  while todo:
   p=todo.pop();names.update(seeds[p])
   for q in adj[p]-node_set:node_set.add(q);left.discard(q);todo.append(q)
  if len(names)!=1:raise ValueError(('Ambiguous wired component',names,node_set))
  for p in node_set:net_at[p]=next(iter(names))
 net_edges=[(net_at[u],u,v) for u,v in raw]
 for group,spec in SHEETS.items():
  bounds=spec['bounds'];refs={c['ref'] for c in components if c['group']==group}
  nets={net for ref in refs for net in comps[ref]['pins'].values() if net}
  selected=[];protected=set();labels=[]
  for ref,inst in symbols.items():
   actual=ref in comps
   if (actual and ref not in refs) or (not actual and not contains(xy(inst),bounds)):continue
   obj=copy.deepcopy(inst)
   # Real components keep their UUID. Virtual power markers are unique per sheet.
   if not actual:child(obj,'uuid')[1]=uid(group+'-'+child(obj,'uuid')[1])
   for path in walk(child(obj,'instances'),'path'):path[1]='/'+uid('root')+'/'+uid('sheet-'+group)
   selected.append(obj)
   if actual:
    protected.update(pos for net,rows in pinids.items() for rr,no,pos in rows if rr==ref)
   else:protected.add(xy(inst))
  edges=[]
  for net,u,v in net_edges:
   if net not in nets:continue
   result=clip(u,v,bounds)
   if result:edges.append((net,*result))
  def touches(p,net=None):return any((net is None or nn==net) and on(p,u,v) for nn,u,v in edges)
  for lab in children(source,'global_label'):
   p=xy(lab)
   if lab[1] in nets and contains(p,bounds) and touches(p,lab[1]):
    labels.append(copy.deepcopy(lab));protected.add(p)
  # Existing junctions are explicit in the template. Its crossings without dots
  # stay crossings; wire endpoints already carry every valid T connection.
  # Remove dead cross-page tails but keep every physical pin, rail marker and label.
  while True:
   degree=defaultdict(int)
   for net,u,v in edges:degree[(net,u)]+=1;degree[(net,v)]+=1
   trimmed=[e for e in edges if not any(degree[(e[0],p)]==1 and p not in protected for p in e[1:])]
   if len(trimmed)==len(edges):break
   edges=trimmed
  # Label each electrically isolated in-sheet conductor once. This is usually an
  # inter-sheet rail/control port; all local passives stay visibly wired together.
  bynet=defaultdict(list)
  for net,u,v in edges:bynet[net].append((u,v))
  for net in sorted(nets):
   graph=defaultdict(set)
   for u,v in bynet[net]:graph[u].add(v);graph[v].add(u)
   # Include single physical pins where the only connection left the sheet.
   own_pins={pos for ref,no,pos in pinids[net] if ref in refs}
   remaining=set(graph)|own_pins
   while remaining:
    todo=[remaining.pop()];cluster=set(todo)
    while todo:
     for q in graph[todo.pop()]-cluster:cluster.add(q);remaining.discard(q);todo.append(q)
    existing=[lab for lab in labels if lab[1]==net and xy(lab) in cluster]
    ground=(net=='GND' and any(child(obj,'lib_id')[1]=='power:GND' and xy(obj) in cluster for obj in selected if obj[0]=='symbol'))
    if existing or ground:continue
    # Choose a free edge toward the sheet boundary, keeping names off conductors.
    candidates=sorted(cluster,key=lambda p:(p not in own_pins,p[0],p[1]))
    p=candidates[0]
    # Follow the pin outwards, never run a port label back through the IC body.
    dx,dy=outward.get(p,(-1,0));q=(p[0]+6*dx,p[1]+6*dy)
    if not any(nn==net and on(p,u,v) and on(q,u,v) for nn,u,v in edges):edges.append((net,p,q))
    angle=180 if dx<0 else 0
    lab=n('global_label',net,n('shape',a('input')),n('at',a(q[0]*G),a(q[1]*G),a(angle)),effects(1.05,'right' if angle==180 else 'left'),n('uuid',uid('logical-port-'+group+net+str(p))))
    labels.append(lab);protected.add(q)
  selected.extend(labels)
  # Rebuild junctions and wires after trimming. UUIDs are deterministic per sheet.
  degree=defaultdict(int)
  for i,(net,u,v) in enumerate(edges):
   selected.append(n('wire',n('pts',n('xy',a(u[0]*G),a(u[1]*G)),n('xy',a(v[0]*G),a(v[1]*G))),n('stroke',n('width',a(.254)),n('type',a('default'))),n('uuid',uid('logical-wire-'+group+str(i)))))
   for p in [u,v]:degree[(net,p)]+=1
  for (net,p),d in degree.items():
   if d>=3:selected.append(n('junction',n('at',a(p[0]*G),a(p[1]*G)),n('diameter',a(.8)),n('color',a(0),a(0),a(0),a(0)),n('uuid',uid('logical-junction-'+group+str(p)))))
  # NC crosses belong only to the components actually placed on this page.
  owned_nc={xy(nc) for nc in children(source,'no_connect') if contains(xy(nc),bounds)}
  for nc in children(source,'no_connect'):
   if xy(nc) in owned_nc:selected.append(copy.deepcopy(nc))
  for txt in children(source,'text'):
   if contains(xy(txt),bounds):selected.append(copy.deepcopy(txt))
  dx=spec['margin'][0]-bounds[0]*G;dy=spec['margin'][1]-bounds[1]*G
  def translate(obj):
   for part in obj:
    if not isinstance(part,list):continue
    if part and part[0] in ['at','xy'] and len(part)>=3:
     part[1]=a(round(float(part[1])+dx,6));part[2]=a(round(float(part[2])+dy,6))
    translate(part)
  for obj in selected:translate(obj)
  doc=[a('kicad_sch'),n('version',a(20250114)),n('generator',a('eeschema')),n('uuid',uid(group)),n('paper',*spec['paper']),
       n('title_block',n('title',spec['title']),n('rev','1.1'),n('comment',a(1),'Complete wired logical block. Only named rails/control nets cross sheets.')),
       copy.deepcopy(child(source,'lib_symbols')),*selected]
  (ROOT/(group+'.kicad_sch')).write_text(dump(doc)+'\n',encoding='utf-8')
  print(group,':',len(refs),'parts,',len(edges),'wire segments')
 root=[a('kicad_sch'),n('version',a(20250114)),n('generator',a('eeschema')),n('uuid',uid('root')),n('paper','A4'),n('lib_symbols'),
       n('title_block',n('title','2S flashlight - functional circuit blocks'),n('rev','1.1'))]
 for i,(group,spec) in enumerate(SHEETS.items()):
  y=35+i*50
  root.append(n('sheet',n('at',a(35),a(y)),n('size',a(225),a(28)),n('stroke',n('width',a(.1524)),n('type',a('default'))),n('fill',n('color',a(0),a(0),a(0),a(0))),n('uuid',uid('sheet-'+group)),
                n('property','Sheetname',spec['name'],n('at',a(35),a(y-2),a(0)),effects(1.5,'left')),
                n('property','Sheetfile',group+'.kicad_sch',n('at',a(35),a(y+30),a(0)),effects(1.27,'left')),
                n('instances',n('project','flashlight',n('path','/'+uid('root'),n('page',spec['page']))))))
  desc={'sense':'J1 battery -> R1 shunt -> Q1 -> J2 lamp; INA226 and Kelvin input filter.',
        'power':'SYS_PLUS 6..8.4 V -> MCP16331 5 V -> TPS7A02 3.3 V. Both rails generated here.',
        'controller':'ESP32 with wired reset/BOOT/USER, UART, NTC, LED, external contact and active buzzer.'}[group]
  root.append(n('text',desc,n('at',a(42),a(y+12),a(0)),effects(1.27,'left'),n('uuid',uid('logical-index-'+group))))
 root.append(n('text','Only external supply: the 2S battery on J1. No external 5 V input.',n('at',a(35),a(193),a(0)),effects(1.27,'left'),n('uuid',uid('logical-index-supply'))))
 root.append(n('sheet_instances',n('path','/',n('page','1'))))
 (ROOT/'flashlight.kicad_sch').write_text(dump(root)+'\n',encoding='utf-8')
