#include <wayland-server-core.h>

struct surface_state {
	struct wl_resource *buffer;
};

struct surface {
	struct surface_state *current, *pending;
	struct wl_resource *frame;
};

void surface_new(struct wl_resource *resource);
