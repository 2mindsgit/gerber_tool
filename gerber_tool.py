#!/usr/bin/env python3
"""
gerber_tool.py — Outil de conversion Gerber <-> SVG

Modes :
  --svg     Gerber -> SVG + JSON   (pour edition dans Inkscape)
  --gerber  SVG -> Gerber          (reconversion apres edition)

Usage :
  python3 gerber_tool.py --svg  board.GTO [output.svg] [--outline board.GKO]
  python3 gerber_tool.py --gerber board.GTO.svg [output.GTO]
"""

import sys
import os
import re
import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path

INCH_TO_MM = 25.4
MM_TO_INCH = 1.0 / INCH_TO_MM
SVG_NS = '{http://www.w3.org/2000/svg}'


# === GERBER PARSING ===

def parse_format_spec(line):
    """Parse %FSLAX25Y25*% → dict"""
    m = re.match(r'%FS([LA])([AI])X(\d)(\d)Y(\d)(\d)\*%', line)
    if not m:
        return None
    return {
        'zero_omit': m.group(1),
        'coord_mode': m.group(2),
        'x_int': int(m.group(3)),
        'x_dec': int(m.group(4)),
        'y_int': int(m.group(5)),
        'y_dec': int(m.group(6)),
    }


def parse_aperture_def(line):
    """Parse %ADD10C,0.00600*% → (id, type, params, params_raw)"""
    m = re.match(r'%ADD(\d+)([A-Za-z][A-Za-z0-9]*),?(.*?)\*%', line)
    if not m:
        return None
    ap_id = int(m.group(1))
    ap_type = m.group(2)
    params_str = m.group(3)
    params = [float(p) for p in params_str.split('X')] if params_str else []
    params_raw = params_str.split('X') if params_str else []
    return ap_id, ap_type, params, params_raw


def parse_aperture_macro(lines):
    """Parse multi-line aperture macros."""
    macros = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith('%AM'):
            name = line[3:].rstrip('*')
            body_lines = []
            i += 1
            while i < len(lines) and not lines[i].startswith('%'):
                body_lines.append(lines[i].rstrip('*'))
                i += 1
            macros[name] = body_lines
        i += 1
    return macros


def parse_coord(s, n_dec):
    """Convertit une chaîne de coordonnée Gerber en float (inches)."""
    if s is None:
        return None
    return int(s) / (10 ** n_dec)


def parse_gerber(filepath):
    """Parse un fichier Gerber complet. Retourne (info, operations)."""
    with open(filepath, 'r') as f:
        raw = f.read()

    lines = raw.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    lines = [l.strip() for l in lines if l.strip()]

    info = {
        'source_file': os.path.basename(filepath),
        'units': 'inch',
        'format': None,
        'apertures': {},
        'macros': {},
        'polarity': 'D',
    }

    operations = []

    # Parse macros (multi-line)
    info['macros'] = parse_aperture_macro(lines)

    current_aperture = None
    current_x = 0.0
    current_y = 0.0
    fmt = None

    for line in lines:
        if line == '%MOIN*%':
            info['units'] = 'inch'
            continue
        if line == '%MOMM*%':
            info['units'] = 'mm'
            continue

        if line.startswith('%FS'):
            fmt = parse_format_spec(line)
            info['format'] = fmt
            continue

        if line.startswith('%ADD'):
            result = parse_aperture_def(line)
            if result:
                ap_id, ap_type, params, params_raw = result
                info['apertures'][str(ap_id)] = {
                    'type': ap_type,
                    'params': params,
                    'params_raw': params_raw,
                }
            continue

        m = re.match(r'%LP([DC])\*%', line)
        if m:
            info['polarity'] = m.group(1)
            continue

        if line.startswith('%'):
            continue
        if re.match(r'^G\d+\*$', line):
            continue
        if re.match(r'^M\d+\*$', line):
            continue

        m = re.match(r'^D(\d+)\*$', line)
        if m:
            d = int(m.group(1))
            if d >= 10:
                current_aperture = d
            continue

        if fmt is None:
            continue

        m = re.match(r'^(?:G\d+)?(?:X([+-]?\d+))?(?:Y([+-]?\d+))?D(\d+)\*$', line)
        if m:
            x_raw, y_raw, d_code = m.group(1), m.group(2), int(m.group(3))
            new_x = parse_coord(x_raw, fmt['x_dec']) if x_raw else current_x
            new_y = parse_coord(y_raw, fmt['y_dec']) if y_raw else current_y

            if d_code == 2:
                current_x = new_x
                current_y = new_y
            elif d_code == 1:
                operations.append({
                    'type': 'line',
                    'aperture': current_aperture,
                    'x1': current_x, 'y1': current_y,
                    'x2': new_x, 'y2': new_y,
                })
                current_x = new_x
                current_y = new_y
            elif d_code == 3:
                operations.append({
                    'type': 'flash',
                    'aperture': current_aperture,
                    'x': new_x, 'y': new_y,
                })
                current_x = new_x
                current_y = new_y

    return info, operations

# === GERBER -> SVG ===

def to_mm(val, units):
    return val * INCH_TO_MM if units == 'inch' else val


def aperture_diameter_mm(ap_def, macros, units):
    """Retourne le diamètre effectif d'une aperture en mm."""
    ap_type = ap_def['type']
    params = ap_def['params']

    if ap_type == 'C':
        return to_mm(params[0], units) if params else 0

    if ap_type == 'R':
        w = to_mm(params[0], units) if len(params) > 0 else 0
        h = to_mm(params[1], units) if len(params) > 1 else w
        return max(w, h)

    if ap_type in macros:
        for body_line in macros[ap_type]:
            parts = body_line.split(',')
            if parts[0].strip() == '5':
                diam_expr = parts[5].strip()
                if 'X' in diam_expr and '$' in diam_expr:
                    factor = float(diam_expr.split('X')[0])
                    diam = factor * params[0] if params else 0
                else:
                    diam = float(diam_expr)
                return to_mm(diam, units)

    return to_mm(params[0], units) if params else 0


def make_flash_svg(ap_def, macros, x_mm, y_mm, units, ap_id):
    """Génère l'élément SVG pour un flash."""
    ap_type = ap_def['type']
    params = ap_def['params']

    if ap_type == 'C':
        r = to_mm(params[0], units) / 2 if params else 0
        if r == 0:
            return f'<circle cx="{x_mm:.4f}" cy="{y_mm:.4f}" r="0.05" class="ap{ap_id} flash zero-size"/>'
        return f'<circle cx="{x_mm:.4f}" cy="{y_mm:.4f}" r="{r:.4f}" class="ap{ap_id} flash"/>'

    if ap_type == 'R':
        w = to_mm(params[0], units) if len(params) > 0 else 0
        h = to_mm(params[1], units) if len(params) > 1 else w
        rx = x_mm - w / 2
        ry = y_mm - h / 2
        return f'<rect x="{rx:.4f}" y="{ry:.4f}" width="{w:.4f}" height="{h:.4f}" class="ap{ap_id} flash"/>'

    if ap_type in macros:
        for body_line in macros[ap_type]:
            parts = body_line.split(',')
            if parts[0].strip() == '5':
                n_vert = int(parts[2].strip())
                diam_expr = parts[5].strip()
                rotation = float(parts[6].strip()) if len(parts) > 6 else 0
                if 'X' in diam_expr and '$' in diam_expr:
                    factor = float(diam_expr.split('X')[0])
                    diam = factor * params[0]
                else:
                    diam = float(diam_expr)
                r = to_mm(diam, units) / 2
                points = []
                for i in range(n_vert):
                    angle = math.radians(rotation + i * 360 / n_vert)
                    px = x_mm + r * math.cos(angle)
                    py = y_mm + r * math.sin(angle)
                    points.append(f'{px:.4f},{py:.4f}')
                return f'<polygon points="{" ".join(points)}" class="ap{ap_id} flash"/>'

    return f'<circle cx="{x_mm:.4f}" cy="{y_mm:.4f}" r="0.1" class="ap{ap_id} flash"/>'


def build_polylines(trace_ops, units):
    """Regroupe les segments consécutifs en polylines."""
    paths = []
    current_path = None
    for op in trace_ops:
        x1_mm = to_mm(op['x1'], units)
        y1_mm = -to_mm(op['y1'], units)
        x2_mm = to_mm(op['x2'], units)
        y2_mm = -to_mm(op['y2'], units)

        if (current_path
                and abs(current_path[-1][0] - x1_mm) < 0.0001
                and abs(current_path[-1][1] - y1_mm) < 0.0001):
            current_path.append((x2_mm, y2_mm))
        else:
            if current_path:
                paths.append(current_path)
            current_path = [(x1_mm, y1_mm), (x2_mm, y2_mm)]
    if current_path:
        paths.append(current_path)
    return paths


def generate_outline_layer(outline_ops, outline_info):
    """Génère le calque SVG verrouillé pour le contour PCB (GKO)."""
    units = outline_info['units']
    svg = []
    svg.append(f'  <!-- Contour PCB (référence verrouillée, ignoré à la reconversion) -->')
    svg.append(f'  <g id="outline-ref"'
               f' inkscape:groupmode="layer"'
               f' inkscape:label="[REF] Contour PCB"'
               f' sodipodi:insensitive="true"'
               f' style="opacity:0.6">')

    # Regrouper toutes les opérations de ligne (toutes apertures confondues)
    all_traces = [op for op in outline_ops if op['type'] == 'line']
    paths = build_polylines(all_traces, units)

    for i, path in enumerate(paths):
        d_parts = [f'M{path[0][0]:.4f},{path[0][1]:.4f}']
        for pt in path[1:]:
            d_parts.append(f'L{pt[0]:.4f},{pt[1]:.4f}')
        d_str = ' '.join(d_parts)
        svg.append(f'    <path d="{d_str}"'
                   f' stroke="#f0c030" stroke-width="0.15"'
                   f' fill="none" stroke-dasharray="1,0.5"'
                   f' id="outline-path-{i}"/>')

    svg.append(f'  </g>')
    return svg


def generate_svg(info, operations, outline_ops=None, outline_info=None):
    """Génère le contenu SVG complet."""
    units = info['units']
    macros = info['macros']
    apertures = info['apertures']

    # Calculer les bornes (inclure l'outline si présent)
    all_x = []
    all_y = []
    for op in operations:
        if op['type'] == 'line':
            all_x.extend([op['x1'], op['x2']])
            all_y.extend([op['y1'], op['y2']])
        elif op['type'] == 'flash':
            all_x.append(op['x'])
            all_y.append(op['y'])

    if outline_ops:
        for op in outline_ops:
            if op['type'] == 'line':
                all_x.extend([op['x1'], op['x2']])
                all_y.extend([op['y1'], op['y2']])

    if not all_x:
        return '<svg xmlns="http://www.w3.org/2000/svg"></svg>'

    margin_inch = 0.05
    min_x = min(all_x) - margin_inch
    max_x = max(all_x) + margin_inch
    min_y = min(all_y) - margin_inch
    max_y = max(all_y) + margin_inch

    vb_x = to_mm(min_x, units)
    vb_w = to_mm(max_x - min_x, units)
    vb_y = -to_mm(max_y, units)
    vb_h = to_mm(max_y - min_y, units)

    svg = []
    svg.append(f'<?xml version="1.0" encoding="UTF-8"?>')
    svg.append(f'<svg xmlns="http://www.w3.org/2000/svg"')
    svg.append(f'     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"')
    svg.append(f'     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.0.dtd"')
    svg.append(f'     width="{vb_w:.4f}mm" height="{vb_h:.4f}mm"')
    svg.append(f'     viewBox="{vb_x:.4f} {vb_y:.4f} {vb_w:.4f} {vb_h:.4f}">')
    svg.append(f'')

    # Style
    svg.append(f'  <defs>')
    svg.append(f'    <style>')
    svg.append(f'      .trace {{ fill: none; stroke: #b02020; stroke-linecap: round; stroke-linejoin: round; }}')
    svg.append(f'      .flash {{ fill: #b02020; stroke: none; }}')
    svg.append(f'      .zero-size {{ fill: #ff00ff; opacity: 0.3; }}')
    svg.append(f'    </style>')
    svg.append(f'  </defs>')
    svg.append(f'')

    # Background
    svg.append(f'  <rect x="{vb_x:.4f}" y="{vb_y:.4f}" width="{vb_w:.4f}" height="{vb_h:.4f}"')
    svg.append(f'        fill="#1a1a2e" id="background"/>')
    svg.append(f'')

    # Outline layer (verrouillé, en dessous de tout)
    if outline_ops and outline_info:
        svg.extend(generate_outline_layer(outline_ops, outline_info))
        svg.append(f'')

    # Couches par aperture
    ops_by_aperture = {}
    for op in operations:
        ap = op['aperture']
        if ap not in ops_by_aperture:
            ops_by_aperture[ap] = []
        ops_by_aperture[ap].append(op)

    for ap_id in sorted(ops_by_aperture.keys()):
        ops = ops_by_aperture[ap_id]
        ap_def = apertures.get(str(ap_id))
        if not ap_def:
            continue

        diam = aperture_diameter_mm(ap_def, macros, units)
        ap_desc = f'{ap_def["type"]}({",".join(f"{p}" for p in ap_def["params"])})'

        svg.append(f'  <!-- Aperture D{ap_id}: {ap_desc} -->')
        svg.append(f'  <g id="aperture-D{ap_id}"'
                   f' inkscape:groupmode="layer"'
                   f' inkscape:label="D{ap_id} {ap_desc}">')

        # Traces
        trace_ops = [op for op in ops if op['type'] == 'line']
        if trace_ops:
            svg.append(f'    <g id="traces-D{ap_id}" class="traces">')
            paths = build_polylines(trace_ops, units)
            for i, path in enumerate(paths):
                d_parts = [f'M{path[0][0]:.4f},{path[0][1]:.4f}']
                for pt in path[1:]:
                    d_parts.append(f'L{pt[0]:.4f},{pt[1]:.4f}')
                svg.append(f'      <path d="{" ".join(d_parts)}"'
                           f' stroke-width="{diam:.4f}"'
                           f' class="trace ap{ap_id}"'
                           f' id="trace-D{ap_id}-{i}"/>')
            svg.append(f'    </g>')

        # Flashes
        flash_ops = [op for op in ops if op['type'] == 'flash']
        if flash_ops:
            svg.append(f'    <g id="flashes-D{ap_id}" class="flashes">')
            for i, op in enumerate(flash_ops):
                x_mm = to_mm(op['x'], units)
                y_mm = -to_mm(op['y'], units)
                elem = make_flash_svg(ap_def, macros, x_mm, y_mm, units, ap_id)
                elem = elem.replace('/>', f' id="flash-D{ap_id}-{i}"/>', 1)
                if '"/' not in elem:
                    elem = elem.replace('class=', f'id="flash-D{ap_id}-{i}" class=', 1)
                svg.append(f'      {elem}')
            svg.append(f'    </g>')

        svg.append(f'  </g>')
        svg.append(f'')

    # Trouver la plus fine aperture circulaire non-zero pour le calque EDIT
    edit_ap_id = None
    edit_ap_diam = float('inf')
    for ap_id_str, ap_def in apertures.items():
        if ap_def['type'] == 'C' and ap_def['params']:
            d = ap_def['params'][0]
            if 0 < d < edit_ap_diam:
                edit_ap_diam = d
                edit_ap_id = int(ap_id_str)

    # Calque EDIT (vide, en haut de la pile)
    if edit_ap_id is not None:
        diam_mm = to_mm(edit_ap_diam, units)
        svg.append(f'  <!-- ============================================ -->')
        svg.append(f'  <!-- CALQUE EDIT : dessinez vos modifications ici -->')
        svg.append(f'  <!-- Aperture D{edit_ap_id} (trait {diam_mm:.3f}mm)     -->')
        svg.append(f'  <!-- Utilisez fill="#b02020" pour les formes      -->')
        svg.append(f'  <!-- ============================================ -->')
        svg.append(f'  <g id="edit-layer"'
                   f' inkscape:groupmode="layer"'
                   f' inkscape:label="EDIT (D{edit_ap_id} — {diam_mm:.3f}mm)">')
        svg.append(f'  </g>')
        svg.append(f'')

    svg.append(f'</svg>')
    return '\n'.join(svg)

# === SVG PARSING (paths, transforms, fill/stroke) ===

def lerp(a, b, t):
    return a + (b - a) * t


def cubic_bezier(p0, p1, p2, p3, n_steps=8):
    """Linéarise une courbe de Bézier cubique en n_steps segments."""
    pts = []
    for i in range(1, n_steps + 1):
        t = i / n_steps
        x = (1-t)**3*p0[0] + 3*(1-t)**2*t*p1[0] + 3*(1-t)*t**2*p2[0] + t**3*p3[0]
        y = (1-t)**3*p0[1] + 3*(1-t)**2*t*p1[1] + 3*(1-t)*t**2*p2[1] + t**3*p3[1]
        pts.append((x, y))
    return pts


def quad_bezier(p0, p1, p2, n_steps=8):
    """Linéarise une courbe de Bézier quadratique."""
    pts = []
    for i in range(1, n_steps + 1):
        t = i / n_steps
        x = (1-t)**2*p0[0] + 2*(1-t)*t*p1[0] + t**2*p2[0]
        y = (1-t)**2*p0[1] + 2*(1-t)*t*p1[1] + t**2*p2[1]
        pts.append((x, y))
    return pts


def arc_to_points(cx, cy, rx, ry, phi, theta1, dtheta, n_steps=16):
    """Convertit un arc SVG paramétrisé en points."""
    pts = []
    for i in range(1, n_steps + 1):
        t = theta1 + dtheta * i / n_steps
        cos_phi = math.cos(phi)
        sin_phi = math.sin(phi)
        x = cos_phi * rx * math.cos(t) - sin_phi * ry * math.sin(t) + cx
        y = sin_phi * rx * math.cos(t) + cos_phi * ry * math.sin(t) + cy
        pts.append((x, y))
    return pts


def svg_arc_to_center(x1, y1, rx, ry, phi, fa, fs, x2, y2):
    """Convertit les paramètres d'arc SVG endpoint → center parameterization."""
    if rx == 0 or ry == 0:
        return [(x2, y2)]

    rx, ry = abs(rx), abs(ry)
    phi_rad = math.radians(phi)
    cos_phi = math.cos(phi_rad)
    sin_phi = math.sin(phi_rad)

    dx2 = (x1 - x2) / 2
    dy2 = (y1 - y2) / 2
    x1p = cos_phi * dx2 + sin_phi * dy2
    y1p = -sin_phi * dx2 + cos_phi * dy2

    # Correction des rayons si trop petits
    lam = (x1p**2) / (rx**2) + (y1p**2) / (ry**2)
    if lam > 1:
        rx *= math.sqrt(lam)
        ry *= math.sqrt(lam)

    num = max(0, rx**2 * ry**2 - rx**2 * y1p**2 - ry**2 * x1p**2)
    den = rx**2 * y1p**2 + ry**2 * x1p**2
    if den == 0:
        return [(x2, y2)]

    sq = math.sqrt(num / den)
    if fa == fs:
        sq = -sq

    cxp = sq * rx * y1p / ry
    cyp = -sq * ry * x1p / rx

    cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) / 2
    cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        n = math.sqrt(ux*ux + uy*uy) * math.sqrt(vx*vx + vy*vy)
        if n == 0:
            return 0
        c = (ux*vx + uy*vy) / n
        c = max(-1, min(1, c))
        a = math.acos(c)
        if ux*vy - uy*vx < 0:
            a = -a
        return a

    theta1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dtheta = angle((x1p - cxp) / rx, (y1p - cyp) / ry,
                   (-x1p - cxp) / rx, (-y1p - cyp) / ry)

    if fs == 0 and dtheta > 0:
        dtheta -= 2 * math.pi
    elif fs == 1 and dtheta < 0:
        dtheta += 2 * math.pi

    return arc_to_points(cx, cy, rx, ry, phi_rad, theta1, dtheta)


def tokenize_svg_path(d):
    """Tokenise un path SVG en commandes et nombres."""
    return re.findall(r'[MmLlHhVvCcSsQqTtAaZz]|[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?', d)


def parse_svg_path_d(d):
    """Parse complet d'un path SVG. Retourne une liste de polylines."""
    tokens = tokenize_svg_path(d)
    polylines = []
    current = []
    cx, cy = 0, 0        # position courante
    sx, sy = 0, 0        # start of subpath (pour Z)
    last_cp = None        # dernier point de contrôle (pour S/T)
    last_cmd = ''

    i = 0

    def next_float():
        nonlocal i
        if i < len(tokens):
            val = float(tokens[i])
            i += 1
            return val
        return 0

    def next_flag():
        nonlocal i
        if i < len(tokens):
            # Les flags peuvent être collés (ex: "0,0,1" ou "001")
            val = int(float(tokens[i]))
            i += 1
            return val
        return 0

    while i < len(tokens):
        tok = tokens[i]
        if tok.isalpha() and tok not in '.-+':
            cmd = tok
            i += 1
        else:
            # Commande implicite (répétition)
            cmd = last_cmd
            if cmd == 'M':
                cmd = 'L'
            elif cmd == 'm':
                cmd = 'l'

        if cmd in 'Zz':
            if current and len(current) > 1:
                current.append((sx, sy))
            cx, cy = sx, sy
            last_cmd = cmd
            continue

        if cmd == 'M':
            if current and len(current) > 0:
                polylines.append(current)
            cx, cy = next_float(), next_float()
            sx, sy = cx, cy
            current = [(cx, cy)]
            last_cmd = 'M'

        elif cmd == 'm':
            if current and len(current) > 0:
                polylines.append(current)
            cx += next_float()
            cy += next_float()
            sx, sy = cx, cy
            current = [(cx, cy)]
            last_cmd = 'm'

        elif cmd == 'L':
            cx, cy = next_float(), next_float()
            current.append((cx, cy))
            last_cmd = 'L'

        elif cmd == 'l':
            cx += next_float()
            cy += next_float()
            current.append((cx, cy))
            last_cmd = 'l'

        elif cmd == 'H':
            cx = next_float()
            current.append((cx, cy))
            last_cmd = 'H'

        elif cmd == 'h':
            cx += next_float()
            current.append((cx, cy))
            last_cmd = 'h'

        elif cmd == 'V':
            cy = next_float()
            current.append((cx, cy))
            last_cmd = 'V'

        elif cmd == 'v':
            cy += next_float()
            current.append((cx, cy))
            last_cmd = 'v'

        elif cmd == 'C':
            x1, y1 = next_float(), next_float()
            x2, y2 = next_float(), next_float()
            x, y = next_float(), next_float()
            pts = cubic_bezier((cx, cy), (x1, y1), (x2, y2), (x, y))
            current.extend(pts)
            last_cp = (x2, y2)
            cx, cy = x, y
            last_cmd = 'C'

        elif cmd == 'c':
            x1, y1 = cx + next_float(), cy + next_float()
            x2, y2 = cx + next_float(), cy + next_float()
            x, y = cx + next_float(), cy + next_float()
            pts = cubic_bezier((cx, cy), (x1, y1), (x2, y2), (x, y))
            current.extend(pts)
            last_cp = (x2, y2)
            cx, cy = x, y
            last_cmd = 'c'

        elif cmd == 'S':
            if last_cmd in 'CcSs' and last_cp:
                x1 = 2 * cx - last_cp[0]
                y1 = 2 * cy - last_cp[1]
            else:
                x1, y1 = cx, cy
            x2, y2 = next_float(), next_float()
            x, y = next_float(), next_float()
            pts = cubic_bezier((cx, cy), (x1, y1), (x2, y2), (x, y))
            current.extend(pts)
            last_cp = (x2, y2)
            cx, cy = x, y
            last_cmd = 'S'

        elif cmd == 's':
            if last_cmd in 'CcSs' and last_cp:
                x1 = 2 * cx - last_cp[0]
                y1 = 2 * cy - last_cp[1]
            else:
                x1, y1 = cx, cy
            x2, y2 = cx + next_float(), cy + next_float()
            x, y = cx + next_float(), cy + next_float()
            pts = cubic_bezier((cx, cy), (x1, y1), (x2, y2), (x, y))
            current.extend(pts)
            last_cp = (x2, y2)
            cx, cy = x, y
            last_cmd = 's'

        elif cmd == 'Q':
            x1, y1 = next_float(), next_float()
            x, y = next_float(), next_float()
            pts = quad_bezier((cx, cy), (x1, y1), (x, y))
            current.extend(pts)
            last_cp = (x1, y1)
            cx, cy = x, y
            last_cmd = 'Q'

        elif cmd == 'q':
            x1, y1 = cx + next_float(), cy + next_float()
            x, y = cx + next_float(), cy + next_float()
            pts = quad_bezier((cx, cy), (x1, y1), (x, y))
            current.extend(pts)
            last_cp = (x1, y1)
            cx, cy = x, y
            last_cmd = 'q'

        elif cmd == 'T':
            if last_cmd in 'QqTt' and last_cp:
                x1 = 2 * cx - last_cp[0]
                y1 = 2 * cy - last_cp[1]
            else:
                x1, y1 = cx, cy
            x, y = next_float(), next_float()
            pts = quad_bezier((cx, cy), (x1, y1), (x, y))
            current.extend(pts)
            last_cp = (x1, y1)
            cx, cy = x, y
            last_cmd = 'T'

        elif cmd == 't':
            if last_cmd in 'QqTt' and last_cp:
                x1 = 2 * cx - last_cp[0]
                y1 = 2 * cy - last_cp[1]
            else:
                x1, y1 = cx, cy
            x, y = cx + next_float(), cy + next_float()
            pts = quad_bezier((cx, cy), (x1, y1), (x, y))
            current.extend(pts)
            last_cp = (x1, y1)
            cx, cy = x, y
            last_cmd = 't'

        elif cmd == 'A':
            rx = next_float()
            ry = next_float()
            phi = next_float()
            fa = next_flag()
            fs = next_flag()
            x, y = next_float(), next_float()
            pts = svg_arc_to_center(cx, cy, rx, ry, phi, fa, fs, x, y)
            current.extend(pts)
            cx, cy = x, y
            last_cmd = 'A'

        elif cmd == 'a':
            rx = next_float()
            ry = next_float()
            phi = next_float()
            fa = next_flag()
            fs = next_flag()
            dx, dy = next_float(), next_float()
            x, y = cx + dx, cy + dy
            pts = svg_arc_to_center(cx, cy, rx, ry, phi, fa, fs, x, y)
            current.extend(pts)
            cx, cy = x, y
            last_cmd = 'a'

        else:
            i += 1  # skip unknown

    if current:
        polylines.append(current)

    return polylines


# ---------------------------------------------------------------------------
#  Transform parsing
# ---------------------------------------------------------------------------

def parse_transform(transform_str):
    """Parse un attribut transform SVG et retourne une matrice 3x3 [a,b,c,d,e,f]."""
    if not transform_str:
        return None

    result = [1, 0, 0, 1, 0, 0]  # identity

    for m in re.finditer(r'(\w+)\s*\(([^)]+)\)', transform_str):
        func = m.group(1)
        vals = [float(v) for v in re.findall(r'[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?', m.group(2))]

        if func == 'matrix' and len(vals) >= 6:
            result = vals[:6]
        elif func == 'translate':
            tx = vals[0] if len(vals) > 0 else 0
            ty = vals[1] if len(vals) > 1 else 0
            result = multiply_matrices(result, [1, 0, 0, 1, tx, ty])
        elif func == 'scale':
            sx = vals[0] if len(vals) > 0 else 1
            sy = vals[1] if len(vals) > 1 else sx
            result = multiply_matrices(result, [sx, 0, 0, sy, 0, 0])
        elif func == 'rotate':
            a = math.radians(vals[0])
            cos_a, sin_a = math.cos(a), math.sin(a)
            result = multiply_matrices(result, [cos_a, sin_a, -sin_a, cos_a, 0, 0])

    return result


def multiply_matrices(m1, m2):
    """Multiplie deux matrices de transformation SVG [a,b,c,d,e,f]."""
    a1, b1, c1, d1, e1, f1 = m1
    a2, b2, c2, d2, e2, f2 = m2
    return [
        a1*a2 + c1*b2,
        b1*a2 + d1*b2,
        a1*c2 + c1*d2,
        b1*c2 + d1*d2,
        a1*e2 + c1*f2 + e1,
        b1*e2 + d1*f2 + f1,
    ]


def apply_transform(matrix, x, y):
    """Applique une matrice de transformation à un point."""
    if matrix is None:
        return x, y
    a, b, c, d, e, f = matrix
    return a*x + c*y + e, b*x + d*y + f


# ---------------------------------------------------------------------------
#  SVG Element Extraction
# ---------------------------------------------------------------------------

def detect_fill_stroke(elem):
    """Détecte si un élément SVG a un fill et/ou un stroke.
    Retourne (has_fill, has_stroke)."""
    style = elem.get('style', '')
    fill_attr = elem.get('fill')
    stroke_attr = elem.get('stroke')

    # Parser le style inline
    fill_val = None
    stroke_val = None
    for part in style.split(';'):
        part = part.strip()
        if part.startswith('fill:'):
            fill_val = part.split(':',1)[1].strip()
        elif part.startswith('stroke:'):
            stroke_val = part.split(':',1)[1].strip()

    # Attributs directs (priorité plus basse que style)
    if fill_val is None:
        fill_val = fill_attr
    if stroke_val is None:
        stroke_val = stroke_attr

    # Défauts SVG : fill=black, stroke=none
    has_fill = fill_val is not None and fill_val.lower() != 'none'
    if fill_val is None:
        has_fill = True  # défaut SVG = rempli

    has_stroke = stroke_val is not None and stroke_val.lower() != 'none'

    return has_fill, has_stroke


def extract_polylines_from_element(elem, parent_transform=None):
    """Extrait les polylines géométriques d'un élément SVG."""
    polylines = []

    elem_transform = parse_transform(elem.get('transform'))
    if parent_transform and elem_transform:
        matrix = multiply_matrices(parent_transform, elem_transform)
    elif elem_transform:
        matrix = elem_transform
    else:
        matrix = parent_transform

    tag = elem.tag.replace(SVG_NS, '')

    if tag == 'path':
        d = elem.get('d', '')
        if d:
            for pl in parse_svg_path_d(d):
                if matrix:
                    pl = [apply_transform(matrix, x, y) for x, y in pl]
                if len(pl) >= 2:
                    polylines.append(pl)

    elif tag == 'polygon':
        pts_str = elem.get('points', '').strip()
        if pts_str:
            coords = re.findall(r'[+-]?(?:\d+\.?\d*|\.\d+)', pts_str)
            pts = []
            for j in range(0, len(coords) - 1, 2):
                x, y = float(coords[j]), float(coords[j+1])
                if matrix:
                    x, y = apply_transform(matrix, x, y)
                pts.append((x, y))
            if len(pts) >= 2:
                pts.append(pts[0])
                polylines.append(pts)

    elif tag == 'rect':
        if elem.get('id') == 'background':
            return polylines
        x = float(elem.get('x', 0))
        y = float(elem.get('y', 0))
        w = float(elem.get('width', 0))
        h = float(elem.get('height', 0))
        pts = [(x, y), (x+w, y), (x+w, y+h), (x, y+h), (x, y)]
        if matrix:
            pts = [apply_transform(matrix, px, py) for px, py in pts]
        polylines.append(pts)

    elif tag == 'circle':
        cx_v = float(elem.get('cx', 0))
        cy_v = float(elem.get('cy', 0))
        r = float(elem.get('r', 0))
        if r > 0:
            pts = []
            for j in range(33):
                angle = 2 * math.pi * j / 32
                px = cx_v + r * math.cos(angle)
                py = cy_v + r * math.sin(angle)
                if matrix:
                    px, py = apply_transform(matrix, px, py)
                pts.append((px, py))
            polylines.append(pts)

    return polylines


def extract_paths_from_element(elem, parent_transform=None):
    """Compat: retourne les polylines sans distinction fill/stroke."""
    return extract_polylines_from_element(elem, parent_transform)


def extract_with_fill_info(elem, parent_transform=None):
    """Extrait les polylines avec info fill/stroke.
    Retourne (fills, holes, strokes) — trois listes de polylines.
    fills = contours extérieurs (dark), holes = évidements (clear)."""
    polylines = extract_polylines_from_element(elem, parent_transform)
    has_fill, has_stroke = detect_fill_stroke(elem)

    fills = []
    holes = []
    strokes = []

    if has_fill and len(polylines) > 1:
        # Plusieurs sous-chemins : séparer extérieur / trous via l'aire signée
        areas = [(signed_area(pl), pl) for pl in polylines]
        # Le sous-chemin avec la plus grande aire absolue = extérieur
        # Les autres avec un signe opposé = trous
        max_area_item = max(areas, key=lambda x: abs(x[0]))
        outer_sign = 1 if max_area_item[0] >= 0 else -1

        for area, pl in areas:
            if area == 0:
                fills.append(pl)  # dégénéré, traité comme fill
            elif (area > 0) == (outer_sign > 0):
                fills.append(pl)  # même signe que l'extérieur
            else:
                holes.append(pl)  # signe opposé = trou
    elif has_fill:
        fills = polylines

    if has_stroke:
        strokes = polylines

    if not has_fill and not has_stroke:
        fills = polylines

    return fills, holes, strokes


def signed_area(polyline):
    """Aire signée d'un polygone (shoelace formula).
    Positif = sens horaire en coordonnées SVG (Y vers le bas)."""
    area = 0
    n = len(polyline)
    for i in range(n):
        x1, y1 = polyline[i]
        x2, y2 = polyline[(i + 1) % n]
        area += (x2 - x1) * (y2 + y1)
    return area / 2


def extract_all_from_group(group, parent_transform=None):
    """Extrait récursivement toutes les polylines d'un groupe SVG.
    Retourne (fills, holes, strokes)."""
    all_fills = []
    all_holes = []
    all_strokes = []

    group_transform = parse_transform(group.get('transform'))
    if parent_transform and group_transform:
        matrix = multiply_matrices(parent_transform, group_transform)
    elif group_transform:
        matrix = group_transform
    else:
        matrix = parent_transform

    for child in group:
        tag = child.tag.replace(SVG_NS, '')
        if tag == 'g':
            fills, holes, strokes = extract_all_from_group(child, matrix)
            all_fills.extend(fills)
            all_holes.extend(holes)
            all_strokes.extend(strokes)
        else:
            fills, holes, strokes = extract_with_fill_info(child, matrix)
            all_fills.extend(fills)
            all_holes.extend(holes)
            all_strokes.extend(strokes)

    return all_fills, all_holes, all_strokes


def parse_svg_elements(svg_path, edit_aperture=None):
    """Parse le SVG et extrait les éléments par aperture."""
    tree = ET.parse(svg_path)
    root = tree.getroot()

    elements = {
        'traces': {},         # ap_id -> [polylines] (contours, D01)
        'flashes': {},        # ap_id -> [(x,y)]
        'regions': {},        # ap_id -> [polylines] (remplissages dark, G36/G37)
        'regions_clear': {},  # ap_id -> [polylines] (évidements clear, G36/G37 + LPC)
    }

    def add_fills_strokes(ap_id, fills, holes, strokes, label=''):
        if fills:
            if ap_id not in elements['regions']:
                elements['regions'][ap_id] = []
            elements['regions'][ap_id].extend(fills)
        if holes:
            if ap_id not in elements['regions_clear']:
                elements['regions_clear'][ap_id] = []
            elements['regions_clear'][ap_id].extend(holes)
        if strokes:
            if ap_id not in elements['traces']:
                elements['traces'][ap_id] = []
            elements['traces'][ap_id].extend(strokes)
        total = len(fills) + len(holes) + len(strokes)
        if total and label:
            parts = []
            if fills: parts.append(f'{len(fills)} fills')
            if holes: parts.append(f'{len(holes)} holes')
            if strokes: parts.append(f'{len(strokes)} strokes')
            print(f'  {label}: {" + ".join(parts)} -> D{ap_id}')

    for g in root.iter(f'{SVG_NS}g'):
        g_id = g.get('id', '')

        # Ignorer le calque outline de référence
        if g_id == 'outline-ref':
            continue

        # Calque EDIT : fill → regions, holes → regions_clear, stroke → traces
        if g_id == 'edit-layer' and edit_aperture is not None:
            fills, holes, strokes = extract_all_from_group(g)
            add_fills_strokes(edit_aperture, fills, holes, strokes, 'EDIT layer')
            continue

        # Groupe traces-Dxx (éléments originaux — toujours en traces)
        m = re.match(r'traces-D(\d+)', g_id)
        if m:
            ap_id = int(m.group(1))
            traces = []
            for child in g:
                traces.extend(extract_paths_from_element(child))
            if ap_id not in elements['traces']:
                elements['traces'][ap_id] = []
            elements['traces'][ap_id].extend(traces)
            continue

        # Groupe flashes-Dxx (éléments originaux)
        m = re.match(r'flashes-D(\d+)', g_id)
        if m:
            ap_id = int(m.group(1))
            flashes = []
            for child in g:
                tag = child.tag.replace(SVG_NS, '')
                if tag == 'circle':
                    cx_v = float(child.get('cx', 0))
                    cy_v = float(child.get('cy', 0))
                    flashes.append((cx_v, cy_v))
                elif tag == 'rect' and child.get('id') != 'background':
                    x = float(child.get('x', 0))
                    y = float(child.get('y', 0))
                    w = float(child.get('width', 0))
                    h = float(child.get('height', 0))
                    flashes.append((x + w/2, y + h/2))
                elif tag == 'polygon':
                    pts = child.get('points', '').strip().split()
                    if pts:
                        xs, ys = [], []
                        for c in pts:
                            parts = c.split(',')
                            xs.append(float(parts[0]))
                            ys.append(float(parts[1]))
                        flashes.append((sum(xs)/len(xs), sum(ys)/len(ys)))
            if ap_id not in elements['flashes']:
                elements['flashes'][ap_id] = []
            elements['flashes'][ap_id].extend(flashes)
            continue

        # Fallback: éléments ajoutés dans un groupe aperture-Dxx
        m = re.match(r'aperture-D(\d+)', g_id)
        if m:
            ap_id = int(m.group(1))
            fallback_ap = edit_aperture if edit_aperture else ap_id
            for child in g:
                child_id = child.get('id', '')
                if child_id.startswith('traces-D') or child_id.startswith('flashes-D'):
                    continue
                fills, holes, strokes = extract_with_fill_info(child)
                add_fills_strokes(fallback_ap, fills, holes, strokes,
                                  f'[compat] D{ap_id}/{child_id}')

    return elements


# === SVG -> GERBER ===

def mm_to_gerber_coord(val_mm, units, n_dec):
    if units == 'inch':
        val = val_mm * MM_TO_INCH
    else:
        val = val_mm
    return round(val * (10 ** n_dec))


def format_coord(val, n_int, n_dec):
    total = n_int + n_dec
    s = f'{abs(val):0{total}d}'
    if val < 0:
        s = '-' + s
    return s


def generate_gerber(info, elements):
    fmt = info['format']
    units = info['units']
    x_dec = fmt['x_dec']
    y_dec = fmt['y_dec']
    x_int = fmt['x_int']
    y_int = fmt['y_int']

    lines = []

    # Header
    lines.append('G75*')
    if units == 'inch':
        lines.append('%MOIN*%')
    else:
        lines.append('%MOMM*%')
    lines.append('%OFA0B0*%')
    lines.append(f'%FS{fmt["zero_omit"]}{fmt["coord_mode"]}'
                 f'X{x_int}{x_dec}Y{y_int}{y_dec}*%')
    lines.append('%IPPOS*%')
    lines.append('%LPD*%')

    # Macros
    for macro_name, macro_body in info['macros'].items():
        lines.append(f'%AM{macro_name}*')
        for body_line in macro_body:
            lines.append(f'{body_line}*')
        lines.append('%')

    # Aperture definitions
    for ap_id_str, ap_def in sorted(info['apertures'].items(), key=lambda x: int(x[0])):
        if 'params_raw' in ap_def and ap_def['params_raw']:
            params_str = 'X'.join(ap_def['params_raw'])
        else:
            params_str = 'X'.join(f'{p}' for p in ap_def['params']) if ap_def['params'] else ''
        sep = ',' if params_str else ''
        lines.append(f'%ADD{ap_id_str}{ap_def["type"]}{sep}{params_str}*%')

    # Drawing commands
    all_ap_ids = sorted(set(
        list(elements['traces'].keys()) +
        list(elements['flashes'].keys()) +
        list(elements.get('regions', {}).keys()) +
        list(elements.get('regions_clear', {}).keys())
    ))

    def emit_region(polyline):
        """Émet un bloc G36/G37 pour une polyline."""
        if len(polyline) < 3:
            return
        lines.append('G36*')
        x_mm, y_mm = polyline[0]
        y_mm = -y_mm
        gx = mm_to_gerber_coord(x_mm, units, x_dec)
        gy = mm_to_gerber_coord(y_mm, units, y_dec)
        lines.append(f'X{format_coord(gx, x_int, x_dec)}'
                     f'Y{format_coord(gy, y_int, y_dec)}D02*')
        for x_mm, y_mm in polyline[1:]:
            y_mm = -y_mm
            gx = mm_to_gerber_coord(x_mm, units, x_dec)
            gy = mm_to_gerber_coord(y_mm, units, y_dec)
            lines.append(f'X{format_coord(gx, x_int, x_dec)}'
                         f'Y{format_coord(gy, y_int, y_dec)}D01*')
        # Fermer si pas déjà fermé
        x0, y0 = polyline[0]
        xn, yn = polyline[-1]
        if abs(x0 - xn) > 0.001 or abs(y0 - yn) > 0.001:
            y0_f = -y0
            gx = mm_to_gerber_coord(x0, units, x_dec)
            gy = mm_to_gerber_coord(y0_f, units, y_dec)
            lines.append(f'X{format_coord(gx, x_int, x_dec)}'
                         f'Y{format_coord(gy, y_int, y_dec)}D01*')
        lines.append('G37*')

    for ap_id in all_ap_ids:
        lines.append(f'D{ap_id}*')

        # Traces (D01 lines)
        if ap_id in elements['traces']:
            for polyline in elements['traces'][ap_id]:
                if len(polyline) < 2:
                    continue
                x_mm, y_mm = polyline[0]
                y_mm = -y_mm
                gx = mm_to_gerber_coord(x_mm, units, x_dec)
                gy = mm_to_gerber_coord(y_mm, units, y_dec)
                lines.append(f'X{format_coord(gx, x_int, x_dec)}'
                             f'Y{format_coord(gy, y_int, y_dec)}D02*')
                for x_mm, y_mm in polyline[1:]:
                    y_mm = -y_mm
                    gx = mm_to_gerber_coord(x_mm, units, x_dec)
                    gy = mm_to_gerber_coord(y_mm, units, y_dec)
                    lines.append(f'X{format_coord(gx, x_int, x_dec)}'
                                 f'Y{format_coord(gy, y_int, y_dec)}D01*')

        # Flashes (D03)
        if ap_id in elements['flashes']:
            for x_mm, y_mm in elements['flashes'][ap_id]:
                y_mm = -y_mm
                gx = mm_to_gerber_coord(x_mm, units, x_dec)
                gy = mm_to_gerber_coord(y_mm, units, y_dec)
                lines.append(f'X{format_coord(gx, x_int, x_dec)}'
                             f'Y{format_coord(gy, y_int, y_dec)}D03*')

        # Regions dark (G36/G37 — formes remplies)
        if ap_id in elements.get('regions', {}):
            for polyline in elements['regions'][ap_id]:
                emit_region(polyline)

        # Regions clear (G36/G37 + LPC — évidements/trous)
        if ap_id in elements.get('regions_clear', {}):
            lines.append('%LPC*%')
            for polyline in elements['regions_clear'][ap_id]:
                emit_region(polyline)
            lines.append('%LPD*%')

    lines.append('M02*')
    return '\n'.join(lines)

# === COMMANDE --svg ===

def cmd_svg(args):
    # Parse arguments
    # args passés en paramètre
    outline_path = None

    # Extraire --outline
    if '--outline' in args:
        idx = args.index('--outline')
        if idx + 1 < len(args):
            outline_path = args[idx + 1]
            args = args[:idx] + args[idx+2:]
        else:
            print('Erreur: --outline nécessite un fichier GKO')
            sys.exit(1)

    if len(args) < 1:
        print(f'Usage: {'gerber_tool.py --svg'} input.GTO [output.svg] [--outline board.GKO]')
        print()
        print('Extensions Gerber courantes (Eagle / KiCad) :')
        print('  .GTL  Top Copper          (cuivre face composants)')
        print('  .GBL  Bottom Copper       (cuivre face soudure)')
        print('  .GTO  Top Silkscreen      (sérigraphie face composants)')
        print('  .GBO  Bottom Silkscreen   (sérigraphie face soudure)')
        print('  .GTS  Top Soldermask      (vernis face composants)')
        print('  .GBS  Bottom Soldermask   (vernis face soudure)')
        print('  .GTP  Top Paste           (pâte à souder composants)')
        print('  .GBP  Bottom Paste        (pâte à souder soudure)')
        print('  .GKO  Board Outline       (contour du PCB)')
        print('  .GML  Milling Layer       (contour Eagle, = GKO)')
        print('  .DRL  Drill / Excellon    (perçages — format différent)')
        print()
        print('Options :')
        print('  --outline board.GKO   Ajoute le contour PCB en calque')
        print('                        verrouillé (ignoré à la reconversion)')
        sys.exit(1)

    input_path = args[0]
    # Nommage par défaut : input.GTO → input.GTO.svg / input.GTO.gerber_info.json
    input_name = os.path.basename(input_path)
    output_svg = args[1] if len(args) > 1 else input_name + '.svg'
    output_json = output_svg.rsplit('.svg', 1)[0] + '.gerber_info.json'

    print(f'Parsing {input_path}...')
    info, operations = parse_gerber(input_path)

    print(f'  Units: {info["units"]}')
    print(f'  Format: {info["format"]}')
    print(f'  Apertures: {len(info["apertures"])}')
    print(f'  Macros: {len(info["macros"])}')
    print(f'  Operations: {len(operations)} '
          f'({sum(1 for o in operations if o["type"] == "line")} lines, '
          f'{sum(1 for o in operations if o["type"] == "flash")} flashes)')

    # Outline optionnel
    outline_ops = None
    outline_info = None
    if outline_path:
        print(f'Parsing outline {outline_path}...')
        outline_info, outline_ops = parse_gerber(outline_path)
        print(f'  Outline: {len(outline_ops)} operations')

    print(f'Generating SVG...')
    svg_content = generate_svg(info, operations, outline_ops, outline_info)

    with open(output_svg, 'w') as f:
        f.write(svg_content)
    print(f'  -> {output_svg} ({os.path.getsize(output_svg)} bytes)')

    # JSON info (sans l'outline — pas nécessaire pour la reconversion)
    # Trouver l'aperture EDIT (la plus fine circulaire non-zero)
    edit_ap_id = None
    edit_ap_diam = float('inf')
    for ap_id_str, ap_def in info['apertures'].items():
        if ap_def['type'] == 'C' and ap_def['params']:
            d = ap_def['params'][0]
            if 0 < d < edit_ap_diam:
                edit_ap_diam = d
                edit_ap_id = int(ap_id_str)

    export_info = {
        **info,
        'operations': operations,
        'svg_file': output_svg,
        'edit_aperture': edit_ap_id,
    }
    with open(output_json, 'w') as f:
        json.dump(export_info, f, indent=2)
    print(f'  -> {output_json} ({os.path.getsize(output_json)} bytes)')

    print('Done!')


# === COMMANDE --gerber ===

def cmd_gerber(args):
    if len(args) < 1:
        print(f'Usage: {'gerber_tool.py --gerber'} board.GTO.svg [output.GTO]')
        print()
        print('  Le fichier board.GTO.gerber_info.json doit exister')
        print('  (créé automatiquement par gerber2svg.py)')
        print()
        print('  Si output omis : board_EDIT.GTO')
        sys.exit(1)

    svg_path = args[0]

    if svg_path.endswith('.svg'):
        base = svg_path[:-4]
    else:
        base = svg_path

    json_path = base + '.gerber_info.json'

    if not os.path.exists(json_path):
        print(f'Erreur: {json_path} introuvable.')
        print(f'  Ce fichier est généré par gerber2svg.py lors de la conversion.')
        sys.exit(1)

    if len(args) > 1:
        output_path = args[1]
    else:
        dot_pos = base.rfind('.')
        if dot_pos > 0:
            stem = base[:dot_pos]
            ext = base[dot_pos+1:]
            output_path = f'{stem}_EDIT.{ext}'
        else:
            output_path = base + '_EDIT.gbr'

    print(f'Loading info from {json_path}...')
    with open(json_path, 'r') as f:
        info = json.load(f)

    print(f'  Source: {info["source_file"]}')
    print(f'  Units: {info["units"]}')
    print(f'  Apertures: {len(info["apertures"])}')

    print(f'Parsing SVG {svg_path}...')
    edit_aperture = info.get('edit_aperture')
    elements = parse_svg_elements(svg_path, edit_aperture)

    n_traces = sum(len(v) for v in elements['traces'].values())
    n_flashes = sum(len(v) for v in elements['flashes'].values())
    n_regions = sum(len(v) for v in elements.get('regions', {}).values())
    n_holes = sum(len(v) for v in elements.get('regions_clear', {}).values())
    print(f'  Traces: {n_traces} polylines')
    print(f'  Flashes: {n_flashes}')
    if n_regions:
        print(f'  Regions (fill): {n_regions}')
    if n_holes:
        print(f'  Regions (holes): {n_holes}')

    print(f'Generating Gerber...')
    gerber_content = generate_gerber(info, elements)

    with open(output_path, 'w') as f:
        f.write(gerber_content)
        f.write('\n')
    print(f'  -> {output_path} ({os.path.getsize(output_path)} bytes)')

    print('Done!')


# === MAIN ===

EXTENSIONS_HELP = '''Extensions Gerber courantes (Eagle / KiCad) :
  .GTL  Top Copper          (cuivre face composants)
  .GBL  Bottom Copper       (cuivre face soudure)
  .GTO  Top Silkscreen      (serigraphie face composants)
  .GBO  Bottom Silkscreen   (serigraphie face soudure)
  .GTS  Top Soldermask      (vernis face composants)
  .GBS  Bottom Soldermask   (vernis face soudure)
  .GTP  Top Paste           (pate a souder composants)
  .GBP  Bottom Paste        (pate a souder soudure)
  .GKO  Board Outline       (contour du PCB)
  .GML  Milling Layer       (contour Eagle, = GKO)
  .DRL  Drill / Excellon    (percages — format different)'''

def main():
    args = sys.argv[1:]
    prog = os.path.basename(sys.argv[0])

    if not args or args[0] in ('-h', '--help'):
        print(f'Usage: {prog} --svg  board.GTO [output.svg] [--outline board.GKO]')
        print(f'       {prog} --gerber board.GTO.svg [output.GTO]')
        print()
        print('Modes :')
        print('  --svg     Gerber -> SVG + JSON   (pour edition dans Inkscape)')
        print('  --gerber  SVG -> Gerber          (reconversion apres edition)')
        print()
        print(EXTENSIONS_HELP)
        print()
        print('Options (--svg) :')
        print('  --outline board.GKO   Contour PCB en calque verrouille')
        sys.exit(0)

    mode = args[0]
    rest = args[1:]

    if mode == '--svg':
        cmd_svg(rest)
    elif mode == '--gerber':
        cmd_gerber(rest)
    else:
        # Auto-detect : .svg -> gerber, sinon -> svg
        if args[0].lower().endswith('.svg'):
            cmd_gerber(args)
        else:
            cmd_svg(args)


if __name__ == '__main__':
    main()
