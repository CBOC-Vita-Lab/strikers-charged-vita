MOVIE_TEST_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
include $(MOVIE_TEST_ROOT)/../../native/platform/sdk/Makefile

$(B)/tests/test_thp_skip: $(MOVIE_TEST_ROOT)/thp_skip.cpp $(LIB) $(OBJ)/gen/thp/THPSimple.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(NP) -fpermissive -DTHP_PLAYER_IMPLEMENTATION='"$(OBJ)/gen/thp/THPSimple.cpp"' -c $< -o $@.o
	$(LD) $(ARCH) $@.o $(LIB) $(LIBS) -o $@
