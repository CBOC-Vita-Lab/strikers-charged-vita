/* Vita homebrew run under Vita3K: the console's real shader compiler over a list of GLSL texts, through vitaGL as the game calls it.
 * In:  ux0:data/mscharged/in.txt: records "#### <name> <f|v> [fastprecision]\n<text>\n".
 * Out: ux0:data/mscharged/out.bin: records u32 name length, name, u32 size, the serialized shader (vitaGL's cache format), size 0 when the compiler refused; log.txt: "script exit" at the end. */
#include <vitasdk.h>
#include <vitaGL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int _newlib_heap_size_user = 64 * 1024 * 1024;
unsigned int sceLibcHeapSize = 8 * 1024 * 1024;
#define ROOT "ux0:data/mscharged/"
int msc_vgl_printf(const char *f, ...) { return 0; }

int main(void) {
	FILE *lg = fopen(ROOT "log.txt", "w");
	vglSetSemanticBindingMode(VGL_MODE_GLOBAL);
	static const char *const tex[8] = {"v_tex0", "v_tex1", "v_tex2", "v_tex3", "v_tex4", "v_tex5", "v_tex6", "v_tex7"};
	for (int i = 0; i < 8; i++)
		vglAddSemanticBinding((const GLchar *const *)tex[i], i, VGL_TYPE_TEXCOORD);
	vglAddSemanticBinding((const GLchar *const *)"v_col0", 8, VGL_TYPE_TEXCOORD);
	vglAddSemanticBinding((const GLchar *const *)"v_col1", 9, VGL_TYPE_TEXCOORD);
	vglInitExtended(0, 960, 544, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
	FILE *in = fopen(ROOT "in.txt", "rb");
	FILE *out = fopen(ROOT "out.bin", "wb");
	if (!in || !out) { fprintf(lg, "no in.txt\nscript exit\n"); fclose(lg); sceKernelExitProcess(0); }
	fseek(in, 0, SEEK_END);
	long n = ftell(in);
	fseek(in, 0, SEEK_SET);
	char *all = malloc(n + 1);
	fread(all, 1, n, in);
	all[n] = 0;
	static char bin[256 * 1024];
	int done = 0, bad = 0;
	char *p = strstr(all, "#### ");
	while (p) {
		char *eol = strchr(p, '\n');
		char *next = strstr(eol, "\n#### ");
		if (next) *next = 0;
		*eol = 0;
		char name[128], kind[8], opt[32] = "";
		sscanf(p + 5, "%127s %7s %31s", name, kind, opt);
		vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, GL_TRUE, !strcmp(opt, "fastprecision") ? GL_TRUE : GL_FALSE, GL_TRUE);
		GLuint s = glCreateShader(kind[0] == 'v' ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
		const char *src = eol + 1;
		glShaderSource(s, 1, &src, NULL);
		glCompileShader(s);
		GLint ok = 0;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		GLsizei len = 0;
		if (ok) vglGetShaderBinary(s, sizeof bin, &len, bin);
		else {
			char info[512] = "";
			glGetShaderInfoLog(s, sizeof info, NULL, info);
			fprintf(lg, "REFUSED %s: %s\n", name, info);
			bad++;
		}
		unsigned nl = strlen(name), sz = len;
		fwrite(&nl, 4, 1, out);
		fwrite(name, 1, nl, out);
		fwrite(&sz, 4, 1, out);
		fwrite(bin, 1, sz, out);
		glDeleteShader(s);
		done++;
		p = next ? next + 1 : NULL;
		if (done % 16 == 0) { fprintf(lg, "%d done\n", done); fflush(lg); fflush(out); }
	}
	fclose(out);
	fprintf(lg, "%d compiled, %d refused\nscript exit\n", done - bad, bad);
	fclose(lg);
	sceKernelExitProcess(0);
	return 0;
}
