// misc_win.c

#include "misc_win.h"

int request_firmware(const struct firmware **p, const char *name, struct device *dummy)
{
	int ret = 0;
	HANDLE file;
	LARGE_INTEGER size;
	DWORD remain;
	struct firmware *fw;
	uint8_t *buf = NULL, *data;

	file = CreateFileA(name, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		ret = -ENOENT;
		goto fail;
	}

	if (!GetFileSizeEx(file, &size)) {
		ret = -EINVAL;
		goto fail;
	}

	if (size.HighPart) {
		ret = -EFBIG;
		goto fail;
	}

	buf = malloc(sizeof(*fw) + size.LowPart);
	if (!buf) {
		ret = -ENOMEM;
		goto fail;
	}

	fw = (struct firmware *)buf;
	data = buf + sizeof(*fw);

	remain = size.LowPart;

	while (remain) {
		DWORD read = 0;

		if (!ReadFile(file, data + (size.LowPart - remain), remain, &read, NULL)) {
			ret = -EIO;
			goto fail;
		}

		remain -= read;
	}

	fw->size = size.LowPart;
	fw->data = data;

	*p = fw;

	CloseHandle(file);

	return 0;

fail:
	if (buf)
		free(buf);

	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);

	return ret;
}

void release_firmware(const struct firmware *fw)
{
	free((void *)fw);
	return;
}
