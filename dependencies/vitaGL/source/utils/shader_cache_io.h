#ifndef VGL_SHADER_CACHE_IO_H
#define VGL_SHADER_CACHE_IO_H

/* Called only by the GL-owning thread after successful program registration. */
static void vgl_cache_compiled_shader(shader *s, GLboolean save_bindings, const char *filename) {
	char parent[256], temporary[260];
	if (!filename || !s->prog || s->size < sizeof(uint32_t))
		return;
	size_t length = strlen(filename);
	const char *slash = strrchr(filename, '/');
	if (length >= sizeof(parent) || !slash || slash == filename || !slash[1])
		return;
	memcpy(parent, filename, slash - filename);
	parent[slash - filename] = 0;
	int n = snprintf(temporary, sizeof(temporary), "%s.tmp", filename);
	if (n < 0 || (size_t)n >= sizeof(temporary))
		return;

	/* Parents exist from initialization; only the bucket is created on a miss. */
	sceIoMkdir(parent, 0777);
	SceUID f = sceIoOpen(temporary, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
	if (f < 0)
		return;
	size_t size = 0;
	void *data = serialize_shader(NULL, &size, s, save_bindings);
	GLboolean complete = data && size >= sizeof(uint32_t) + s->size;
	for (size_t written = 0; complete && written < size;) {
		int count = sceIoWrite(f, (uint8_t *)data + written, size - written);
		if (count <= 0 || (size_t)count > size - written)
			complete = GL_FALSE;
		else
			written += count;
	}
	if (sceIoClose(f) < 0)
		complete = GL_FALSE;
	vgl_free(data);
	/* Never truncate a final entry or remove one when publication fails. */
	if (complete && sceIoRename(temporary, filename) < 0) {
		/* The Vita's rename does not replace an existing file: an entry that was just rejected as invalid would otherwise
		   stay for ever (recompiled on every start). Only a complete new entry replaces it. */
		sceIoRemove(filename);
		if (sceIoRename(temporary, filename) < 0)
			complete = GL_FALSE;
	}
	if (!complete)
		sceIoRemove(temporary);
}

#endif
