amd_gpu_fan_control: amd_gpu_fan_control.cpp
	g++ -Werror -Wall -Wextra -I./satarith/include -o $@ $<
	