// =====================================================================
//  Honeycomb — M5Stack Tab5 "motion-tracker" handheld case
//  Parametric, no-solder assembly (M3 screws + printed hex nut traps).
//  Printer target: Flashforge Adventurer 5X (220 x 220 x 220, enclosed).
//  Material: PETG or ASA recommended (gets handled/carried).
//
//  HOW TO USE
//    1. Open in OpenSCAD. Set PART below to the piece you want.
//    2. Check the fit dims in the TAB5 section against your actual unit
//       (especially TAB_T — bare Tab5 is ~12mm, the battery Kit ~27mm).
//    3. F6 (render), then File > Export > STL. Slice in FlashPrint/Orca.
//
//  PRINT ORIENTATION (see notes at each module)
//    back_shell : open face UP  (flat back on the bed, no supports)
//    bezel      : front face DOWN (flat, no supports)
//    grip       : foot DOWN       (tube grows upward, no supports)
//    top_handle : legs DOWN       (short bridge across the top)
// =====================================================================

PART = "all";   // "back_shell" | "bezel" | "grip" | "top_handle" | "all"

$fn = 64;

// ---- TAB5 body (VERIFY against your unit) ---------------------------
TAB_W   = 128;   // long edge (landscape)
TAB_H   = 80;    // short edge
TAB_T   = 13;    // body thickness. Battery Kit: set ~27
// Active display area (what the bezel opening must clear):
SCR_W   = 112;   // ~110.9 active width  (a touch generous)
SCR_H   = 64;    // ~62.4  active height
SCR_OFFY = 0;    // shift opening toward the bottom if camera sits up top

// ---- fit + structure -----------------------------------------------
TOL     = 0.5;   // clearance around the Tab5 (per side)
WALL    = 2.6;   // shell wall thickness
BEZ_T   = 3.2;   // bezel front thickness
LIP     = 4;     // how far the bezel overlaps the glass edge
RAD     = 7;     // outer corner radius
PORT_GAP = 9;    // depth of the open channel along the bottom edge

// ---- fasteners (M3) ------------------------------------------------
SCR_CLR = 1.75;  // M3 clearance hole radius (3.5 dia)
SCR_HEAD= 3.2;   // countersink head radius
NUT_AF  = 3.3;   // M3 nut circumradius (5.5 across-flats / 1.66)
NUT_TH  = 2.7;   // nut trap depth

// ---- grip + foot ----------------------------------------------------
GRIP_LX = 46;    // grip cross-section (long axis, oval)
GRIP_LY = 34;    // grip cross-section (short axis)
GRIP_LEN= 96;    // grip length
GRIP_WALL=3.2;
RIB_N   = 7;     // number of finger ribs
FOOT_W  = 92;
FOOT_D  = 72;
FOOT_T  = 6;

// derived
OW = TAB_W + 2*(TOL+WALL);   // shell outer width
OD = TAB_H + 2*(TOL+WALL);   // shell outer depth (screen short edge)
OH = TAB_T + WALL + 2;       // shell height (back wall + side walls)
PW = TAB_W + 2*TOL;          // pocket
PD = TAB_H + 2*TOL;

// ---------------------------------------------------------------------
module rrect(w, d, h, r) {            // rounded-corner rectangular prism
    hull() for (x=[r, w-r], y=[r, d-r]) translate([x,y,0]) cylinder(h=h, r=r);
}

module nut_trap(h) {                  // hex pocket (point-up) + screw hole
    union() {
        cylinder(h=NUT_TH, r=NUT_AF, $fn=6);
        cylinder(h=h, r=SCR_CLR);
    }
}

// corner screw positions (shared by shell + bezel)
INSET = RAD + 1;
function corners() = [[INSET,INSET],[OW-INSET,INSET],[INSET,OD-INSET],[OW-INSET,OD-INSET]];

// ---- grip mounting pad position (centred, lower-rear) ---------------
GP_W = GRIP_LX + 10;
GP_D = GRIP_LY + 10;
GP_X = OW/2;
GP_Y = OD - GP_D/2 - 2;       // toward the bottom edge
GRIP_BOLTS = [[-GRIP_LX/2+6, 0],[GRIP_LX/2-6, 0]];

// =====================================================================
//  BACK SHELL  (print open-face up)
// =====================================================================
module back_shell() {
    difference() {
        rrect(OW, OD, OH, RAD);
        // Tab5 pocket
        translate([(OW-PW)/2, (OD-PD)/2, WALL]) rrect(PW, PD, OH, RAD-WALL);
        // open port channel along the bottom edge
        translate([(OW-PW)/2+8, OD-WALL-0.1, WALL+2])
            cube([PW-16, WALL+1, OH]);
        // corner nut traps (nuts load from the back, screws come through bezel)
        for (c = corners())
            translate([c[0], c[1], -0.1]) nut_trap(OH+1);
        // grip bolt nut traps through the back wall
        for (b = GRIP_BOLTS)
            translate([GP_X+b[0], GP_Y+b[1], -0.1]) nut_trap(WALL+NUT_TH+1);
    }
    // raised grip pad on the back for a solid mating face
    translate([GP_X-GP_W/2, GP_Y-GP_D/2, 0])
        difference() {
            translate([0,0,-4]) rrect(GP_W, GP_D, 4, 5);
            for (b = GRIP_BOLTS)
                translate([GP_W/2+b[0], GP_D/2+b[1], -5]) cylinder(h=10, r=SCR_CLR);
        }
}

// =====================================================================
//  BEZEL  (print front-face down)
// =====================================================================
module bezel() {
    difference() {
        union() {
            rrect(OW, OD, BEZ_T, RAD);
            // inner lip that drops into the shell to locate the bezel
            translate([WALL+TOL, WALL+TOL, BEZ_T-0.01])
                difference() {
                    rrect(PW-0.3, PD-0.3, LIP, RAD-WALL);
                    translate([LIP,LIP,-1]) rrect(PW-0.3-2*LIP, PD-0.3-2*LIP, LIP+2, RAD-WALL);
                }
        }
        // screen opening
        translate([(OW-SCR_W)/2, (OD-SCR_H)/2 + SCR_OFFY, -1])
            rrect(SCR_W, SCR_H, BEZ_T+LIP+2, 4);
        // countersunk corner screws (heads on the front)
        for (c = corners()) translate([c[0], c[1], -0.1]) {
            cylinder(h=BEZ_T+LIP+1, r=SCR_CLR);
            cylinder(h=1.8, r1=SCR_HEAD, r2=SCR_CLR);
        }
    }
}

// =====================================================================
//  GRIP  (print foot-down)  — ribbed oval tube + angled base foot
// =====================================================================
module grip_tube() {
    difference() {
        scale([GRIP_LX/GRIP_LY, 1, 1]) cylinder(h=GRIP_LEN, r=GRIP_LY/2);
        translate([0,0,GRIP_WALL])
            scale([(GRIP_LX-2*GRIP_WALL)/(GRIP_LY-2*GRIP_WALL), 1, 1])
                cylinder(h=GRIP_LEN, r=(GRIP_LY-2*GRIP_WALL)/2);
        // finger ribs (shallow grooves)
        for (i=[1:RIB_N])
            translate([0,0, GRIP_LEN*0.18 + i*(GRIP_LEN*0.6/RIB_N)])
                rotate_extrude() translate([GRIP_LX/2-0.6,0]) circle(r=1.6);
    }
}

module grip() {
    // foot
    difference() {
        hull() {
            translate([-FOOT_W/2, -FOOT_D/2, 0]) rrect(FOOT_W, FOOT_D, FOOT_T, 6);
        }
        // lighten + screw-down holes to a belt clip / stand (optional)
        for (x=[-FOOT_W/2+12, FOOT_W/2-12])
            translate([x, FOOT_D/2-12, -1]) cylinder(h=FOOT_T+2, r=SCR_CLR);
    }
    // tube rising from the foot
    translate([0,0,FOOT_T]) grip_tube();
    // top cap with bolt nut-traps that mate to the shell grip pad
    translate([0,0,FOOT_T+GRIP_LEN-0.01])
        difference() {
            scale([GRIP_LX/GRIP_LY, 1, 1]) cylinder(h=6, r=GRIP_LY/2);
            for (b = GRIP_BOLTS)
                translate([b[0], b[1], 6-NUT_TH]) {
                    cylinder(h=NUT_TH+1, r=NUT_AF, $fn=6);
                    translate([0,0,-8]) cylinder(h=10, r=SCR_CLR);
                }
        }
}

// =====================================================================
//  TOP HANDLE  (print legs-down) — carry bar that bolts to the top edge
// =====================================================================
module top_handle() {
    span = OW*0.55; legh = 20; barr = 7;
    union() {
        for (x=[-span/2, span/2])
            translate([x,0,0]) difference() {
                cylinder(h=legh, r=barr);
                translate([0,0,-1]) cylinder(h=10, r=SCR_CLR);   // bolt down into shell
            }
        translate([-span/2, 0, legh]) rotate([0,90,0])
            cylinder(h=span, r=barr);
    }
}

// =====================================================================
if (PART=="back_shell") back_shell();
else if (PART=="bezel") bezel();
else if (PART=="grip")  grip();
else if (PART=="top_handle") top_handle();
else {
    back_shell();
    translate([0,0,40]) bezel();
    translate([OW/2, OD+GRIP_LY, 0]) grip();
    translate([OW/2, -30, 0]) top_handle();
}
