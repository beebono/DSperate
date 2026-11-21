#version 450
// No output: the fragment stage of a pass that writes the stencil alone, for a driver that cannot build a pipeline
// without one (vk_lean.cpp, Impl::null_frag).
void main() {}
