from inspect_sources import *

for lib, names in {
    'Transistor_FET':['AO3401A','2N7002'],
    'Regulator_Linear':['AMS1117-3.3','TLV1117LV33','TLV75533PDRV'],
    'Regulator_Switching':['MCP16331x-xCH'],
    'Device':['Q_PMOS_GSD','Q_NMOS_GSD'],
}.items():
    doc=parse((KICAD/'symbols'/f'{lib}.kicad_sym').read_text(encoding='utf-8'))
    for s in children(doc,'symbol'):
        if s[1] in names:
            print(lib,s[1], 'extends',child(s,'extends'), 'pins',[(child(p,'number')[1],child(p,'name')[1]) for p in walk(s,'pin')])
for fp in ['RF_Module:ESP32-C3-WROOM-02','Package_TO_SOT_SMD:SOT-23-6','Package_TO_SOT_SMD:SOT-223-3_TabPin2','Package_SO:VSSOP-10_3x3mm_P0.5mm','Button_Switch_SMD:SW_SPST_TL3342','Inductor_SMD:L_Bourns_SRN6045TA','Resistor_SMD:R_2512_6332Metric']:
    lib,name=fp.split(':')
    p=KICAD/'footprints'/f'{lib}.pretty'/f'{name}.kicad_mod'
    if p.exists():
        d=parse(p.read_text(encoding='utf-8'))
        print(fp,[(n[1],child(n,'at')[1:],child(n,'size')[1:]) for n in children(d,'pad')])
        if lib=='RF_Module':
            print('keepout',children(d,'zone'))
    else:
        print('MISSING',fp)
