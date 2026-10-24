/* GXP fixed header and parameter layout used by vglProgramGetParameterBase.
 * Bounds-check before calling native accessors, which do not take a byte length.
 * This validates layout, not the shader instruction stream. */
static uint32_t shader_binary_word(const uint8_t *p, size_t offset) {
	uint32_t value;
	vgl_fast_memcpy(&value, p + offset, sizeof(value));
	return value;
}

static GLboolean shader_binary_valid(const void *data, size_t length, GLenum type) {
	const uint8_t *p = (const uint8_t *)data;
	if (!p || length < 156 || shader_binary_word(p, 0) != 0x00505847)
		return GL_FALSE;
	size_t size = shader_binary_word(p, 8);
	if (size < 156 || size > length || ((shader_binary_word(p, 20) & 1) != (type == GL_FRAGMENT_SHADER)))
		return GL_FALSE;
	uint32_t count = shader_binary_word(p, 36), offset = shader_binary_word(p, 40);
	if (offset > size - 40 || count > (size - 40 - offset) / 16)
		return GL_FALSE;
	size_t table = 40 + offset;
	if (count && (table < 156 || (table & 3)))
		return GL_FALSE;
	for (uint32_t i = 0; i < count; ++i) {
		size_t at = table + (size_t)i * 16;
		int32_t relative;
		vgl_fast_memcpy(&relative, p + at, sizeof(relative));
		int64_t name = (int64_t)at + relative;
		if (name < 0 || (uint64_t)name >= size || !memchr(p + name, 0, size - (size_t)name))
			return GL_FALSE;
	}
	return GL_TRUE;
}
