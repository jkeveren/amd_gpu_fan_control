#include <iostream>
#include <limits>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

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
		} catch (std::exception&) {
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

fs::path find_gpu_sysfs() {
	fs::directory_options options = fs::directory_options::skip_permission_denied;
	fs::recursive_directory_iterator recusive_iterator("/sys/devices", options);

	for (const fs::directory_entry &entry : recusive_iterator) {
		fs::path path = entry.path();
		std::string name = path.filename();

		if (name == "hwmon" && entry.is_directory()) {
			// Found hwmon. Check that it is for an AMD GPU.
			// This directory should have another directory inside it called hwmonN where N is the number of the hwmon.

			// Disable recursion into this directory because we will explore it manually.
			recusive_iterator.disable_recursion_pending();

			// Get hwmonN from the found hwmon directory.
			fs::directory_iterator hwmon(path, options);
			// Skip if no entries.
			if (hwmon == fs::directory_iterator()) {
				continue;
			}
			
			fs::path hwmon_path = hwmon->path();
			std::string name_path = hwmon_path / "name";
			std::ifstream name_file(name_path);
			if (!name_file.is_open()) {
				// file does not exist. Probably not the GPU.
				continue;
			}

			std::string name;
			name_file >> name;

			if (name == "amdgpu") {
				// Found GPU.
				return hwmon_path;
			}
		}
	}

	throw std::runtime_error("Unable to find GPU");
}

int main(int argc, char** argv) {
	// Usage: <executable> <0% temp> <100% temp>

	arguments_t args = {argc, argv};
	(void)args;
	
	fs::path hwmon_path = find_gpu_sysfs();

	std::cout << hwmon_path << std::endl;
}