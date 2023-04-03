#include <iostream>
#include <limits>

typedef signed int temp_t; // Short for temperature not temporary.

std::string executable;

class arguments_t {
	public:
	unsigned exit_code; // If not 0, program should exit immediately using this code.

	temp_t min;
	temp_t max;

	arguments_t(int argc, char **argv) {
		executable = std::string(argv[0]);

		if (argc != 3){
			print_usage();
			exit_code = 1;
			return;
		}

		temp_t temp_a, temp_b;
		try {
			temp_a = std::stoi(std::string(argv[1]));
			temp_b = std::stoi(std::string(argv[2]));
		} catch (std::exception) {
			print_usage();
			exit_code = 1;
			return;
		}

		min = std::min(temp_a, temp_b);
		max = std::max(temp_a, temp_b);
	}

	private:
	void print_usage() {
		std::cerr << "Usage: " << executable << R"( TEMP TEMP
	TEMP must be an integer in millicelcius between )" << std::numeric_limits<temp_t>::min() << " and " << std::numeric_limits<temp_t>::max() << R"(.
	GPU fans will be off when GPU temp is below the lower TEMP.
	GPU fans will be full speed when GPU temp is above the higher TEMP.
	Fans will be proportionally controlled between those values.)" << std::endl;
	}
};

int main(int argc, char** argv) {
	// Usage: <executable> <0% temp> <100% temp>

	arguments_t args = {argc, argv};

	std::cout << args.min << std::endl << args.max << std::endl;
}