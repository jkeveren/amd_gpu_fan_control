#include <iostream>
#include <limits>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <csignal>

namespace fs = std::filesystem;

typedef signed int temp_t; // Short for temperature not temporary.

std::string executable;

class arguments_t {
	public:
	unsigned exit_code = 0; // If not 0, program should exit immediately using this code.

	// Temp for 0% fan and 100% fan in millicelcius.
	temp_t min_temp;
	temp_t max_temp;

	arguments_t(int argc, char **argv) {
		executable = std::string(argv[0]);

		if (argc != 3){
			print_usage();
			exit_code = 1;
			return;
		}

		temp_t temp_a, temp_b;
		try {
			temp_a = std::stoi(std::string(argv[1])) * 1000;
			temp_b = std::stoi(std::string(argv[2])) * 1000;
		} catch (...) {
			print_usage();
			exit_code = 1;
			return;
		}

		min_temp = std::min(temp_a, temp_b);
		max_temp = std::max(temp_a, temp_b);
	}

	private:
	void print_usage() {
		std::cerr << "Usage: " << executable << R"( TEMP TEMP
	TEMP must be an integer in celcius between )" << std::numeric_limits<temp_t>::min() << " and " << std::numeric_limits<temp_t>::max() << R"(.
	GPU fans will be off when GPU temp is below the lower TEMP.
	GPU fans will be full speed when GPU temp is above the higher TEMP.
	Fans will be proportionally controlled between those values.)" << std::endl;
	}
};

fs::path find_gpu_hwmon_path() {
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
			std::ifstream name_stream(name_path);
			if (!name_stream.is_open()) {
				// file does not exist. Probably not the GPU.
				continue;
			}

			std::string name;
			name_stream >> name;

			if (name == "amdgpu") {
				// Found GPU.
				return hwmon_path;
			}
		}
	}

	return "";
}

class hwmon_t {
	public:

	enum class mode_t {
		DISABLED = 0,
		MANUAL = 1,
		AUTOMATIC = 2,
	};

	typedef uint8_t pwm_t;

	bool error = false;

	mode_t original_mode;
	pwm_t original_pwm;

	std::fstream mode_stream;
	std::ifstream temp_stream;
	std::fstream pwm_stream;

	hwmon_t(fs::path path) :
		mode_stream{path / "pwm1_enable"},
		temp_stream{path / "temp2_input"},
		pwm_stream{path / "pwm1"}
	{
		if (
			mode_stream.fail() ||
			pwm_stream.fail() ||
			temp_stream.fail()
		) {
			std::cerr << "Found GPU but could not open all required sysfs files. Do you need to run this as root? Use strace for more detailed info." << std::endl;
			error = true;
			return;
		}

		original_mode = static_cast<mode_t>(read<unsigned>(mode_stream));
		original_pwm = read<unsigned>(pwm_stream);
	}

	~hwmon_t() {
		set_pwm(original_pwm);
		set_mode(original_mode);
	}

	template<typename T>
	static T read(std::basic_istream<char> &stream) {
		stream.seekg(0);
		T ret;
		stream >> ret;
		return ret;
	}

	template<typename T>
	static void write(std::basic_ostream<char> &stream, T v) {
		// stream.seekp(0);
		stream << +v; // + so it prints the decimal number instead of character represended by char code.
		stream.flush();
	}

	void set_mode(mode_t mode) {
		write(mode_stream, static_cast<unsigned>(mode));
	}

	void set_pwm(pwm_t pwm) {
		write(pwm_stream, pwm);
	}

	temp_t get_temp() {
		return read<temp_t>(temp_stream);
	}
};

hwmon_t *hwmon;

void signal_handler(int) {
	// Explicitly destruct in order to set mode back to original state
	hwmon->~hwmon_t();
	std::exit(0);
}

int main(int argc, char** argv) {
	// Parse args.
	arguments_t args = {argc, argv};
	if (args.exit_code != 0) {
		return args.exit_code;
	}
	
	// Find GPU hwmon sysfs path.
	fs::path hwmon_path = find_gpu_hwmon_path();
	if (hwmon_path == "") {
		std::cout << "Unable to find GPU in sysfs." << std::endl;
		return 1;
	}

	auto h = hwmon_t(hwmon_path);
	hwmon = &h;
	if (hwmon->error) {
		return 1;
	}

	// Set up signal handler to return pwm to original state.
	std::signal(SIGINT, signal_handler);
	std::signal(SIGTERM, signal_handler);

	hwmon->set_mode(hwmon_t::mode_t::MANUAL);

	while (true) {
		signed temp = hwmon->get_temp();

		temp_t max_pwm = std::numeric_limits<hwmon_t::pwm_t>::max();

		// std::cout << temp << " " << args.min_temp << " " << max_pwm << " " << args.max_temp << std::endl;

		temp_t range = args.max_temp - args.min_temp;
		temp_t relative = temp - args.min_temp;
		temp_t pwm = relative * (float)max_pwm / range;
		if (pwm < 0) {
			pwm = 0;
		} else if (pwm > max_pwm) {
			pwm = max_pwm;
		}

		// std::cout << temp / 1000 << "°C, " << pwm << std::endl;

		hwmon->set_pwm(pwm);

		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
}
