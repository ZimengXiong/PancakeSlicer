import math
from pathlib import Path
import re

NOTE = 'cross-platform geometric equivalence, not byte identity'
RULE = 'printed layer Z strings identical; positive moving extrusion E times filament cross-section: each visible role per layer <= 0.5%, each internal role per layer <= 1.0% and total <= 0.5%; extrusion plus travel XYZ path length <= 0.5%; header model printing time <= 1%; XY arcs measured analytically; custom machine start/end excluded'
INTERNAL = {'Internal solid infill','Sparse infill','Inner wall','Support','Support interface','Support transition','Gap infill'}
WORD = re.compile(r'([A-Z])([-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)', re.I)


def seconds(value):
    terms = re.findall(r'([0-9]+(?:\.[0-9]+)?)\s*([dhms])', value)
    if not terms or re.sub(r'([0-9]+(?:\.[0-9]+)?)\s*([dhms])', '', value).strip():
        raise ValueError('Missing or invalid model printing time')
    return sum(float(number)*{'d':86400,'h':3600,'m':60,'s':1}[unit] for number,unit in terms)


def arc_length(start, end, words, clockwise):
    if 'R' in words or 'K' in words:
        raise ValueError('Qualification geometry requires XY I/J arcs')
    x, y = words.get('I',0), words.get('J',0)
    radius = math.hypot(x,y)
    if radius == 0:
        raise ValueError('Zero-radius arc')
    center = (start[0]+x,start[1]+y)
    first = math.atan2(-y,-x)
    last = math.atan2(end[1]-center[1],end[0]-center[0])
    sweep = (first-last if clockwise else last-first) % (2*math.pi)
    if start[:2] == end[:2]:
        sweep = 2*math.pi
    return math.hypot(radius*sweep,end[2]-start[2])


def metrics(path):
    position = [0.0,0.0,0.0]
    absolute = True
    relative_e = False
    extruder = 0.0
    feature = 'Undefined'
    nonplanar = False
    layers = []
    diameter = None
    model_time = None
    declared_layers = None
    with Path(path).open(encoding='utf-8') as source:
        for number,raw in enumerate(source,1):
            code,_,comment = raw.partition(';')
            comment = comment.strip()
            if comment.startswith('model printing time:'):
                model_time = seconds(comment.split(':',1)[1].split(';',1)[0])
            elif comment.startswith('filament_diameter:'):
                diameter = float(comment.split(':',1)[1])
            elif comment.startswith('total layer number:'):
                declared_layers = int(comment.split(':',1)[1])
            elif comment in {'CHANGE_LAYER','LAYER_CHANGE'}:
                layers.append({'z':None,'extrusion_mm':0.0,'path_mm':0.0,'extrusion_path_mm':0.0,'features':{},'first_line':number})
            elif comment.startswith(('Z_HEIGHT:','Z:')) and layers:
                if layers[-1]['z'] is not None:
                    raise ValueError('Duplicate layer Z marker')
                layers[-1]['z'] = comment.split(':',1)[1].strip()
            elif comment.startswith('_NONPLANAR_BEGIN'):
                nonplanar = True
            elif comment.startswith('_NONPLANAR_END'):
                nonplanar = False
            elif comment.startswith(('FEATURE:','TYPE:')):
                feature = comment.split(':',1)[1].strip()
            code = code.strip()
            if not code:
                continue
            command_match = re.match(r'^([GMT][0-9]+(?:\.[0-9]+)?)(?:\s|$)',code)
            if not command_match:
                continue
            command = command_match[1]
            words = {axis.upper():float(value) for axis,value in WORD.findall(code[command_match.end():])}
            if not all(math.isfinite(value) for value in words.values()):
                raise ValueError('Nonfinite G-code value')
            if command in {'G20','G18','G19','G90.1','G93'}:
                raise ValueError(f'Unsupported qualification geometry mode: {command}')
            if command == 'G90':
                absolute = True
            elif command == 'G91':
                absolute = False
            elif command == 'M82':
                relative_e = False
            elif command == 'M83':
                relative_e = True
            elif command == 'G92':
                for index,axis in enumerate('XYZ'):
                    if axis in words:
                        position[index] = words[axis]
                extruder = words.get('E',extruder)
            elif command in {'G0','G00','G1','G01','G2','G02','G3','G03'}:
                if any(axis in words for axis in 'ABC'):
                    raise ValueError('Rotary motion is not part of the Benchy equivalence definition')
                target = [(words[axis] if absolute else position[index]+words[axis]) if axis in words else position[index]
                          for index,axis in enumerate('XYZ')]
                delta_e = words.get('E',0.0) if relative_e else words.get('E',extruder)-extruder
                extruder = extruder+words.get('E',0.0) if relative_e else words.get('E',extruder)
                length = arc_length(position,target,words,command in {'G2','G02'}) if command in {'G2','G02','G3','G03'} else math.dist(position,target)
                if layers and feature != 'Custom':
                    layer = layers[-1]
                    role = 'Curved/non-planar roads' if nonplanar else feature
                    part = layer['features'].setdefault(role,{'extrusion_mm':0.0,'path_mm':0.0,'extrusion_path_mm':0.0,'moves':0})
                    layer['path_mm'] += length
                    part['path_mm'] += length
                    part['moves'] += 1
                    if delta_e > 0 and length > 1e-9:
                        layer['extrusion_mm'] += delta_e
                        layer['extrusion_path_mm'] += length
                        part['extrusion_mm'] += delta_e
                        part['extrusion_path_mm'] += length
                position = target
    if not layers or len(layers) != declared_layers or any(layer['z'] is None for layer in layers) or not diameter or not model_time:
        raise ValueError('Incomplete layer, filament or model-time metadata')
    section = math.pi*(diameter/2)**2
    for layer in layers:
        layer['extruded_volume_mm3'] = layer['extrusion_mm']*section
    return {'layer_count':len(layers),'layer_z':[layer['z'] for layer in layers], 'filament_diameter_mm':diameter,
            'model_time_s':model_time,'path_length_mm':sum(layer['path_mm'] for layer in layers),
            'extrusion_path_length_mm':sum(layer['extrusion_path_mm'] for layer in layers),'layers':layers}


def percentage(reference,candidate):
    if reference == 0:
        return 0.0 if candidate == 0 else None
    return abs(candidate-reference)/abs(reference)*100


def compare(reference,candidate):
    left,right = metrics(reference),metrics(candidate)
    same_layers = left['layer_count'] == right['layer_count'] and left['layer_z'] == right['layer_z']
    volumes = [percentage(a['extruded_volume_mm3'],b['extruded_volume_mm3']) for a,b in zip(left['layers'],right['layers'])]
    maximum = max(volumes) if same_layers and all(value is not None for value in volumes) else None
    path = percentage(left['path_length_mm'],right['path_length_mm'])
    duration = percentage(left['model_time_s'],right['model_time_s'])
    roles = sorted({role for layer in left['layers']+right['layers'] for role in layer['features']})
    per_role = {}
    for role in roles:
        a = [layer['features'].get(role,{}).get('extrusion_mm',0.0) for layer in left['layers']]
        b = [layer['features'].get(role,{}).get('extrusion_mm',0.0) for layer in right['layers']]
        differences = [percentage(x,y) for x,y in zip(a,b)]
        worst = max(differences) if same_layers and all(value is not None for value in differences) else None
        total = percentage(sum(a),sum(b))
        internal = role in INTERNAL
        limit = 1.0 if internal else 0.5
        per_role[role] = {'class':'internal' if internal else 'visible','max_layer_volume_pct':worst,
                          'total_volume_pct':total,'layer_limit_pct':limit,
                          'passed':worst is not None and worst <= limit and (not internal or total is not None and total <= 0.5),
                          'worst_layer':max(range(len(differences)),key=lambda i:differences[i] if differences[i] is not None else math.inf)+1}
    passed = same_layers and left['filament_diameter_mm'] == right['filament_diameter_mm'] and all(value['passed'] for value in per_role.values()) and path is not None and path <= 0.5 and duration is not None and duration <= 1.0
    return {'passed':passed,'mode':'geometric','note':NOTE,'rule':RULE,'same_layer_z':same_layers,
            'layer_count':right['layer_count'],'reference_layer_count':left['layer_count'],
            'max_layer_volume_pct':maximum,'path_length_pct':path,'time_pct':duration,
            'extrusion_path_length_pct':percentage(left['extrusion_path_length_mm'],right['extrusion_path_length_mm']),
            'per_role':per_role,'layer_volume_pct':volumes,'reference':left,'candidate':right}


def summary(comparison):
    value = comparison['geometry']
    roles = value['per_role']
    return {'mode':'geometric','note':NOTE,'rule':RULE,'passed':value['passed'],
            'reference_gcode_sha256':comparison['reference_gcode_sha256'],
            'max_layer_volume_pct':value['max_layer_volume_pct'],
            'max_visible_layer_volume_pct':max(item['max_layer_volume_pct'] for item in roles.values() if item['class']=='visible'),
            'max_internal_layer_volume_pct':max((item['max_layer_volume_pct'] for item in roles.values() if item['class']=='internal'),default=0),
            'total_volume_pct':percentage(sum(layer['extruded_volume_mm3'] for layer in value['reference']['layers']),sum(layer['extruded_volume_mm3'] for layer in value['candidate']['layers'])),
            'roles':{role:{'max_layer_pct':item['max_layer_volume_pct'],'total_pct':item['total_volume_pct'],'class':item['class']} for role,item in roles.items()},
            'path_length_pct':value['path_length_pct'],'extrusion_path_length_pct':value['extrusion_path_length_pct'],
            'time_pct':value['time_pct'],'layer_count':value['layer_count'],'same_layer_z':value['same_layer_z']}
