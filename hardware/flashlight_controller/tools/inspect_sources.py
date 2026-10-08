"""Read KiCad source symbols/footprints without third party dependencies."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KICAD = Path('C:/Program Files/KiCad/10.0/share/kicad')

class Atom(str):
    pass

def parse(text):
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+', text)
    stack = []
    root = None
    for t in tokens:
        if t == '(':
            node = []
            if stack:
                stack[-1].append(node)
            stack.append(node)
        elif t == ')':
            root = stack.pop()
        else:
            stack[-1].append(json.loads(t) if t.startswith('"') else Atom(t))
    return root

def children(node, key):
    return [n for n in node if isinstance(n, list) and n and n[0] == key]

def child(node, key, default=None):
    return next(iter(children(node, key)), default)

def walk(node, key):
    for n in node:
        if isinstance(n, list):
            if n and n[0] == key:
                yield n
            yield from walk(n, key)

def dump(node, depth=0):
    if not isinstance(node, list):
        return str(node) if isinstance(node, Atom) else json.dumps(node, ensure_ascii=False)
    if not any(isinstance(n, list) for n in node):
        return '(' + ' '.join(dump(n) for n in node) + ')'
    out = '(' + ' '.join(dump(n) for n in node if not isinstance(n, list))
    for n in node:
        if isinstance(n, list):
            out += '\n' + '  ' * (depth + 1) + dump(n, depth + 1)
    return out + ')'

if __name__ == '__main__':
    for name in ['fan_power', 'power', 'ina', 'esp', 'ads']:
        doc = parse(Path(f'U:/plate_schemas/speed_and_temp_v8/{name}.kicad_sch').read_text(encoding='utf-8'))
        print('\nSHEET', name)
        for sym in children(child(doc, 'lib_symbols'), 'symbol'):
            if any(x in sym[1] for x in ['MCP', 'MMFTP', 'NXV', 'INA', 'ESP32', 'TPS']):
                print(sym[1], [(child(p,'number')[1],child(p,'name')[1],child(p,'at')[1:]) for p in walk(sym, 'pin')])
        for sym in children(doc, 'symbol'):
            props={n[1]:n[2] for n in children(sym,'property')}
            print(props.get('Reference'), props.get('Value'), props.get('Footprint'))
