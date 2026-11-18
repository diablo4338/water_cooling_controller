"""Generate the flashlight KiCad design from an explicit pin/net specification.

Only the Python standard library is required. KiCad CLI performs ERC/DRC and
zone filling. Coordinates in the specification are a TOP view; components use both
faces in revision 0.4. Regenerate only inside this project directory.
"""
from inspect_sources import *
import copy
import csv
import math
import uuid

def uid(name):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, 'flashlight-controller-r1/' + name))

def a(v):
    if isinstance(v,float):
        return Atom(f'{v:.6f}'.rstrip('0').rstrip('.') or '0')
    return Atom(str(v))

def n(key,*args):
    return [Atom(key), *args]

def setnode(obj,key,*args):
    old=child(obj,key)
    if old is not None: obj.remove(old)
    obj.append(n(key,*args))

def num(node,key,index=1):
    return float(child(node,key)[index])

LIBCACHE={}
def symbol(lib_id):
    lib,name=lib_id.split(':')
    if lib not in LIBCACHE:
        LIBCACHE[lib]={s[1]:s for s in children(parse((KICAD/'symbols'/f'{lib}.kicad_sym').read_text(encoding='utf-8')),'symbol')}
    s=copy.deepcopy(LIBCACHE[lib][name])
    ex=child(s,'extends')
    if ex:
        parent=symbol(lib+':'+ex[1])
        props={p[1]:p for p in children(parent,'property')}
        props.update({p[1]:p for p in children(s,'property')})
        s=[x for x in parent if not (isinstance(x,list) and x[0]=='property')]
        s.extend(props.values())
    oldname=s[1].split(':')[-1]
    s[1]=lib_id
    for ss in children(s,'symbol'):
        # Library inheritance may use a different parent basename.
        suffix=re.search(r'(_\d+_\d+)$',ss[1]).group(1)
        ss[1]=name+suffix
    return s

RFP='Resistor_SMD:R_0805_2012Metric'
CFP='Capacitor_SMD:C_0805_2012Metric'
COMP=[]
def comp(ref,value,lib,fp,pins,xy,group,rot=0,note=''):
    COMP.append(dict(ref=ref,value=value,lib=lib,fp=fp,pins={str(k):v for k,v in pins.items()},xy=xy,group=group,rot=rot,note=note,uuid=uid(ref)))

def r(ref,value,p,q,xy,group,rot=0,note=''):
    comp(ref,value,'Device:R',RFP,{1:p,2:q},xy,group,rot,note)
def c(ref,value,p,q,xy,group,rot=0,fp=CFP,note=''):
    comp(ref,value,'Device:C',fp,{1:p,2:q},xy,group,rot,note)

def design():
    # Power entrance, current sense and switched load: direct copper path.
    comp('J1','PACK 2S / 6.0-8.4V','Connector_Generic:Conn_01x02','custom:WirePads_2x_5mm',{1:'PACK_PLUS',2:'GND'},(64,54),'sense')
    comp('J2','LAMP / 1A nominal','Connector_Generic:Conn_01x02','custom:WirePads_2x_5mm',{1:'LAMP_PLUS',2:'GND'},(64,37),'sense')
    comp('R1','0.020R 1% 1W','Device:R','Resistor_SMD:R_2512_6332Metric',{1:'PACK_PLUS',2:'SYS_PLUS'},(55,52),'sense',note='Kelvin pickups; INA measures total pack current')
    comp('Q1','AO3401A','Transistor_FET:AO3401A','Package_TO_SOT_SMD:SOT-23',{1:'LOAD_GATE',2:'SYS_PLUS',3:'LAMP_PLUS'},(56,39),'sense',note='P-channel high-side switch; OFF on reset')
    comp('Q2','2N7002','Transistor_FET:2N7002','Package_TO_SOT_SMD:SOT-23',{1:'LOAD_EN',2:'GND',3:'GATE_SINK'},(48,38),'sense')
    r('R2','47k','SYS_PLUS','LOAD_GATE',(57,44),'sense')
    r('R3','1k','LOAD_GATE','GATE_SINK',(51,41),'sense')
    r('R4','100k','LOAD_EN','GND',(45,35),'sense')
    comp('C1','470u 16V low-ESR','Device:C_Polarized','custom:Electrolytic_D10_P5',{1:'SYS_PLUS',2:'GND'},(66,45.5),'sense',note='Bulk buffer for lamp pulses; mounted on bottom')
    comp('U4','INA226AIDGSR','Sensor_Energy:INA226','Package_SO:MSOP-10_3x3mm_P0.5mm',{1:'GND',2:'GND',3:None,4:'SDA',5:'SCL',6:'3V3',7:'GND',8:'PACK_PLUS',9:'SENSE_MINUS',10:'SENSE_PLUS'},(44,51),'sense',note='I2C address 0x40; VBUS senses raw pack voltage')
    r('R5','10R','PACK_PLUS','SENSE_PLUS',(49,49),'sense')
    r('R6','10R','SYS_PLUS','SENSE_MINUS',(49,53),'sense')
    c('C2','100n','SENSE_PLUS','SENSE_MINUS',(43,47),'sense')
    c('C3','100n','3V3','GND',(40,55),'sense')
    # MCP16331: compact switching loop; feedback quiet and separate from SW.
    comp('U2','MCP16331T-E/CH','Regulator_Switching:MCP16331x-xCH','Package_TO_SOT_SMD:SOT-23-6',{1:'BOOST',2:'GND',3:'FB',4:'SYS_PLUS',5:'SYS_PLUS',6:'SW'},(28,46),'power')
    c('C4','10u 16V X7R','SYS_PLUS','GND',(24,48),'power',rot=90)
    c('C5','10u 16V X7R','SYS_PLUS','GND',(21,48),'power',rot=90)
    c('C6','100n 16V','SYS_PLUS','GND',(28,51),'power',rot=90)
    comp('D1','SS24','Device:D_Schottky','Diode_SMD:D_SMA',{1:'SW',2:'GND'},(22,41),'power',note='Catch diode: cathode to SW, anode to GND')
    comp('L1','22uH / Isat>=1.2A','Device:L','Inductor_SMD:L_Bourns_SRN6045TA',{1:'SW',2:'5V'},(21,34),'power',note='SRN6045TA-220M or electrically equivalent footprint-compatible part')
    c('C7','100n 16V','BOOST','SW',(28,42),'power')
    comp('D2','1N4148WS','Device:D','Diode_SMD:D_SOD-323',{1:'BOOST',2:'5V'},(32,41),'power',note='Bootstrap diode: cathode to BOOST, anode to 5V')
    c('C8','22u 10V X7R','5V','GND',(17,28),'power',rot=90,fp='Capacitor_SMD:C_1206_3216Metric')
    c('C9','22u 10V X7R','5V','GND',(21,28),'power',rot=90,fp='Capacitor_SMD:C_1206_3216Metric')
    r('R7','52.3k 1%','5V','FB',(32,47),'power')
    r('R8','10k 1%','FB','GND',(32,50),'power')
    comp('U3','TPS7A02-3.3','Regulator_Linear:TPS7A0508PDBV','Package_TO_SOT_SMD:SOT-23-5',{1:'5V',2:'GND',3:'5V',4:None,5:'3V3'},(14,20),'power',note='Reference power U2; TPS7A0233PDBVR; IN1 GND2 EN3 NC4 OUT5; rated 200mA')
    c('C10','1u 10V X7R','5V','GND',(17.5,21),'power',rot=90,note='Reference power C6; local LDO input bypass')
    c('C11','4.7u 10V X7R','3V3','GND',(10.5,21),'power',rot=90,note='Reference power C5; local LDO output bypass')
    # MCU and UI; no GPIO on the boot straps is assigned a load control role.
    comp('U1','ESP32-C3-WROOM-02-N4','RF_Module:ESP32-C3-WROOM-02','RF_Module:ESP32-C3-WROOM-02',{1:'3V3',2:'ESP_EN',3:'SDA',4:'SCL',5:'LED_GPIO',6:None,7:'STRAP8',8:'BOOT',9:'GND',10:'LOAD_EN',11:'UART_RX',12:'UART_TX',13:None,14:None,15:'USER_KEY',16:'STRAP2',17:None,18:'TEMP_ADC',19:'GND'},(21,7.25),'controller',note='PCB antenna fully above base-board FR4; all-layer copper keepout')
    c('C12','10u 10V X7R','3V3','GND',(33,2),'controller')
    c('C13','100n','3V3','GND',(33,5),'controller')
    r('R9','10k','3V3','ESP_EN',(34,9),'controller')
    c('C14','1u','ESP_EN','GND',(37,9),'controller',rot=90)
    r('R10','10k','3V3','BOOT',(34,17),'controller')
    r('R11','10k','3V3','STRAP8',(34,14),'controller')
    r('R12','10k','3V3','STRAP2',(8,7),'controller',rot=90)
    r('R13','4.7k','3V3','SDA',(38,13),'controller',rot=90)
    r('R14','4.7k','3V3','SCL',(41,13),'controller',rot=90)
    comp('SW1','BOOT','Switch:SW_Push','Button_Switch_SMD:SW_SPST_TL3342',{1:'BOOT',2:'GND'},(46,8),'controller')
    comp('SW2','USER / GPIO3','Switch:SW_Push','Button_Switch_SMD:SW_SPST_TL3342',{1:'USER_KEY',2:'GND'},(60,8),'controller')
    r('R15','10k','3V3','USER_KEY',(55,14),'controller')
    c('C15','100n','USER_KEY','GND',(59,14),'controller')
    comp('D3','LED GREEN','Device:LED','LED_SMD:LED_0805_2012Metric',{1:'GND',2:'LED_A'},(48,18),'controller')
    r('R16','1k','LED_GPIO','LED_A',(44,18),'controller')
    comp('J3','NTC SENSOR','Connector_Generic:Conn_01x02','custom:SensorPads_2',{1:'TEMP_RAW',2:'GND'},(6,31),'controller',note='External NTC to ground; provisional 10k/B3950, confirm actual sensor')
    r('R17','10k 1%','3V3','TEMP_RAW',(7,25),'controller',note='Match actual NTC nominal; ADC attenuation and calibration required')
    r('R18','1k','TEMP_RAW','TEMP_ADC',(5,22),'controller')
    c('C16','100n','TEMP_ADC','GND',(7,11),'controller',rot=90)
    comp('J4','UART 3V3 LOGIC','Connector_Generic:Conn_01x03','custom:UART_Pads_3',{1:'GND',2:'UART_TX',3:'UART_RX'},(39,25),'controller',note='Only GND, MCU TX and MCU RX; 3.3V logic; power board from pack; use BOOT and RESET buttons for download mode')

    # Revision 0.2: reuse the supplied speed_and_temp_v8 parts where suitable.
    updates={
        'R1': dict(value='WSL2512R0470FEA',note='Reference ina R17; 47mOhm 1% 1W; INA226 full scale 1.743A; update calibration'),
        'Q1': dict(value='MMFTP3334K',note='Reference fan_power Q1; verified G1 S2 D3; slow lamp on/off, not high-frequency PWM'),
        'Q2': dict(value='NXV40UNR',note='Reference fan_power Q2 / esp Q3; verified G1 S2 D3'),
        'R2': dict(value='61.9k',note='Reference 61k9; 11uA combined gate/sink leakage gives 0.681V off-state Vgs'),
        'D1': dict(value='PD3S120LQ',fp='custom:Reference_PowerDI323',note='Reference fan_power D2; 20V 1A; cathode SW, anode GND'),
        'D2': dict(value='1N4148WS-HG3_A-08',note='Reference fan_power D3; cathode BOOST, anode 5V'),
        'R7': dict(value='523k 1%',note='Feedback current 8uA; verify stability on assembled board'),
        'R8': dict(value='100k 1%',note='Reference 100k; practical design ceiling, no manufacturer maximum specified'),
        'SW1': dict(fp='custom:Reference_Button',xy=(48,7),note='Reference esp button footprint'),
        'SW2': dict(fp='custom:Reference_Button',note='Reference esp button footprint'),
        'R17': dict(value='12.4k 1%',pins={'1':'TEMP_EXCITE','2':'TEMP_RAW'},note='Provisional NTC10k B3950 0..100C; GPIO1 high during sampling only; settle 15ms'),
    }
    for co in COMP:
        co.update(updates.get(co['ref'],{}))
        if co['ref']=='U1':
            co['pins']['17']='TEMP_EXCITE'
            co['note']='GPIO0 ADC1_CH0, GPIO1 switched NTC excitation; antenna above FR4; all-layer copper keepout'

    # Compact revision: preserve package geometry, compress component spacing.
    for co in COMP:
        x,y=co['xy']; co['xy']=(round(x*.85,4),round(y*.85,4))
    next(co for co in COMP if co['ref']=='U1')['xy']=(17.85,7.25)
    next(co for co in COMP if co['ref']=='U1')['pins']['6']='GYRO_SIGNAL'
    offsets={'C12':(2.5,0),'C13':(2.5,0),'R9':(2.5,0),'R10':(2.5,0),'R11':(2.5,0),'C14':(3.2,0),
             'R13':(2.5,0),'R14':(3.15,1.75),'R16':(0,.8),'R15':(0,1.5),'C15':(0,1.5),
             'R4':(0,-1),'R3':(1,1),'C8':(0,-1.5),'C9':(0,-1.5),
             'R5':(0,-2),'R6':(-.5,3),'C11':(-.7,0),'C10':(.7,0),'R7':(1,0),
             'U4':(0,.05),'J1':(0,2.1),'J2':(0,-1.65)}
    for co in COMP:
        dx,dy=offsets.get(co['ref'],(0,0));x,y=co['xy'];co['xy']=(round(x+dx,4),round(y+dy,4))

    comp('J5','GYRO SIGNAL / GND','Connector_Generic:Conn_01x02','custom:GyroPads_2',
         {1:'GYRO_SIGNAL',2:'GND'},(47,20),'controller',note='GPIO7 input (module pad6), 3.3V logic; contact to GND requires firmware pull-up')

    # Revision 0.4: stacked blocks, analogue circuitry on the opposite face.
    buck={'U2','C4','C5','C6','D1','L1','C7','D2','C8','C9','R7','R8'}
    mcu={'U1','C12','C13','R9','C14','R10','R11','R12','C16'}
    positions={'J1':(30,40),'J2':(28,17),'R1':(21,41.5),'C1':(32,30),
       'Q1':(26,22),'Q2':(32,23),'R2':(29,26),'R3':(36,24),'R4':(32,20),
       'U4':(21,36),'R5':(27,32),'R6':(27,36),'C2':(20,32),'C3':(17.5,39),
       'U3':(10,21),'C10':(14,21),'C11':(6,21),
       'SW1':(8,12),'SW2':(34,6),'R15':(30,9),'C15':(34,9),
       'R13':(26.5,10),'R14':(25,13.8),'R10':(24,5),'R16':(36,34),'D3':(39.5,31),
       'J3':(3,28),'R17':(4,24),'R18':(3,20),'C8':(3.45,21),'C9':(6.85,21),'C6':(14,35),'J4':(18,17),'J5':(5,34)}
    front={'U4','R5','R6','C2','C3','U3','C10','C11','SW1','SW2','R15','C15',
           'C6','R13','R14','R10','J4','Q2','R2','R3','R4','R16','D3','R17','R18','J5'}
    for co in COMP:
        ref=co['ref'];x,y=co['xy']
        if ref in buck:co['xy']=(round(x-5,4),round(y-7,4))
        if ref in mcu:co['xy']=(round(x-4.35,4),y)
        if ref in positions:co['xy']=positions[ref]
        co['side']='F' if ref in front else 'B'

    # Active 3 V buzzer, low-side NMOS switch, OFF while the GPIO floats.
    next(co for co in COMP if co['ref']=='U1')['pins']['13']='BUZZER_EN'
    comp('BZ1','AI-1223-TWT-3V-2-R','Device:Buzzer','Buzzer_Beeper:Buzzer_12x9.5RM7.6',
         {1:'3V3',2:'BUZZER_MINUS'},(13.3,41.5),'controller',note='Active magnetic 3V indicator; 2..4V; <=30mA at 3V; diameter12, pitch7.6; GPIO18 high=ON')
    comp('Q3','NXV40UNR','Transistor_FET:2N7002','Package_TO_SOT_SMD:SOT-23',
         {1:'BUZZER_GATE',2:'GND',3:'BUZZER_MINUS'},(10,44),'controller',note='NXV40UNR G1 S2 D3; low-side buzzer driver')
    r('R19','100R','BUZZER_EN','BUZZER_GATE',(10,39),'controller',note='Series gate resistor; not a pull resistor')
    r('R20','100k','BUZZER_GATE','GND',(6,38.5),'controller',note='Gate-source pull-down; default OFF')
    comp('D4','1N4148WS-HG3_A-08','Device:D','Diode_SMD:D_SOD-323',
         {1:'3V3',2:'BUZZER_MINUS'},(15,45),'controller',note='Clamp diode, cathode to 3V3, anode to buzzer minus')
    for co in COMP:
        if co['ref'] in ['Q3','R19','R20','D4']:co['side']='F'
        if co['ref']=='BZ1':co['side']='B'

    comp('SW3','RESET','Switch:SW_Push','custom:Reference_Button',
         {1:'ESP_EN',2:'GND'},(21,12),'controller',
         note='Momentary reset: EN to GND; R9=10k pull-up to3V3 and C14=1u RC retained')
    COMP[-1]['side']='F'
    next(co for co in COMP if co['ref']=='R14')['rot']=0

def custom_footprints():
    out={}
    for name in ['Reference_PowerDI323','Reference_Button']:
        out[name]=parse((ROOT/'tools'/'reference_footprints'/f'{name}.kicad_mod').read_text(encoding='utf-8'))
    def base(name):
        return [a('footprint'),name,n('version',a(20260206)),n('generator',a('pcbnew')),n('layer','F.Cu'),
            n('property','Reference','REF**',n('at',a(0),a(-3),a(0)),n('layer','F.SilkS'),n('effects',n('font',n('size',a(1),a(1)),n('thickness',a(.15))))),
            n('property','Value',name,n('at',a(0),a(3),a(0)),n('layer','F.Fab'),n('effects',n('font',n('size',a(1),a(1)),n('thickness',a(.15)))))]
    def pad(no,x,y,w,h,drill=None):
        p=n('pad',str(no),a('thru_hole' if drill else 'smd'),a('oval' if drill else 'roundrect'),n('at',a(x),a(y)),n('size',a(w),a(h)))
        if drill:
            p.extend([n('drill',a(drill)),n('layers','*.Cu','*.Mask')])
        else:
            p.extend([n('layers','F.Cu','F.Mask'),n('roundrect_rratio',a(.15))])
        return p
    for name,pads,box in [
        ('WirePads_2x_5mm', [pad(1,0,0,5,5,1.6),pad(2,-7,0,5,5,1.6)],(-10,-3,3,3)),
        ('SensorPads_2',[pad(1,0,0,2.4,2.4,1),pad(2,-3.5,0,2.4,2.4,1)],(-5,-1.5,1.5,1.5)),
        ('Electrolytic_D10_P5',[pad(1,2.5,0,2.2,2.2,.8),pad(2,-2.5,0,2.2,2.2,.8)],(-5.3,-5.3,5.3,5.3)),
        ('GyroPads_2',[pad(1,0,0,2,2),pad(2,-3,0,2,2)],(-4.3,-1.3,1.3,1.3)),
        ('UART_Pads_3',[pad(i+1,i*2.54,0,1.6,2) for i in range(3)],(-1,-1.5,6.08,1.5))]:
        f=base(name)
        f+=pads
        f.append(n('attr',a('through_hole' if name not in ['UART_Pads_3','GyroPads_2'] else 'smd')))
        x1,y1,x2,y2=box
        for layer,width in [('F.CrtYd',.05),('F.Fab',.1)]:
            f.append(n('fp_rect',n('start',a(x1),a(y1)),n('end',a(x2),a(y2)),n('stroke',n('width',a(width)),n('type',a('default'))),n('fill',a('none')),n('layer',layer)))
        out[name]=f
    return out

def libraries():
    folder=ROOT/'Flashlight.pretty'
    folder.mkdir(exist_ok=True)
    custom=custom_footprints()
    fps={}
    for co in COMP:
        lib,name=co['fp'].split(':')
        if name not in fps:
            f=custom[name] if lib=='custom' else parse((KICAD/'footprints'/f'{lib}.pretty'/f'{name}.kicad_mod').read_text(encoding='utf-8'))
            f[1]=name
            if name=='ESP32-C3-WROOM-02':
                f[:]=[s for s in f if not (isinstance(s,list) and s[0] in ['fp_line','fp_poly','fp_rect'] and child(s,'layer',[None,''])[1]=='F.SilkS')]
                # No drilled holes in exposed solder pad: escape to off-pad vias.
                f[:]=[v for v in f if not (isinstance(v,list) and v[0]=='pad' and v[1]=='19' and v[2]=='thru_hole')]
            fps[name]=f
            (folder/f'{name}.kicad_mod').write_text(dump(f)+'\n',encoding='utf-8')
        co['footprint']='Flashlight:'+name
    (ROOT/'fp-lib-table').write_text('(fp_lib_table (version 7) (lib (name "Flashlight") (type "KiCad") (uri "${KIPRJMOD}/Flashlight.pretty") (options "") (descr "Self-contained project footprints")))\n',encoding='utf-8')
    return fps

def effects(size=1.0,justify=None):
    e=n('effects',n('font',n('size',a(size),a(size))))
    if justify: e.append(n('justify',a(justify)))
    return e

def datasheet_for(co,default=''):
    return {
        'Q1':'https://diotec.com/files/diotec/productfiles/datasheet/mmftp3334k.pdf',
        'Q2':'https://assets.nexperia.com/documents/data-sheet/NXV40UN.pdf',
        'Q3':'https://assets.nexperia.com/documents/data-sheet/NXV40UN.pdf',
        'BZ1':'https://puiaudio.com/file/specs-AI-1223-TWT-3V-2-R.pdf',
        'D1':'https://www.diodes.com/datasheet/download/PD3S120LQ.pdf',
        'U3':'https://www.ti.com/lit/ds/symlink/tps7a02.pdf',
    }.get(co['ref'],default)

def schematic():
    from logical_schematic import generate
    generate(COMP)


def transform_footprint(source,co,netids):
    f=copy.deepcopy(source)
    f[1]=co['footprint']
    f[:]=[v for v in f if not (isinstance(v,list) and v[0] in ['version','generator','generator_version','uuid'])]
    angle_global=180+co['rot']
    theta=math.radians(angle_global)
    def point(x,y):
        return (x,y) if co.get('side')=='F' else (x,-y)
    def rec(obj):
        for v in obj:
            if not isinstance(v,list): continue
            key=v[0]
            if key in ['at','start','end','mid','center','xy'] and len(v)>=3:
                x,y=point(float(v[1]),float(v[2])); v[1:3]=[a(x),a(y)]
                if key=='at':
                    angle=float(v[3]) if len(v)>3 else 0
                    if len(v)>3: v[3]=a(angle if co.get('side')=='F' else -angle)
                    else: v.append(a(-angle))
            elif key in ['layer','layers'] and co.get('side')!='F':
                for j in range(1,len(v)):
                    if v[j].startswith('F.'): v[j]='B.'+v[j][2:]
                    elif v[j].startswith('B.'): v[j]='F.'+v[j][2:]
            elif key=='uuid':
                v[1]=uid(co['ref']+v[1])
            rec(v)
    rec(f)
    setnode(f,'layer',co.get('side','B')+'.Cu')
    setnode(f,'at',*[a(v) for v in co['xy']],a(angle_global))
    setnode(f,'uuid',co['uuid'])
    setnode(f,'path','/'+uid('root')+'/'+uid('sheet-'+co['group'])+'/'+co['uuid'])
    datasheet=next((p[2] for p in children(symbol(co['lib']),'property') if p[1]=='Datasheet'),'')
    datasheet=datasheet_for(co,datasheet)
    if not any(p[1]=='Datasheet' for p in children(f,'property')):
        f.append(n('property','Datasheet',datasheet,n('at',a(0),a(0),a(0)),n('layer','B.Fab'),n('hide',a('yes')),effects()))
    for prop in children(f,'property'):
        if prop[1]=='Reference': prop[2]=co['ref']
        elif prop[1]=='Value': prop[2]=co['value']
        elif prop[1]=='Datasheet': prop[2]=datasheet
        elif prop[1]=='Description': prop[2]=''
        eff=child(prop,'effects')
        if eff:
            setnode(eff,'justify',*([a('mirror')] if co.get('side')!='F' else []))
        setnode(prop,'hide',a('yes'))
    for txt in children(f,'fp_text'):
        eff=child(txt,'effects')
        if eff:setnode(eff,'justify',*([a('mirror')] if co.get('side')!='F' else []))
    pads=[]
    for p in children(f,'pad'):
        number=p[1]
        if not number: continue
        if number not in co['pins']: raise ValueError((co['ref'],'footprint pin',number))
        net=co['pins'][number]
        if net is None:
            pinname=next(child(pin,'name')[1] for pin in walk(symbol(co['lib']),'pin') if child(pin,'number')[1]==number)
            net=f'unconnected-({co["ref"]}-{pinname}-Pad{number})'
        if net:
            setnode(p,'net',a(netids[net]),net)
            if net=='GND':
                setnode(p,'zone_connect',a(2))
        pos=child(p,'at')
        layers=child(p,'layers')[1:]
        # Skip paste-only pad subdivisions in module footprint.
        if not any(v in layers for v in ['B.Cu','F.Cu','*.Cu']): continue
        sx,sy=map(float,child(p,'size')[1:3]); angle=angle_global+(float(pos[3]) if len(pos)>3 else 0)
        if round(angle)%180: sx,sy=sy,sx
        lx,ly=map(float,pos[1:3]);wx=lx*math.cos(theta)+ly*math.sin(theta);wy=-lx*math.sin(theta)+ly*math.cos(theta)
        pads.append(dict(ref=co['ref'],pin=number,net=net,x=round(co['xy'][0]+wx,6),y=round(co['xy'][1]+wy,6),w=sx,h=sy,layers=[0,1] if '*.Cu' in layers else [0 if co.get('side')=='F' else 1],shape=p[3],drill=float(child(p,'drill',[None,0])[1])))
    return f,pads

def board(fps):
    nets=sorted({v for co in COMP for v in co['pins'].values() if v})
    for co in COMP:
        for pin in walk(symbol(co['lib']),'pin'):
            no=child(pin,'number')[1]
            if co['pins'][no] is None:nets.append(f'unconnected-({co["ref"]}-{child(pin,"name")[1]}-Pad{no})')
    ids={v:i+1 for i,v in enumerate(nets)}
    b=[a('kicad_pcb'),n('version',a(20241229)),n('generator',a('pcbnew')),n('general',n('thickness',a(1.6))),n('paper','A4'),n('layers',n('0','F.Cu',a('signal')),n('31','B.Cu',a('signal')),n('34','B.Paste',a('user')),n('35','F.Paste',a('user')),n('36','B.SilkS',a('user')),n('37','F.SilkS',a('user')),n('38','B.Mask',a('user')),n('39','F.Mask',a('user')),n('40','Dwgs.User',a('user'),'User.Drawings'),n('44','Edge.Cuts',a('user')),n('46','B.CrtYd',a('user')),n('47','F.CrtYd',a('user')),n('48','B.Fab',a('user')),n('49','F.Fab',a('user'))),n('setup',n('pad_to_mask_clearance',a(0)))]
    b.extend(n('net',a(v),k) for k,v in ids.items())
    allpads=[]
    for co in COMP:
        f,pads=transform_footprint(fps[co['fp'].split(':')[1]],co,ids)
        b.append(f); allpads+=pads
    for start,end in [((0,-8),(42,-8)),((42,-8),(42,48)),((42,48),(0,48)),((0,48),(0,-8))]:
        b.append(n('gr_line',n('start',*[a(v) for v in start]),n('end',*[a(v) for v in end]),n('stroke',n('width',a(.05)),n('type',a('default'))),n('layer','Edge.Cuts'),n('uuid',uid(str(start)))))
    for ref,xy in [('H1',(2,14)),('H2',(40,14)),('H3',(2,45)),('H4',(40,45))]:
        f=n('footprint','MountingHole_3.2mm',n('layer','B.Cu'),n('at',*[a(v) for v in xy]),n('uuid',uid(ref)),n('property','Reference',ref,n('at',a(0),a(0),a(0)),n('layer','B.Fab'),n('hide',a('yes')),n('effects',n('font',n('size',a(1),a(1))),n('justify',a('mirror')))),n('attr',a('board_only'),a('exclude_from_pos_files'),a('exclude_from_bom')),n('pad','',a('np_thru_hole'),a('circle'),n('at',a(0),a(0)),n('size',a(3.2),a(3.2)),n('drill',a(3.2)),n('layers','*.Cu','*.Mask')))
        b.append(f)
        allpads.append(dict(ref=ref,pin='',net=None,x=xy[0],y=xy[1],w=3.2,h=3.2,layers=[0,1],shape='circle',drill=3.2))
    # Labels are mirrored for reading from the populated bottom.
    (ROOT/'unrouted.kicad_pcb').write_text(dump(b)+'\n',encoding='utf-8')
    (ROOT/'tools'/'layout.json').write_text(json.dumps(dict(pads=allpads,nets=ids,components=COMP),indent=2),encoding='utf-8')

def project():
    p={'meta':{'filename':'flashlight.kicad_pro','version':1},'board':{'design_settings':{'rules':{'min_clearance':.15,'min_track_width':.15,'min_via_diameter':.6,'min_through_hole_diameter':.3,'min_copper_edge_clearance':.25},'rule_severities':{'lib_footprint_mismatch':'warning'},'track_widths':[.2,.3,.5,1,2,3], 'via_dimensions':[{'diameter':.6,'drill':.3}]}},'net_settings':{'classes':[{'name':'Default','clearance':.15,'track_width':.25,'via_diameter':.6,'via_drill':.3,'microvia_diameter':.3,'microvia_drill':.1,'diff_pair_width':.2,'diff_pair_gap':.25,'diff_pair_via_gap':.25}], 'meta':{'version':3}}}
    if not (ROOT/'flashlight.kicad_pro').exists():
        (ROOT/'flashlight.kicad_pro').write_text(json.dumps(p,indent=2)+'\n',encoding='utf-8')
    with (ROOT/'BOM.csv').open('w',newline='',encoding='utf-8-sig') as f:
        w=csv.writer(f); w.writerow(['Reference','Value','Footprint','Notes'])
        for co in COMP: w.writerow([co['ref'],co['value'],co['footprint'],co['note']])
    (ROOT/'tools'/'expected_nets.json').write_text(json.dumps({co['ref']:co['pins'] for co in COMP},indent=2),encoding='utf-8')

if __name__=='__main__':
    design()
    fps=libraries()
    schematic()
    board(fps)
    project()
    print(f'Generated {len(COMP)} components in {ROOT}')
