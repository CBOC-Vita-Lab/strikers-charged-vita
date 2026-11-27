#include "shared.h"
#include "utils/glsl_utils.h"
void vglAddSemanticBinding(const GLchar *const *varying, GLint index, GLenum type) {
	strcpy(glsl_custom_bindings[glsl_custom_bindings_num].name, (const char*)varying);
	glsl_custom_bindings[glsl_custom_bindings_num].idx = index;
	glsl_custom_bindings[glsl_custom_bindings_num].type = type;
	glsl_custom_bindings[glsl_custom_bindings_num++].ref_idx = glsl_current_ref_idx;
}
// drv <glsl file> v|f: what the Vita host sets up (vglSetSemanticBindingMode, vglAddSemanticBinding, vgx_host_vita.cpp), then glsl_translator_process; prints the text the compiler would get.
int main(int c, char**v){
	static const char* t[8]={"v_tex0","v_tex1","v_tex2","v_tex3","v_tex4","v_tex5","v_tex6","v_tex7"};
	glsl_sema_mode = VGL_MODE_GLOBAL;
	for(int i=0;i<8;i++) vglAddSemanticBinding((const GLchar*const*)t[i],i,VGL_TYPE_TEXCOORD);
	vglAddSemanticBinding((const GLchar*const*)"v_col0",8,VGL_TYPE_TEXCOORD);
	vglAddSemanticBinding((const GLchar*const*)"v_col1",9,VGL_TYPE_TEXCOORD);
	FILE*f=fopen(v[1],"rb"); static char b[1<<20]; strcpy(b,"#version 100\n"); size_t n=fread(b+13,1,sizeof b-14,f); b[13+n]=0;
	shader s; memset(&s,0,sizeof s); s.type=v[2][0]=='v'?GL_VERTEX_SHADER:GL_FRAGMENT_SHADER; s.source=strdup(b); s.size=strlen(b); s.is_glsl=1;
	glsl_translator_process(&s); puts(s.source); return 0; }
