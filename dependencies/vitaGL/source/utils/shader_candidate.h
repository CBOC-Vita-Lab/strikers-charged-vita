/* Unregistered candidates own their bytes and metadata until commit. */
static void discard_shader_candidate(shader *s) {
	while (s->mat) {
		matrix_uniform *next = (matrix_uniform *)s->mat->chain;
		vgl_free(s->mat);
		s->mat = next;
	}
	while (s->unif_blk) {
		block_uniform *next = (block_uniform *)s->unif_blk->chain;
		vgl_free(s->unif_blk);
		s->unif_blk = next;
	}
	vgl_free((void *)s->prog);
	s->prog = NULL;
}

static void commit_shader_candidate(shader *s, const shader *candidate) {
	s->prog = candidate->prog;
	s->id = candidate->id;
	s->size = candidate->size;
	s->unif_buf_size = candidate->unif_buf_size;
	s->mat = candidate->mat;
	s->unif_blk = candidate->unif_blk;
}
