#define _POSIX_C_SOURCE 200809L
#include <backend/input.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libudev.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>

struct input {
	int key_fd;
	int keymap_fd;
	unsigned int keymap_size;

	struct xkb_context *context;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
};

struct key_dev {
	char *devnode;
	char *name;
};

struct key_dev *find_keyboard_devices(int *count) {
	struct udev *udev = udev_new();
	struct udev_enumerate *enu = udev_enumerate_new(udev);
	udev_enumerate_add_match_property(enu, "ID_INPUT_KEYBOARD", "1");
	udev_enumerate_add_match_sysname(enu, "event*");
	udev_enumerate_scan_devices(enu);
	struct udev_list_entry *cur;
	*count = 0;
	udev_list_entry_foreach(cur, udev_enumerate_get_list_entry(enu)) {
		(*count)++;
	}
	struct key_dev *key_devs = calloc(*count, sizeof(struct key_dev));
	int i=0;
	udev_list_entry_foreach(cur, udev_enumerate_get_list_entry(enu)) {
		struct udev_device *dev = udev_device_new_from_syspath(udev,
		udev_list_entry_get_name(cur));
		const char *devnode = udev_device_get_devnode(dev);
		key_devs[i].devnode = malloc((strlen(devnode)+1)*sizeof(char));
		strcpy(key_devs[i].devnode, devnode);
		i++;
	}
	return key_devs;
}

void free_keyboard_devices(struct key_dev *key_devs, int count) {
	for (int i=0; i<count; i++) {
		free(key_devs[i].devnode);
	}
	free(key_devs);
}

int create_file(off_t size) {
	static const char template[] = "/compositor-XXXXXX";
	const char *path;
	char *name;
	int ret;

	path = getenv("XDG_RUNTIME_DIR");
	if (!path) {
		errno = ENOENT;
		return -1;
	}

	name = malloc(strlen(path) + sizeof(template));
	if (!name)
		return -1;

	strcpy(name, path);
	strcat(name, template);

	int fd = mkstemp(name);
	if (fd >= 0) {
		long flags = fcntl(fd, F_GETFD);
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		unlink(name);
	}

	free(name);

	if (fd < 0)
		return -1;

	do {
		ret = posix_fallocate(fd, 0, size);
	} while (ret == EINTR);
	if (ret != 0) {
		close(fd);
		errno = ret;
		return -1;
	}
	
	return fd;
}

struct input *input_setup() {
	int count;
	struct key_dev *key_devs = find_keyboard_devices(&count);
	int n;
	if (count > 1) {
		printf("Found multiple keyboards:\n");
		for (int i=0; i<count; i++) {
			printf("(%d) [%s]\n", i, key_devs[i].devnode);
		}
		printf("Choose one: ");
		scanf("%d", &n);
	} else {
		// Handle count == 0
		n = 0;
	}

	struct input *S = calloc(1, sizeof(struct input));
	S->key_fd = open(key_devs[n].devnode, O_RDONLY|O_CLOEXEC|O_NONBLOCK);
	if (S->key_fd < 0) {
		perror("open");
		return 0;
	}
	free_keyboard_devices(key_devs, count);

	ioctl(S->key_fd, EVIOCGRAB, 1);

	S->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!S->context) {
		printf("Cannot create XKB context\n");
	}
	
	struct xkb_rule_names rules = {
		getenv("XKB_DEFAULT_RULES"),
		getenv("XKB_DEFAULT_MODEL"),
		getenv("XKB_DEFAULT_LAYOUT"),
		getenv("XKB_DEFAULT_VARIANT"),
		getenv("XKB_DEFAULT_OPTIONS")
	};

	S->keymap = xkb_map_new_from_names(S->context, &rules,
	XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (S->keymap == NULL) {
		xkb_context_unref(S->context);
		printf("Cannot create XKB keymap\n");
		return 0;
	}

	char *keymap_str = NULL;
	keymap_str = xkb_keymap_get_as_string(S->keymap,
	XKB_KEYMAP_FORMAT_TEXT_V1);
	S->keymap_size = strlen(keymap_str) + 1;
	S->keymap_fd = create_file(S->keymap_size);
	if (S->keymap_fd < 0) {
		printf("creating a keymap file for %u bytes failed\n", S->keymap_size);
		return 0;
	}
	void *ptr = mmap(NULL, S->keymap_size,
		PROT_READ | PROT_WRITE, MAP_SHARED, S->keymap_fd, 0);
	if (ptr == (void*)-1) {
		printf("failed to mmap() %u bytes", S->keymap_size);
		return 0;
	}
	strcpy(ptr, keymap_str);
	free(keymap_str);
	
	S->state = xkb_state_new(S->keymap);

	return S;
}

int input_get_key_fd(struct input *S) {
	return S->key_fd;
}

unsigned int input_get_keymap_fd(struct input *S) {
	return S->keymap_fd;
}

unsigned int input_get_keymap_size(struct input *S) {
	return S->keymap_size;
}

_Bool input_handle_event(struct input *S, struct aaa *aaa) {
	struct input_event ev;
	read(S->key_fd, &ev, sizeof(struct input_event));

	if (ev.type == EV_KEY) {
		aaa->key = ev.code;
		aaa->state = ev.value > 0 ? 1 : 0;
		xkb_keycode_t keycode = aaa->key + 8;
		enum xkb_key_direction direction = aaa->state ? XKB_KEY_DOWN : XKB_KEY_UP;
		xkb_state_update_key(S->state, keycode, direction);

		aaa->mods_depressed = xkb_state_serialize_mods(S->state,
		XKB_STATE_MODS_DEPRESSED);
		aaa->mods_latched = xkb_state_serialize_mods(S->state,
		XKB_STATE_MODS_LATCHED);
		aaa->mods_locked = xkb_state_serialize_mods(S->state,
		XKB_STATE_MODS_LOCKED);
		aaa->group = xkb_state_serialize_mods(S->state,
		XKB_STATE_LAYOUT_EFFECTIVE);

		return 1;
	} else
		return 0;
}

void input_release(struct input *S) {
	xkb_state_unref(S->state);
	xkb_keymap_unref(S->keymap);
	xkb_context_unref(S->context);
	close(S->key_fd);
	free(S);
}
