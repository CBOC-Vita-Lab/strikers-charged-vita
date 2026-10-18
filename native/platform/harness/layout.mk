# Compiles tests/layout_check.cpp with the game's own flags (np/Makefile): every static_assert in it is
# an offset that np_harness_game.cpp reads by number. Run from the np root:
#   make -f Makefile -f platform/harness/layout.mk TARGET=host32 layout
.PHONY: layout
layout:
	$(CXX) $(CXXFLAGS) -funsigned-char -Wno-invalid-offsetof -fsyntax-only platform/harness/tests/layout_check.cpp && echo "layout_check: every offset np_harness_game.cpp reads matches the headers ($(TARGET))"
