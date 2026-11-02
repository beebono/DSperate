// Polygons and vertices for the vertex stage: Mali's GLES has no storage
// buffers there, so they come as RGBA32UI textures, 1024 texels wide, four
// texels a GpuPoly and three a GpuVert (vk_layout.h's field order).
uniform usampler2D polytex;
uniform usampler2D verttex;
uniform int u_base;   // the first instance (ES draws have no base instance)
uvec4 ds_tx(usampler2D t, uint i) { return texelFetch(t, ivec2(int(i & 1023u), int(i >> 10u)), 0); }
GpuPoly load_poly(uint i) {
  uvec4 a = ds_tx(polytex, i * 4u), b = ds_tx(polytex, i * 4u + 1u), c = ds_tx(polytex, i * 4u + 2u), d = ds_tx(polytex, i * 4u + 3u);
  GpuPoly p;
  p.first_vert = a.x; p.nverts = a.y; p.attr = a.z; p.texparam = a.w;
  p.tex_offset = b.x; p.tex_w = b.y; p.tex_h = b.z; p.flags = b.w;
  p.ytop = int(c.x); p.ybot = int(c.y); p.span_base = c.z; p.vbot = c.w;
  p.xmin = int(d.x); p.xmax = int(d.y); p.row_base = d.z; p.pad_ = d.w;
  return p;
}
GpuVert load_vert(uint i) {
  uvec4 a = ds_tx(verttex, i * 3u), b = ds_tx(verttex, i * 3u + 1u), c = ds_tx(verttex, i * 3u + 2u);
  GpuVert v;
  v.sx = int(a.x); v.sy = int(a.y); v.z = int(a.z); v.w = int(a.w);
  v.r = int(b.x); v.g = int(b.y); v.b = int(b.z); v.s = int(b.w);
  v.t = int(c.x); v.fx = int(c.y); v.fy = int(c.z); v.pad_ = c.w;
  return v;
}
// The DS depth the stage compares, as lean.vert has it: the vertex's z
// (W-buffer: its normalised w), depth-equal polygons pulled nearer.
float ds_depth(GpuVert vt, uint attr) {
  float z = clamp(float(vt.z), 0.0, 16777215.0);
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  if ((attr & 0x4000u) != 0u) z = max(z - (wbuf ? 255.0 : 512.0), 0.0);
  return z;
}
