"""Copy selected user reference footprints, removing placement and net identity."""
from inspect_sources import *

def main():
    source=Path('U:/plate_schemas/speed_and_temp_v8')
    board=parse((source/'main.kicad_pcb').read_text(encoding='utf-8'))
    dest=ROOT/'tools'/'reference_footprints'
    dest.mkdir(exist_ok=True)
    for ref,name in [('D2','Reference_PowerDI323'),('SW1','Reference_Button')]:
        fp=next(f for f in children(board,'footprint') if any(p[1]=='Reference' and p[2]==ref for p in children(f,'property')))
        assert child(fp,'layer')[1]=='F.Cu'
        placement=child(fp,'at')
        angle=float(placement[3]) if len(placement)>3 else 0
        fp[:]=[x for x in fp if not (isinstance(x,list) and x[0] in ['at','path','sheetname','sheetfile','uuid','model'])]
        fp[1]=name
        for pad in children(fp,'pad'):
            pad[:]=[x for x in pad if not (isinstance(x,list) and x[0] in ['net','pinfunction','pintype','uuid'])]
            at=child(pad,'at')
            if len(at)>3: at[3]=Atom(str((float(at[3])-angle)%360))
        for prop in children(fp,'property'):
            if prop[1]=='Reference':prop[2]='REF**'
            if prop[1]=='Value':prop[2]=name
        (dest/f'{name}.kicad_mod').write_text(dump(fp)+'\n',encoding='utf-8')
    rows=[]
    for path in sorted(source.glob('*.kicad_sch')):
        doc=parse(path.read_text(encoding='utf-8'))
        for sym in children(doc,'symbol'):
            props={p[1]:p[2] for p in children(sym,'property')}
            rows.append(dict(sheet=path.name,reference=props.get('Reference'),value=props.get('Value'),footprint=props.get('Footprint')))
    (ROOT/'tools'/'reference_parts.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
