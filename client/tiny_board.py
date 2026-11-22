"""A small .kicad_pcb written from a few lines of Python, for tests of a geometry no demo has
(a USB-C's doubled pads, a QFN between two parts).

    write(path, parts, outline=(x0, y0, x1, y1))
    parts: [(ref, (x, y), side, [(number, (dx, dy), (w, h), net), ...]), ...]   side "F" or "B"

Pads are SMD rectangles on the part's side (a part on "B" is written flipped, so its pad
offsets are mirrored as KiCad mirrors them). Nets are made from the names used.
"""

LAYERS = """\t(layers
\t\t(0 "F.Cu" signal)
\t\t(2 "B.Cu" signal)
\t\t(13 "F.Paste" user)
\t\t(15 "B.Paste" user)
\t\t(5 "F.SilkS" user "F.Silkscreen")
\t\t(7 "B.SilkS" user "B.Silkscreen")
\t\t(1 "F.Mask" user)
\t\t(3 "B.Mask" user)
\t\t(25 "Edge.Cuts" user)
\t\t(31 "F.CrtYd" user "F.Courtyard")
\t\t(29 "B.CrtYd" user "B.Courtyard")
\t\t(35 "F.Fab" user)
\t\t(33 "B.Fab" user)
\t)
"""


def write(path, parts, outline=(80, 80, 140, 130)):
    nets = []
    for _, _, _, pads in parts:
        for _, _, _, net in pads:
            if net and net not in nets:
                nets.append(net)
    code = {n: i + 1 for i, n in enumerate(nets)}

    out = ['(kicad_pcb\n\t(version 20250907)\n\t(generator "pcbnew")\n\t(generator_version "9.99")\n'
           '\t(general\n\t\t(thickness 1.6)\n\t\t(legacy_teardrops no)\n\t)\n\t(paper "A4")\n', LAYERS,
           '\t(setup\n\t\t(pad_to_mask_clearance 0)\n\t)\n', '\t(net 0 "")\n']
    out += [f'\t(net {code[n]} "{n}")\n' for n in nets]

    for ref, (x, y), side, pads in parts:
        cu, mask, paste, silk = (f"{side}.Cu", f"{side}.Mask", f"{side}.Paste", f"{side}.SilkS")
        out.append(f'\t(footprint "tiny:{ref}"\n\t\t(layer "{cu}")\n\t\t(at {x} {y})\n'
                   f'\t\t(property "Reference" "{ref}"\n\t\t\t(at 0 -2 0)\n\t\t\t(layer "{silk}")\n'
                   f'\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 0.5 0.5)\n\t\t\t\t\t(thickness 0.1)\n\t\t\t\t)'
                   + ('\n\t\t\t\t(justify mirror)' if side == "B" else '') + '\n\t\t\t)\n\t\t)\n'
                   f'\t\t(attr smd)\n')
        for num, (dx, dy), (w, h), net in pads:
            if side == "B":
                dx = -dx
            n = f'\n\t\t\t(net {code[net]} "{net}")' if net else ""
            out.append(f'\t\t(pad "{num}" smd rect\n\t\t\t(at {dx} {dy})\n\t\t\t(size {w} {h})\n'
                       f'\t\t\t(layers "{cu}" "{mask}" "{paste}"){n}\n\t\t)\n')
        out.append('\t)\n')

    x0, y0, x1, y1 = outline
    out.append(f'\t(gr_rect\n\t\t(start {x0} {y0})\n\t\t(end {x1} {y1})\n\t\t(stroke\n\t\t\t(width 0.1)\n'
               f'\t\t\t(type default)\n\t\t)\n\t\t(fill no)\n\t\t(layer "Edge.Cuts")\n\t)\n')
    out.append(')\n')
    open(path, "w", encoding="utf-8").write("".join(out))
