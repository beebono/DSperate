// The crack fill's test, shared by the passes that fill and the ones that mark edges (Vulkan and GLES; keep it
// GLSL ES-clean). Two polygons whose sides nearly meet without meeting (the DS's own rounding: Pokémon Platinum's
// ground, sides 1/50 px apart) leave a sliver no polygon covers, and the pixels whose centres fall in it show what
// is behind the 3D: dots, and along a side that runs near a row or column of centres a line of them, one pixel
// wide (up to 7 long at 1x in Platinum's routes, longer above). The DS fills its rows to whole pixels and shows none.
// A crack is such a line: pixels with something undrawn, each between two full pixels across the line, up to
// `longest` of them, ending both ways in a full pixel between two full ones. One pixel is the case of all eight
// neighbours full. Not the 2x2 quad: an outline's inside corner on a texture's cut-out has its quad drawn too (5100
// such pixels filled in 13 scenes). And not a line of any length: a gap a pixel wide between two surfaces is the
// same picture at 1x (Meteos' blocks: 3292 pixels of 64 frames filled, 189 at 8, 40 at 1; Platinum's cracks all
// close at 8), so at 1x the length is 8 pixels. Not so where the line is thinner than a DS pixel, which no picture
// of the DS's has: above 1x, and under MSAA where some of the pixel's samples were drawn (crack_longest; a side
// of 14 DS pixels at 4x in Platinum). Depth tells neither apart: both lie inside one surface.
// The frame's edge counts as full: a crack in the screen's first column has one neighbour, and is filled from it.
// The includer defines crack_in(q) (on the frame), crack_full(q) (every sample drawn at alpha 31, or off the frame)
// and crack_cand(q) (on the frame, something undrawn).
int crack_longest(int scale, bool thin) { return (scale > 1 || thin) ? 32 * scale : 8; }
// The two beside a crack's pixel (crack_axis's result): one of them twice at the frame's edge.
void crack_sides(ivec2 p, int ax, out ivec2 a, out ivec2 b) {
  ivec2 s = ax == 1 ? ivec2(1, 0) : ivec2(0, 1);
  a = p - s; b = p + s;
  if (!crack_in(a)) a = b;
  if (!crack_in(b)) b = a;
}
// 0: no crack; 1: the two beside it are left and right; 2: above and below.
int crack_axis(ivec2 p, int longest) {
  for (int ax = 0; ax < 2; ++ax) {
    ivec2 d = ax == 0 ? ivec2(0, 1) : ivec2(1, 0), s = ivec2(d.y, d.x);
    if (!crack_full(p - s) || !crack_full(p + s)) continue;
    int len = 1;
    bool ok = true;
    for (int sg = -1; sg <= 1 && ok; sg += 2) {
      ok = false;
      for (int k = 1; k <= longest; ++k) {
        ivec2 q = p + sg * k * d;
        if (!crack_full(q - s) || !crack_full(q + s)) break;
        if (crack_full(q)) { ok = true; break; }
        if (!crack_cand(q) || ++len > longest) break;
      }
    }
    if (ok) return ax + 1;
  }
  return 0;
}
