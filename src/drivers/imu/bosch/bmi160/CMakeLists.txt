############################################################################
#
#   Copyright (c) 2026 PX4 Development Team. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Neither the name PX4 nor the names of its contributors may be
#    used to endorse or promote products derived from this software
#    without specific prior written permission.
#
############################################################################

px4_add_module(
	MODULE drivers__imu__bosch__bmi160
	MAIN bmi160
	SRCS
		bmi160_main.cpp
		BMI160.cpp
		BMI160.hpp
	DEPENDS
		drivers_accelerometer
		drivers_gyroscope
		px4_work_queue
	)
