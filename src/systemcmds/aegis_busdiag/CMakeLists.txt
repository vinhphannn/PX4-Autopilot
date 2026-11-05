px4_add_module(
	MODULE systemcmds__aegis_busdiag
	MAIN aegis_busdiag
	STACK_MAIN 4096
	SRCS aegis_busdiag.cpp
	DEPENDS px4_work_queue
)
