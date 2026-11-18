"""Shared wired-circuit placement template. Coordinates are on a 1.27 mm grid.
The PCB component UUIDs and electrical pin/net specification are preserved.
"""
from build_project import ROOT, n, a, uid, symbol, effects, datasheet_for, child, children, walk, dump
import copy
import math

G=1.27
# x, y, rotation. The connector and switch symbols are rotated deliberately.
PLACEMENT={
 'J1':(25,28,90),'R1':(60,40,90),'Q1':(105,38,270),'J2':(145,40,0),
 'R2':(97,20,90),'R3':(122,50,0),'Q2':(120,65,0),'R4':(105,76,0),'C1':(80,60,0),
 'R5':(54,85,0),'R6':(66,100,0),'C2':(60,135,0),'U4':(100,150,0),'C3':(125,126,0),
 'U2':(220,50,0),'C4':(170,60,0),'C5':(185,60,0),'C6':(200,60,0),
 'D1':(246,61,270),'L1':(266,48,90),'C7':(240,35,90),'D2':(235,25,0),
 'C8':(288,60,0),'C9':(306,60,0),'R7':(320,58,0),'R8':(320,73,0),
 'U3':(400,50,0),'C10':(380,60,0),'C11':(420,60,0),
 'U1':(300,190,0),'C12':(314,167,0),'C13':(334,167,0),
 'R9':(266,142,0),'C14':(278,163,0),'SW3':(250,164,270),
 'R10':(262,209,0),'SW1':(250,220,270),'R11':(280,221,0),'R12':(276,179,0),
 'R13':(200,175,0),'R14':(215,175,0),'R15':(238,167,0),'C15':(246,179,0),'SW2':(230,180,270),
 'J3':(425,165,0),'R17':(390,152,0),'R18':(373,165,270),'C16':(360,174,0),
 'J4':(365,178,0),'R16':(245,245,90),'D3':(260,245,180),'J5':(320,290,0),
 'BZ1':(404,252,0),'Q3':(400,275,0),'R19':(385,275,90),'R20':(392,284,0),'D4':(418,253,270),
}

def generate(components,write=True):
    comps={c['ref']:c for c in components}
    if set(comps)!=set(PLACEMENT):raise ValueError(('Missing schematic positions',set(comps)^set(PLACEMENT)))
    libs={c['lib']:symbol(c['lib']) for c in components}
    for lib in ['power:GND','power:PWR_FLAG']:libs[lib]=symbol(lib)
    doc=[a('kicad_sch'),n('version',a(20250114)),n('generator',a('eeschema')),n('uuid',uid('root')),
         n('paper','A2'),n('title_block',n('title','2S FLASHLIGHT - COMPLETE CIRCUIT / SINGLE SHEET'),n('rev','1.0'),
         n('comment',a(1),'BATTERY 6.0-8.4 V -> MCP16331 5 V -> TPS7A02 3.3 V. No external 5 V supply.')),
         n('lib_symbols',*[copy.deepcopy(v) for v in libs.values()])]
    pins={};pin_nets={};edges=[];labels={};grounds=[];serial=0
    def point(p):return (round(p[0],6),round(p[1],6))
    def xy(p):return [a(round(p[0]*G,6)),a(round(p[1]*G,6))]
    def add(kind,*args):
        nonlocal serial
        serial+=1;doc.append(n(kind,*args,n('uuid',uid('flat-'+kind+str(serial)))))
    def text(txt,x,y,size=1.2):add('text',txt,n('at',*xy((x,y)),a(0)),effects(size,'left'))
    def wire(net,*pts):
        pts=[point(p) for p in pts]
        for p,q in zip(pts,pts[1:]):
            if p==q:continue
            if abs(p[0]-q[0])>1e-6 and abs(p[1]-q[1])>1e-6:raise ValueError(('Nonorthogonal',net,p,q))
            edges.append((net,p,q))
    def pin(ref,no):return pins[(ref,str(no))]
    def w(net,ref,no,*pts):
        if comps[ref]['pins'][str(no)]!=net:raise ValueError(('Wrong net',ref,no,net))
        wire(net,pin(ref,no),*pts)
    def label(net,p,angle=0):
        # One name per connected signal network, never a label on every IC pin.
        labels.setdefault(net,(point(p),angle))
    def power(net,p):label(net,p)
    def gnd(p,angle=0):grounds.append((point(p),angle))
    def ground(ref,no,p=None):
        pp=pin(ref,no)
        if p is not None:w('GND',ref,no,p);pp=p
        gnd(pp)
    def instance(co):
        ref=co['ref'];x,y,angle=PLACEMENT[ref];rad=math.radians(angle)
        def tr(px,py):return point((x+(px*math.cos(rad)-py*math.sin(rad))/G,y-(px*math.sin(rad)+py*math.cos(rad))/G))
        inst=n('symbol',n('lib_id',co['lib']),n('at',*xy((x,y)),a(angle)),n('unit',a(1)),n('in_bom',a('yes')),n('on_board',a('yes')),n('dnp',a('no')),n('uuid',co['uuid']))
        # Ref/value labels are horizontal and kept outside symbol bodies.
        if ref in ['U2','U3']:rp=(x,y-12);vp=(x,y-9);justify=None
        elif ref=='U1':rp=(x+17,y-9);vp=(x+17,y-6);justify='left'
        elif ref=='U4':rp=(x+6,y-15);vp=(x+6,y-12);justify='left'
        elif ref.startswith(('R','C','L','D')) and angle in [90,270,180]:rp=(x,y-6);vp=(x,y-3.5);justify=None
        else:rp=(x+5,y-3);vp=(x+5,y);justify='left'
        if ref=='J1':rp=(x,y-11);vp=(x,y-8);justify=None
        if ref=='J2':rp=(x-10,y+13);vp=(x-10,y+16)
        if ref=='Q2':rp=(x+6,y+7);vp=(x+6,y+10)
        if ref=='C14':rp=(x+2,y-1);vp=(x+2,y+1)
        if ref=='C15':rp=(x+2,y-1);vp=(x+2,y+1)
        if ref=='R11':rp=(x-6,y-1);vp=(x-6,y+1)
        if ref=='R12':rp=(x-9,y-1);vp=(x-9,y+1)
        if ref=='SW1':rp=(x-6,y-1);vp=(x-6,y+1)
        if ref=='BZ1':rp=(x+1,y+12);vp=(x+1,y+15)
        if ref=='D4':rp=(x+5,y+4);vp=(x+5,y+6)
        vals=[('Reference',ref,rp,1.35),('Value',co['value'],vp,1.15),('Footprint',co['footprint'],(x,y),.9),('Datasheet',datasheet_for(co,next((p[2] for p in children(libs[co['lib']],'property') if p[1]=='Datasheet'),'')),(x,y),.9)]
        for i,(key,val,pos,size) in enumerate(vals):
            prop=n('property',key,val,n('at',*xy(pos),a(90 if angle in [90,270] else 0)),effects(size,justify))
            if i>=2:child(prop,'effects').append(n('hide',a('yes')))
            inst.append(prop)
        inst.append(n('instances',n('project','flashlight',n('path','/'+uid('root'),n('reference',ref),n('unit',a(1))))))
        doc.append(inst)
        seen=set()
        for p in walk(libs[co['lib']],'pin'):
            no=child(p,'number')[1];pa=child(p,'at');pos=tr(float(pa[1]),float(pa[2]));pins[(ref,no)]=pos
            net=co['pins'][no]
            if net is None:add('no_connect',n('at',*xy(pos)))
            elif pos not in seen:pin_nets.setdefault(pos,set()).add(net)
            seen.add(pos)
    for co in components:instance(co)
    # 1. Battery entrance, shunt and high-side lamp switch.
    text('BATTERY 2S / 6.0-8.4 V',15,12,1.7)
    text('LAMP SUPPLY IS THE BATTERY VOLTAGE',83,12,1.4)
    text('SHUNT 47 mOhm / 1 W',46,31,1.1)
    w('PACK_PLUS','J1',1,(25,40),pin('R1',1));label('PACK_PLUS',(40,40))
    w('GND','J1',2,(27,36));gnd((27,36))
    w('SYS_PLUS','R1',2,pin('Q1',2));w('SYS_PLUS','U2',5,(165,46),(165,15),(85,15),(85,40));label('SYS_PLUS',(85,40))
    w('SYS_PLUS','Q1',2,(85,40));w('SYS_PLUS','C1',1,(80,40));ground('C1',2,(80,72))
    w('LAMP_PLUS','Q1',3,pin('J2',1));label('LAMP_PLUS',(128,40));ground('J2',2,(141,48))
    w('SYS_PLUS','R2',1,(90,20),(90,40))
    w('LOAD_GATE','R2',2,(105,20),pin('Q1',1));wire('LOAD_GATE',(105,20),(122,20),pin('R3',1));label('LOAD_GATE',(110,20))
    w('GATE_SINK','R3',2,pin('Q2',3));label('GATE_SINK',(122,57))
    ground('Q2',2,(122,80))
    w('LOAD_EN','Q2',1,(105,65),pin('R4',1));ground('R4',2,(105,84))
    # 2. Battery buck converter. 5 V exists only at the inductor output.
    text('ON-BOARD BUCK: BATTERY -> 5 V',175,12,1.7)
    w('SYS_PLUS','U2',4,(208,50),(208,46),(190,46))
    wire('SYS_PLUS',(180,46),(208,46))
    for ref in ['C4','C5','C6']:w('SYS_PLUS',ref,1,(PLACEMENT[ref][0],46));ground(ref,2,(PLACEMENT[ref][0],70))
    ground('U2',2,(220,70))
    w('SW','U2',6,pin('L1',1));w('SW','D1',1,(246,48));ground('D1',2,(246,70));label('SW',(252,48))
    w('BOOST','U2',1,(232,46),(232,35),pin('C7',1))
    w('SW','C7',2,(250,35),(250,48))
    w('BOOST','D2',1,(232,35));label('BOOST',(232,31))
    w('5V','D2',2,(280,25),(280,48))
    w('5V','L1',2,pin('U3',1));label('5V',(340,48))
    for ref in ['C8','C9']:w('5V',ref,1,(PLACEMENT[ref][0],48));ground(ref,2,(PLACEMENT[ref][0],70))
    w('5V','R7',1,(320,48));w('FB','R7',2,pin('R8',1))
    w('FB','U2',3,(236,52),(236,88),(325,88),(325,65),(320,65));label('FB',(320,65));ground('R8',2,(320,82))
    text('5V = OUTPUT OF U2 + L1, NOT AN EXTERNAL INPUT',260,96,1.25)
    text('R7/R8 = 523k/100k -> 4.984 V nominal',260,101,1.1)
    # 3. LDO and one visible 3V3 distribution rail.
    text('ON-BOARD LDO: 5 V -> 3.3 V',365,12,1.7)
    w('5V','U3',3,(390,50),(390,48));ground('U3',2,(400,70))
    w('5V','C10',1,(380,48));ground('C10',2,(380,70))
    w('3V3','U3',5,(440,48),(440,244),(402,244),pin('BZ1',1));label('3V3',(432,48))
    w('3V3','C11',1,(420,48));ground('C11',2,(420,70))
    wire('3V3',(440,110),(100,110),(100,140))
    w('3V3','C3',1,(125,118),(100,118));ground('C3',2,(125,134))
    # 4. Kelvin pickups and INA226. The two 10 ohm resistors feed a differential filter.
    text('INA226 / CURRENT AND BATTERY VOLTAGE',80,102,1.6)
    w('PACK_PLUS','R5',1,(54,40));w('SYS_PLUS','R6',1,(66,40))
    w('PACK_PLUS','U4',8,(44,144),(44,40))
    w('SENSE_PLUS','R5',2,(54,130),(80,130),(80,152),pin('U4',10));label('SENSE_PLUS',(70,130))
    w('SENSE_MINUS','R6',2,(66,140),(75,140),(75,154),pin('U4',9));label('SENSE_MINUS',(68,140))
    w('SENSE_PLUS','C2',1,(60,130),(54,130));w('SENSE_MINUS','C2',2,(60,140),(66,140))
    ground('U4',7,(100,168))
    w('GND','U4',1,(110,144),(110,146),pin('U4',2));wire('GND',(110,146),(120,146),(120,140));gnd((120,140),180)
    text('Address 0x40; 47 mOhm shunt; range +/-1.743 A',22,174,1.1)
    # 5. MCU and local power-on/reset circuit.
    text('ESP32-C3 / MCU',322,212,1.5)
    w('3V3','U1',1,(300,110));wire('3V3',(300,160),(334,160))
    for ref in ['C12','C13']:w('3V3',ref,1,(PLACEMENT[ref][0],160));ground(ref,2,(PLACEMENT[ref][0],174))
    ground('U1',9,(300,216))
    w('3V3','R9',1,(266,110))
    w('ESP_EN','R9',2,(266,160));wire('ESP_EN',(250,160),(284,160),(284,176),pin('U1',2))
    w('ESP_EN','C14',1,(250,160));ground('C14',2,(278,170))
    w('ESP_EN','SW3',1,(266,160));ground('SW3',2,(250,172));label('ESP_EN',(270,160))
    text('EN: R9/C14 power-on delay; SW3 = manual reset',18,279,1.1)
    # I2C is wired across the page, not split into port stubs.
    w('SDA','U4',4,(160,150),(160,188),pin('U1',3));label('SDA',(170,188))
    w('SCL','U4',5,(168,152),(168,190),pin('U1',4));label('SCL',(178,190))
    wire('3V3',(200,110),(200,165),(215,165))
    for ref in ['R13','R14']:w('3V3',ref,1,(PLACEMENT[ref][0],165))
    w('SDA','R13',2,(200,188));w('SCL','R14',2,(215,190))
    # USER and strapping pull-ups. Only rail symbols are repeated locally.
    w('USER_KEY','SW2',1,(272,176),(272,186),pin('U1',15));ground('SW2',2,(230,184))
    w('USER_KEY','C15',1,(230,176));ground('C15',2,(246,184))
    w('3V3','R15',1,(238,150));power('3V3',(238,150))
    w('USER_KEY','R15',2,(238,176));label('USER_KEY',(240,176))
    w('3V3','R12',1,(276,174));power('3V3',(276,174));w('STRAP2','R12',2,(276,184),pin('U1',16));label('STRAP2',(278,184))
    w('3V3','R11',1,(280,214));power('3V3',(280,214));w('STRAP8','R11',2,(280,230),(286,230),(286,196),pin('U1',7));label('STRAP8',(280,230))
    w('BOOT','SW1',1,(274,216),(274,198),pin('U1',8));ground('SW1',2,(250,228))
    w('3V3','R10',1,(262,204));power('3V3',(262,204));w('BOOT','R10',2,(262,216));label('BOOT',(256,216))
    text('FLASH: hold BOOT, press/release RESET, release BOOT',305,225,1.1)
    # UART: MCU TX goes to adapter RX. There is no programming power pad.
    w('UART_TX','U1',12,pin('J4',2));label('UART_TX',(344,178))
    w('UART_RX','U1',11,(350,176),(350,180),pin('J4',3));label('UART_RX',(344,176))
    w('GND','J4',1,(358,176),(358,172));gnd((358,172),180)
    text('UART: GND / TX / RX; 3.3 V logic',340,190,1.1)
    # External NTC and ADC RC filter.
    text('EXTERNAL NTC / ADC',355,128,1.7)
    w('TEMP_EXCITE','U1',17,(340,182),(340,145),(390,145),pin('R17',1));label('TEMP_EXCITE',(365,145))
    w('TEMP_RAW','R17',2,(390,165),pin('J3',1));label('TEMP_RAW',(395,165));ground('J3',2,(421,174))
    w('TEMP_RAW','R18',1,(390,165));w('TEMP_ADC','R18',2,(345,165),(345,180),pin('U1',18));label('TEMP_ADC',(352,165))
    w('TEMP_ADC','C16',1,(360,165));ground('C16',2,(360,184))
    text('Provisional NTC 10k B3950; GPIO1 powers divider only while sampling',350,200,1.0)
    # Indicator LED and the external contact sensor.
    text('STATUS LED / EXTERNAL CONTACT',220,252,1.5)
    w('LED_GPIO','U1',5,(232,192),(232,245),pin('R16',1));label('LED_GPIO',(232,240))
    w('LED_A','R16',2,pin('D3',2));label('LED_A',(252,245));ground('D3',1,(270,245))
    w('GYRO_SIGNAL','U1',6,(226,194),(226,250),(310,250),(310,290),pin('J5',1));label('GYRO_SIGNAL',(286,250));ground('J5',2,(316,298))
    text('J5: GPIO7 / contact to GND; firmware pull-up required',265,307,1.0)
    # Visible control wire back to the load switch.
    w('LOAD_EN','U1',10,(184,200),(184,65),pin('Q2',1));label('LOAD_EN',(150,65))
    # Active magnetic buzzer and diode clamp.
    text('ACTIVE BUZZER / 3.3 V',365,230,1.7)
    w('BUZZER_EN','U1',13,(190,202),(190,258),(370,258),(370,275),pin('R19',1));label('BUZZER_EN',(371,275))
    w('BUZZER_GATE','R19',2,pin('Q3',1));w('BUZZER_GATE','R20',1,(392,275));label('BUZZER_GATE',(392,275));ground('R20',2,(392,288));ground('Q3',2,(402,288))
    w('BUZZER_MINUS','BZ1',2,pin('Q3',3));label('BUZZER_MINUS',(402,263))
    w('3V3','D4',1,(418,244),(402,244));w('BUZZER_MINUS','D4',2,(418,260),(402,260))
    text('GPIO18 HIGH = SOUND',365,235,1.1)
    text('D4: K -> 3V3, A -> Q3 drain',365,239,1.1)
    # Supply labels repeated as rail markers only where routing one more rail is clutter.
    for ref in ['R10','R11','R12','R15']:
        pp=pin(ref,1);p=(pp[0],{'R10':204,'R11':214,'R12':174,'R15':150}[ref])
        add('global_label','3V3',n('shape',a('input')),n('at',*xy(p),a(0)),effects(.95,'left'))
    # ERC flags are markers, not external supply sources.
    for idx,(net,p) in enumerate([('SYS_PLUS',(85,40)),('5V',(280,48)),('GND',(27,36))],1):
        ref='#FLG0'+str(idx);co={'ref':ref,'lib':'power:PWR_FLAG','value':'PWR_FLAG','pins':{'1':net},'footprint':'','uuid':uid(ref)}
        inst=n('symbol',n('lib_id','power:PWR_FLAG'),n('at',*xy(p),a(0)),n('unit',a(1)),n('in_bom',a('no')),n('on_board',a('yes')),n('uuid',co['uuid']))
        for key,val in [('Reference',ref),('Value','PWR_FLAG')]:
            prop=n('property',key,val,n('at',*xy(p),a(0)),effects(.8));child(prop,'effects').append(n('hide',a('yes')));inst.append(prop)
        inst.append(n('instances',n('project','flashlight',n('path','/'+uid('root'),n('reference',ref),n('unit',a(1))))));doc.append(inst)
    # Emit the ground symbols with real electrical pins.
    for i,(p,angle) in enumerate(grounds):
        ref='#PWR'+str(100+i);inst=n('symbol',n('lib_id','power:GND'),n('at',*xy(p),a(angle)),n('unit',a(1)),n('in_bom',a('no')),n('on_board',a('yes')),n('uuid',uid('flat-ground'+str(i))))
        prop=n('property','Reference',ref,n('at',*xy(p),a(0)),effects(.8));child(prop,'effects').append(n('hide',a('yes')));inst.append(prop)
        inst.append(n('property','Value','GND',n('at',*xy((p[0],p[1]+(3 if angle==0 else -3))),a(0)),effects(.9)))
        inst.append(n('instances',n('project','flashlight',n('path','/'+uid('root'),n('reference',ref),n('unit',a(1))))));doc.append(inst)
    # Net names are short branches above conductors, never text struck through by a wire.
    for net,(p,angle) in list(labels.items()):
        delta=5 if net in ['STRAP8','BOOT','UART_TX','BUZZER_GATE'] else -4
        q=(p[0],p[1]+delta)
        wire(net,p,q);labels[net]=(q,180 if net=='BUZZER_GATE' else 0)
    # Join only same-net intersections. Unlike crossings, every real T has a dot.
    def on(p,u,v):return min(u[0],v[0])-1e-6<=p[0]<=max(u[0],v[0])+1e-6 and min(u[1],v[1])-1e-6<=p[1]<=max(u[1],v[1])+1e-6 and abs((v[0]-u[0])*(p[1]-u[1])-(v[1]-u[1])*(p[0]-u[0]))<1e-6
    splits=[{u,v} for net,u,v in edges]
    for i,(net,u,v) in enumerate(edges):
        for j,(other,p,q) in enumerate(edges):
            if i==j:continue
            if net!=other:
                if any(on(z,p,q) for z in [u,v]) or any(on(z,u,v) for z in [p,q]):raise ValueError(('Different-net wire endpoint collision',net,other,u,v,p,q))
                continue
            for z in [p,q]:
                if on(z,u,v):splits[i].add(z)
            if (u[0]==v[0])!=(p[0]==q[0]):
                cross=(u[0],p[1]) if u[0]==v[0] else (p[0],u[1])
                if on(cross,u,v) and on(cross,p,q):splits[i].add(cross)
    unique=set();degree={}
    for (net,u,v),points in zip(edges,splits):
        ps=sorted(points,key=lambda p:(p[0]-u[0])**2+(p[1]-u[1])**2)
        for p,q in zip(ps,ps[1:]):
            key=(net,*sorted([p,q]))
            if key in unique:continue
            unique.add(key);add('wire',n('pts',n('xy',*xy(p)),n('xy',*xy(q))),n('stroke',n('width',a(.254)),n('type',a('default'))))
            for z in [p,q]:degree[(net,z)]=degree.get((net,z),0)+1
    for (net,p),d in degree.items():
        if d>=3:add('junction',n('at',*xy(p)),n('diameter',a(.8)),n('color',a(0),a(0),a(0),a(0)))
    for net,(p,angle) in labels.items():
        add('global_label',net,n('shape',a('input')),n('at',*xy(p),a(angle)),effects(1.0,'left' if angle==0 else 'right'))
    text('Wires crossing WITHOUT a dot are NOT connected. A dot marks a junction.',18,286,1.1)
    text('Only external supply: J1 BATTERY 2S. J2 is the switched battery output to the lamp.',18,293,1.1)
    text('5 V and 3.3 V are produced on this board. PWR_FLAG is an ERC marker, not another source.',18,300,1.1)
    text('Shunt 47 mOhm: firmware calibration CAL=1089 at Current_LSB=100 uA. NTC type must match firmware.',18,307,1.0)
    doc.append(n('sheet_instances',n('path','/',n('page','1'))))
    if write:(ROOT/'flashlight.kicad_sch').write_text(dump(doc)+'\n',encoding='utf-8')
    return doc
