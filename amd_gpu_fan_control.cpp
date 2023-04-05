#include <iostream>
#include <limits>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <csignal>

#include "satarith.hpp"

namespace fs = std::filesystem;

typedef uint8_t pwm_t;

std::string executable;

const unsigned mc_offset = 273150; // 0°mC in mK

// millicelcius to millikelvin.
unsigned mc_to_mk(signed celcius) {
	return celcius + mc_offset;
}

// millikelvin to millicelcius.
signed mk_to_mc(unsigned kelvin) {
	return kelvin - mc_offset;
}

class arguments_t {
	public:
	unsigned exit_code = 0; // If not 0, program should exit immediately using this code.

	// Temp for 0% fan and 100% fan in millicelcius.
	unsigned min_temp_kelvin;
	unsigned max_temp_kelvin;

	arguments_t(int argc, char **argv) {
		executable = std::string(argv[0]);

		if (argc != 3){
			print_usage();
			exit_code = 1;
			return;
		}

		unsigned temp_a, temp_b;
		try {
			temp_a = std::stoul(std::string(argv[1])) * 1000;
			temp_b = std::stoul(std::string(argv[2])) * 1000;
		} catch (...) {
			print_usage();
			exit_code = 1;
			return;
		}

		min_temp_kelvin = mc_to_mk(std::min(temp_a, temp_b));
		max_temp_kelvin = mc_to_mk(std::max(temp_a, temp_b));
	}

	private:
	void print_usage() {
		std::cerr << "Usage: " << executable << R"( TEMP TEMP
	TEMP must be an integer in celcius between )" << std::numeric_limits<signed>::min() << " and " << std::numeric_limits<signed>::max() << R"(.
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

	signed get_temp() {
		return read<signed>(temp_stream);
	}

	unsigned get_temp_kelvin() {
		return mc_to_mk(get_temp());
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

	/*
		Common ranges:
		temperature: roughly 270'000 to 400'000 in millikelvin.
		control: 0 to UINT_MAX (depends on your system).
		pwm: 0 to UINT8_MAX (255).
	*/

	const unsigned control_max = std::numeric_limits<unsigned>::max();

	// Max amount to rise and fall by in each loop as a portion of control_max.
	const unsigned rise_max = control_max * 0.01; // Rise quick enough to not overheat but slow enough to not go crazy for spikes.
	const unsigned fall_max = control_max * 0.001; // Fall quick enough to not be quiet once cold but slow enough to not have to spin up again after a breif lull in heat.
	// Slowing the fan for short periods of low temperature results in the fan spinning up and down frequently for varying loads which can sound irritating.

	while (true) {
		unsigned temp = hwmon->get_temp_kelvin();

		unsigned temp_range = args.max_temp_kelvin - args.min_temp_kelvin;
		unsigned temp_relative = satarith::subtract(temp, args.min_temp_kelvin); // Temperature relative to min_temp. Does not go below 0.
		const unsigned multiplier = control_max / temp_range;
		unsigned control_raw = satarith::multiply(temp_relative, multiplier); // fan control_smoothed value if not smoothed.

		// React to rise and fall at specific speed.
		// This avoids fans spinning up and down frequently while still reacting to temp rise quickly.
		static unsigned control_smoothed = control_raw;
		if (control_raw > control_smoothed) {
			// Limit rise speed.
			control_smoothed = std::min(control_raw, satarith::add(control_smoothed, rise_max));
		} else {
			// Limit fall speed.
			control_smoothed = std::max(control_raw, satarith::subtract(control_smoothed, fall_max));
		}

		// Convert control_smoothed range to pwm.
		static const pwm_t pwm_max = std::numeric_limits<pwm_t>::max();
		static const unsigned divisor = control_max / pwm_max;
		pwm_t pwm = control_smoothed / divisor;

		unsigned percentage = (pwm * 100) / pwm_max;

		std::cout << mk_to_mc(temp) / 1000 << "°C " << percentage << "%" << std::endl;

		hwmon->set_pwm(pwm);

		static const unsigned interval = 200;
		std::this_thread::sleep_for(std::chrono::milliseconds(interval));
	}
}
