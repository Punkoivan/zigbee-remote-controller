import FreeCAD as App, Part, Mesh
V = App.Vector
outdir = "/home/punka/projects/esp32/remote-control/case"
doc = App.getDocument("remote_test_plate")

def box(x0, y0, z0, w, l, h):
    return Part.makeBox(w, l, h, V(x0, y0, z0))

def rbox(x0, y0, z0, w, l, h, r):
    b = box(x0, y0, z0, w, l, h)
    vert = [e for e in b.Edges if abs(e.Vertexes[0].Point.x - e.Vertexes[1].Point.x) < 1e-6
            and abs(e.Vertexes[0].Point.y - e.Vertexes[1].Point.y) < 1e-6]
    return b.makeFillet(r, vert)

def fuse_all(base, parts):
    for p in parts:
        base = base.fuse(p)
    return base

W, L = 50.0, 115.0
WALL, FLOOR = 2.0, 1.6
R_OUT, R_IN = 5.0, 1.0
PCB_W, PCB_H, PCB_T = 43.6, 40.0, 1.6
PCB_X0 = WALL + (W - 2 * WALL - PCB_W) / 2
PCB_TOP_GAP = 1.3
PCB_Y0 = L - WALL - PCB_TOP_GAP - PCB_H
PCB_Z0 = FLOOR + 2.5
SW = 6.0
ACT_TOP = PCB_Z0 + PCB_T + 5.0
ACT_ABOVE_BODY = 1.4                     # consistent with the printed cap (1.1 socket + travel) working
SW_TOP = ACT_TOP - ACT_ABOVE_BODY
ACT_D = 3.45
X0, PX = 8.3, 6.0 + 2.95
Y_TOP, PY = 3.5, 6.0 + 3.45
CAP_SOCKET_D, CAP_SOCKET_H = ACT_D + 0.25, 1.1
CAP_FLANGE_D, CAP_FLANGE_T = 8.0, 1.1
CAP_STEM_D, CAP_HOLE_D = 4.0, 4.5
CAP_PROTRUDE = 1.5
WEB_T, WEB_W = 0.2, 3.0
LID_T = 1.6
FLANGE_Z0 = ACT_TOP - CAP_SOCKET_H
LID_Z = FLANGE_Z0 + CAP_FLANGE_T + 0.1
CAP_TOP = LID_Z + LID_T + CAP_PROTRUDE
BAT_W, BAT_L, BAT_T = 40.0, 45.0, 3.0
BAT_X0, BAT_Y0 = (W - BAT_W) / 2, WALL + 0.5
X_W, X_L, X_PCB_T = 21.0, 17.8, 1.2
X_X0 = WALL + 0.8
X_Y0 = BAT_Y0 + BAT_L + 1.5
X_RAIL = 1.5
X_Z0 = FLOOR + X_RAIL
USB_W, USB_H, USB_L = 8.94, 3.26, 7.35
JST_L, JST_W, JST_H = 14.0, 6.0, 4.6
JST_X0 = W - WALL - 1.0 - JST_L - 0.6
JST_Y0 = X_Y0 + (X_L - JST_W) / 2
usb_cy, usb_cz = X_Y0 + X_L / 2, X_Z0 + X_PCB_T + USB_H / 2
keys = [(PCB_X0 + X0 + c * PX, PCB_Y0 + PCB_H - Y_TOP - r * PY) for r in range(4) for c in range(4)]
row_y = [PCB_Y0 + PCB_H - Y_TOP - r * PY for r in range(4)]

# As built (measured 2026-10-19): the tray above is printed as designed, but the keypad got glued into the
# battery bay and the battery into the keypad bay. The lid and caps follow the real layout.
# Measured from the inside walls, top view with USB-C on the right (the model's USB wall is x = 0, so the
# user's left wall is x = W - WALL and the keypad end wall is y = WALL):
#   S1  13 mm from the left wall,  7.5 mm from the end wall
#   S16 5.5 mm from the right wall / 40 mm from the left one, 36 mm from the end wall
S1_X, S16_X = W - WALL - 13.0, ((WALL + 5.5) + (W - WALL - 40.0)) / 2
B_CX, B_CY = (S1_X + S16_X) / 2, WALL + (7.5 + 36.0) / 2
keys_b = [(B_CX + (1.5 - c) * PX, B_CY + (r - 1.5) * PY) for r in range(4) for c in range(4)]
row_y_b = [B_CY + (r - 1.5) * PY for r in range(4)]
ACT_TOP_B = LID_Z - 1.4                  # measured: rim to actuator top
BAT_TOP_B = LID_Z - 2.8                  # measured: rim to battery top
# Printed caps bottomed out on the switch bodies before touching the actuators: the actuator sticks out
# less than the 1.1 mm socket depth, so the socket is 0.5 mm shallower and the cap rides on the actuator
CAP_SOCKET_H_B = CAP_SOCKET_H - 0.5
FLANGE_Z0_B = ACT_TOP_B - CAP_SOCKET_H_B
CAP_FLANGE_T_B = LID_Z - 0.1 - FLANGE_Z0_B   # flange top stays 0.1 mm under the lid
BAT_BAY_Y0 = X_Y0 + X_L + 1.2            # battery now sits between the XIAO rails and the far end wall

tray = rbox(0, 0, 0, W, L, LID_Z, R_OUT).cut(rbox(WALL, WALL, FLOOR, W - 2 * WALL, L - 2 * WALL, 50, R_IN))
snap_y = [(20, 28), (85, 93)]
for y0, y1 in snap_y:
    tray = tray.cut(box(WALL - 0.5, y0, LID_Z - 1.9, 0.51, y1 - y0, 1.0))
    tray = tray.cut(box(W - WALL - 0.01, y0, LID_Z - 1.9, 0.51, y1 - y0, 1.0))
tray = tray.cut(box(-1, usb_cy - 6, usb_cz - 3.5, WALL + 2, 12, 7))
tray = tray.cut(box(W / 2 - 3, -1, LID_Z - 1.0, 6, WALL + 2, 2))
fp = []
rib_h = 1.5
fp += [box(BAT_X0 - 1.5, BAT_Y0, FLOOR, 1.0, BAT_L, rib_h), box(BAT_X0 + BAT_W + 0.5, BAT_Y0, FLOOR, 1.0, BAT_L, rib_h)]
fp += [box(BAT_X0 - 0.5, BAT_Y0 + BAT_L + 0.5, FLOOR, 5, 0.8, rib_h), box(BAT_X0 + BAT_W - 4.5, BAT_Y0 + BAT_L + 0.5, FLOOR, 5, 0.8, rib_h)]
fp += [box(X_X0 + 1.5, X_Y0, FLOOR, X_W - 1.5, 1.0, X_RAIL), box(X_X0 + 1.5, X_Y0 + X_L - 1.0, FLOOR, X_W - 1.5, 1.0, X_RAIL)]
fp += [box(X_X0 + X_W + 0.2, X_Y0 + 2, FLOOR, 1.0, X_L - 4, X_RAIL + X_PCB_T + 1.0)]
fp += [box(X_X0 + 6, X_Y0 - 1.2, FLOOR, 8, 1.0, X_RAIL + X_PCB_T + 1.0), box(X_X0 + 6, X_Y0 + X_L + 0.2, FLOOR, 8, 1.0, X_RAIL + X_PCB_T + 1.0)]
fp += [box(JST_X0 + 2, JST_Y0 - 1.3, FLOOR, JST_L - 4, 1.0, 3.0), box(JST_X0 + 2, JST_Y0 + JST_W + 0.3, FLOOR, JST_L - 4, 1.0, 3.0)]
post_h = PCB_Z0 - FLOOR
side_ys = [PCB_Y0 + 3.0] + [(row_y[i] + row_y[i + 1]) / 2 for i in range(3)]
post_spots = []
for y in side_ys:
    post_spots.append((PCB_X0 + 2.6, y - 1, 1.8, 2))
    post_spots.append((PCB_X0 + PCB_W - 1.5, y - 1, 1.3, 2))
for xm in (PCB_X0 + X0 + PX / 2, PCB_X0 + X0 + 1.5 * PX, PCB_X0 + X0 + 2.5 * PX):
    post_spots.append((xm - 1, PCB_Y0 + 0.2, 2, 1.4))
fp += [box(x, y, FLOOR, w, l, post_h) for x, y, w, l in post_spots]
for xs in (PCB_X0 + 1.0, PCB_X0 + PCB_W - 4.0):
    fp.append(box(xs, PCB_Y0 + PCB_H + 0.15, FLOOR, 3.0, PCB_TOP_GAP - 0.15, PCB_Z0 + PCB_T - FLOOR))
tray = fuse_all(tray, fp).removeSplitter()

lid = rbox(0, 0, LID_Z, W, L, LID_T, R_OUT)
SK_T, SK_D, CLR = 1.2, 3.0, 0.15
skirt = rbox(WALL + CLR, WALL + CLR, LID_Z - SK_D, W - 2 * (WALL + CLR), L - 2 * (WALL + CLR), SK_D + 0.01, max(R_IN - CLR, 0.3))
skirt = skirt.cut(box(WALL + CLR + SK_T, WALL + CLR + SK_T, LID_Z - SK_D - 1, W - 2 * (WALL + CLR + SK_T), L - 2 * (WALL + CLR + SK_T), SK_D + 2))
skirt = skirt.cut(box(0, X_Y0 - 1.5, LID_Z - SK_D - 1, WALL + CLR + SK_T + 1, X_L + 3, SK_D + 1))
skirt = skirt.cut(box(-1, L - WALL - CLR - SK_T - 0.5, LID_Z - SK_D - 1, W + 2, SK_T + 1, SK_D + 2))
lp = [skirt]
for y0, y1 in snap_y:
    lp.append(box(WALL + CLR - 0.4, y0 + 0.5, LID_Z - 1.8, 0.41, y1 - y0 - 1, 0.8))
    lp.append(box(W - WALL - CLR - 0.01, y0 + 0.5, LID_Z - 1.8, 0.41, y1 - y0 - 1, 0.8))
# The keypad is glued, so no hold-down posts; the JST is held by its wires, so no tab
lp += [box(BAT_X0 + 5, BAT_BAY_Y0 + y, BAT_TOP_B, BAT_W - 10, 1.0, LID_Z - BAT_TOP_B + 0.01) for y in (10, 32)]
lid = fuse_all(lid, lp)
for x, y in keys_b:
    lid = lid.cut(Part.makeCylinder(CAP_HOLE_D / 2, 10, V(x, y, LID_Z - 5)))
led_x, led_y = 3.4 + 2.8, usb_cy + 4.6 + 2.0
lid = lid.cut(box(3.4, usb_cy + 4.6, LID_Z - 2, 5.6, 4.0, 2 + LID_T - 0.4))
lid = lid.cut(Part.makeCylinder(0.5, 10, V(led_x, led_y, LID_Z - 5)))
lid = lid.removeSplitter()

def cap_shape(x, y, z0):
    c = Part.makeCylinder(CAP_FLANGE_D / 2, CAP_FLANGE_T_B, V(x, y, z0)).fuse(
        Part.makeCylinder(CAP_STEM_D / 2, CAP_TOP - z0 - CAP_FLANGE_T_B, V(x, y, z0 + CAP_FLANGE_T_B)))
    ztop = CAP_TOP
    top = [e for e in c.Edges if abs(e.BoundBox.ZMin - ztop) < 1e-6 and abs(e.BoundBox.ZMax - ztop) < 1e-6]
    return c.makeFillet(0.5, top)

rows = []
for r in range(4):
    ry = row_y_b[r]
    xs = sorted(B_CX + (1.5 - c) * PX for c in range(4))
    m = fuse_all(cap_shape(xs[0], ry, FLANGE_Z0_B), [cap_shape(x, ry, FLANGE_Z0_B) for x in xs[1:]])
    m = m.fuse(box(xs[0], ry - WEB_W / 2, FLANGE_Z0_B, 3 * PX, WEB_W, WEB_T))
    for x in xs:
        m = m.cut(Part.makeCylinder(CAP_SOCKET_D / 2, CAP_SOCKET_H_B, V(x, ry, FLANGE_Z0_B)))
    rows.append(m.removeSplitter())
all_caps = fuse_all(rows[0], rows[1:])

SW_TOP_B, PCB_TOP_B = ACT_TOP_B - ACT_ABOVE_BODY, ACT_TOP_B - 5.0
dummies = {
    "pcb": box(WALL + 1.2, B_CY - 1.5 * PY - 7.5, PCB_TOP_B - PCB_T, PCB_W, PCB_H, PCB_T),
    "switches": fuse_all(box(0, 0, -50, 1, 1, 1), [box(x - SW / 2, y - SW / 2, PCB_TOP_B, SW, SW, SW_TOP_B - PCB_TOP_B) for x, y in keys_b]),
    "actuators": fuse_all(box(0, 0, -50, 1, 1, 1), [Part.makeCylinder(ACT_D / 2, ACT_ABOVE_BODY, V(x, y, SW_TOP_B)) for x, y in keys_b]),
    "battery": box(BAT_X0, BAT_BAY_Y0, BAT_TOP_B - BAT_T, BAT_W, L - WALL - BAT_BAY_Y0, BAT_T),
    "xiao": box(X_X0, X_Y0, X_Z0, X_W, X_L, X_PCB_T).fuse(box(WALL, usb_cy - USB_W / 2, X_Z0 + X_PCB_T, USB_L, USB_W, USB_H)),
    "jst": box(JST_X0, JST_Y0, FLOOR, JST_L, JST_W, JST_H),
}
report = {n: (round(tray.common(d).Volume, 3), round(lid.common(d).Volume, 3), round(all_caps.common(d).Volume, 3)) for n, d in dummies.items()}
report["tray_x_lid"] = round(tray.common(lid).Volume, 3)
report["caps_x_lid"] = round(all_caps.common(lid).Volume, 3)
report["caps_x_tray"] = round(all_caps.common(tray).Volume, 3)
pressed = all_caps.copy(); pressed.translate(V(0, 0, -0.25))
report["pressed_x_switch_bodies"] = round(pressed.common(dummies["switches"]).Volume, 3)
report["pressed_x_lid"] = round(pressed.common(lid).Volume, 3)
report["pressed_x_tray"] = round(pressed.common(tray).Volume, 3)

for n in ("Tray", "Lid", "Caps"):
    if doc.getObject(n):
        doc.removeObject(n)
t = doc.addObject("Part::Feature", "Tray"); t.Shape = tray
l = doc.addObject("Part::Feature", "Lid"); l.Shape = lid
c = doc.addObject("Part::Feature", "Caps"); c.Shape = all_caps
for o in doc.Objects:
    o.Visibility = False
doc.recompute()

def export(shape, path, flip):
    s = shape.copy()
    if flip:
        s.rotate(V(0, 0, 0), V(1, 0, 0), 180)
    bb = s.BoundBox
    s.translate(V(-bb.XMin, -bb.YMin, -bb.ZMin))
    m = Mesh.Mesh(); m.addFacets(s.tessellate(0.01)); m.write(path)

export(tray, outdir + "/case_tray.3mf", False)
export(lid, outdir + "/case_lid.3mf", True)
export(rows[0], outdir + "/keycap_row4.3mf", False)
import os
if os.path.exists(outdir + "/keycaps.3mf"):
    os.remove(outdir + "/keycaps.3mf")
doc.save()
print(report)
print("body height", round(LID_Z + LID_T, 2), "with caps", round(CAP_TOP, 2))
print("valid", tray.isValid(), lid.isValid(), all_caps.isValid())