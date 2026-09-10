// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Runtime loader for libwayland-client, so the binary carries no Wayland
// link dependency: handhelds without a compositor run SDL's KMSDRM path and
// must not fail to start over a missing library. The scheme is SDL's own
// dynamic-loading pattern: every libwayland *function* we use is a function
// pointer resolved by dlsym, and a macro maps the real name onto the pointer
// BEFORE the wayland headers are included, so both the header inlines and
// the vendored protocol code (src/frontend/sdl/wl/) marshal through the
// pointers. The interface *data* symbols (wl_surface_interface, ...) are
// compiled in from wl/wayland-protocol.c -- libwayland marshals by interface
// content, not identity, so our copies are equivalent.
//
// Include this before any wayland header, always. The generated .c files get
// it force-included by CMake for the same reason.
#pragma once

#include <stdint.h>

struct wl_proxy;
struct wl_display;
struct wl_event_queue;
struct wl_interface;

#ifdef __cplusplus
namespace ds::sdl::wldyn {

// True once the library is open and every pointer below is resolved. Safe to
// call repeatedly; failure is sticky and cheap.
bool load();
const char* error();   // why load() failed, for the fallback log line

} // namespace ds::sdl::wldyn

// The pointers live outside the namespace: the macros below substitute them
// into vendored C code, which sees this header too (force-included).
extern "C" {
#endif
union wl_argument;   // forward: the prototype below must not declare it in its parameter list
extern struct wl_proxy* (*p_wl_proxy_marshal_flags)(struct wl_proxy*, uint32_t opcode, const struct wl_interface*, uint32_t version, uint32_t flags, ...);
extern struct wl_proxy* (*p_wl_proxy_marshal_array_flags)(struct wl_proxy*, uint32_t opcode, const struct wl_interface*, uint32_t version, uint32_t flags, union wl_argument*);
extern int              (*p_wl_proxy_add_listener)(struct wl_proxy*, void (**implementation)(void), void* data);
extern void             (*p_wl_proxy_destroy)(struct wl_proxy*);
extern uint32_t         (*p_wl_proxy_get_version)(struct wl_proxy*);
extern void             (*p_wl_proxy_set_queue)(struct wl_proxy*, struct wl_event_queue*);
extern struct wl_event_queue* (*p_wl_display_create_queue)(struct wl_display*);
extern void             (*p_wl_event_queue_destroy)(struct wl_event_queue*);
extern int              (*p_wl_display_roundtrip_queue)(struct wl_display*, struct wl_event_queue*);
extern int              (*p_wl_display_dispatch_queue)(struct wl_display*, struct wl_event_queue*);
extern int              (*p_wl_display_dispatch_queue_pending)(struct wl_display*, struct wl_event_queue*);
extern int              (*p_wl_display_flush)(struct wl_display*);
extern int              (*p_wl_display_get_error)(struct wl_display*);
#ifdef __cplusplus
}
#endif

// Map the real names onto the pointers for everything compiled after this
// header -- the vendored headers' static inlines and the protocol .c files.
#define wl_proxy_marshal_flags (*p_wl_proxy_marshal_flags)
#define wl_proxy_marshal_array_flags (*p_wl_proxy_marshal_array_flags)
#define wl_proxy_add_listener (*p_wl_proxy_add_listener)
#define wl_proxy_destroy (*p_wl_proxy_destroy)
#define wl_proxy_get_version (*p_wl_proxy_get_version)
#define wl_proxy_set_queue (*p_wl_proxy_set_queue)
#define wl_display_create_queue (*p_wl_display_create_queue)
#define wl_event_queue_destroy (*p_wl_event_queue_destroy)
#define wl_display_roundtrip_queue (*p_wl_display_roundtrip_queue)
#define wl_display_dispatch_queue (*p_wl_display_dispatch_queue)
#define wl_display_dispatch_queue_pending (*p_wl_display_dispatch_queue_pending)
#define wl_display_flush (*p_wl_display_flush)
#define wl_display_get_error (*p_wl_display_get_error)
