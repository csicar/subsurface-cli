# headless dive planner using the subsurface core (see README.md)
add_executable(planner-cli planner-cli.cpp)
target_link_libraries(planner-cli
	subsurface_backend_shared
	subsurface_corelib
	${SUBSURFACE_LINK_LIBRARIES}
)
install(TARGETS planner-cli DESTINATION bin)
