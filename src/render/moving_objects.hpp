#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <thread>
#include <vector>
#include <limits>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

namespace chrono = std::chrono;

namespace MovingObjects {
	const size_t OBJECT_OUTER_SECTION = 0x278, OBJECT_MIDDLE_SECTION = 0x27c, OBJECT_TEMP_OFFSET_X = 0x2c8;
	const size_t OBJECT_POSITION_DIRTY = 0x351, OBJECT_UNMODIFIED_POSITION_DIRTY = 0x352, OBJECT_RECT_DIRTY = 0x368, OBJECT_ORIENTED_BOX_DIRTY = 0x369;
	const size_t OBJECT_POSITION_X = 0x3b8, OBJECT_POSITION_Y = 0x3c0;
	const size_t OBJECT_LAST_POSITION_X = 0x4d0, OBJECT_LAST_POSITION_Y = 0x4d4, OBJECT_LAST_POSITION_COMMAND = 0x4dc, OBJECT_IS_DECORATION_2 = 0x512;
	const size_t LAYER_COMMAND_INDEX = 0x3e0, LAYER_SECTION_X_FACTOR = 0x36a0, LAYER_SECTION_Y_FACTOR = 0x36a4, LAYER_MOVED_COUNT = 0x3724;

	void prepare();

	template <class T>
	inline T read(const void* object, size_t offset) {
		T value;
		std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof(T));
		return value;
	}

	template <class T>
	inline void write(void* object, size_t offset, T value) {
		std::memcpy(static_cast<char*>(object) + offset, &value, sizeof(T));
	}

	inline bool section_index(double position, float factor, int& result) {
		if (!std::isfinite(position) || !std::isfinite(factor)) return false;
		double scaled = position <= 0. ? 0. : position >= 10000000. ? double(factor * 10000000.f)
																	: double(factor) * position;
		if (!std::isfinite(scaled) || scaled < std::numeric_limits<int>::min() || scaled > std::numeric_limits<int>::max())
			return false;
		result = (int)scaled;
		return true;
	}

	inline bool same_section(const void* layer, const void* object, double x, double y) {
		int old_section_x = read<int>(object, OBJECT_OUTER_SECTION);
		if (old_section_x < 0) return true;
		float factor_x = read<float>(layer, LAYER_SECTION_X_FACTOR), factor_y = read<float>(layer, LAYER_SECTION_Y_FACTOR);
		if (x >= 0. && x < 10000000. && y >= 0. && y < 10000000. &&
			factor_x >= 0.f && factor_x <= 200.f && factor_y >= 0.f && factor_y <= 200.f) {
			return (int)(double(factor_x) * x) == old_section_x &&
				   (int)(double(factor_y) * y) == read<int>(object, OBJECT_MIDDLE_SECTION);
		}
		int section_x, section_y;
		return section_index(x, read<float>(layer, LAYER_SECTION_X_FACTOR), section_x) &&
			   section_index(y, read<float>(layer, LAYER_SECTION_Y_FACTOR), section_y) &&
			   section_x == old_section_x && section_y == read<int>(object, OBJECT_MIDDLE_SECTION);
	}

	inline bool same_section(const void* layer, const void* object) {
		return same_section(layer, object, read<double>(object, OBJECT_POSITION_X), read<double>(object, OBJECT_POSITION_Y));
	}

	inline void prefetch_object(const void* object) {
#if defined(__SSE2__) || defined(_M_X64)
		auto* bytes = static_cast<const char*>(object);
		for (size_t offset : { OBJECT_OUTER_SECTION, OBJECT_MIDDLE_SECTION + 3, OBJECT_TEMP_OFFSET_X, OBJECT_POSITION_DIRTY, OBJECT_ORIENTED_BOX_DIRTY,
				 OBJECT_POSITION_X, OBJECT_POSITION_Y + 7, OBJECT_LAST_POSITION_X, OBJECT_LAST_POSITION_COMMAND + 3, OBJECT_IS_DECORATION_2 })
			_mm_prefetch(bytes + offset, _MM_HINT_T0);
#else
		(void)object;
#endif
	}

	const unsigned PREFETCH_AHEAD = 8;

	inline bool move_object(void* layer, void* object, double dx, double dy, bool move_x, bool move_y) {
		double x = read<double>(object, OBJECT_POSITION_X), y = read<double>(object, OBJECT_POSITION_Y);
		if (!read<bool>(object, OBJECT_IS_DECORATION_2) && read<int>(object, OBJECT_LAST_POSITION_COMMAND) != read<int>(layer, LAYER_COMMAND_INDEX)) {
			write(object, OBJECT_LAST_POSITION_X, (float)x);
			write(object, OBJECT_LAST_POSITION_Y, (float)y);
			write(object, OBJECT_LAST_POSITION_COMMAND, read<int>(layer, LAYER_COMMAND_INDEX));
			write(object, OBJECT_RECT_DIRTY, true);
			write(object, OBJECT_ORIENTED_BOX_DIRTY, true);
		}
		if (move_x && !read<bool>(object, OBJECT_TEMP_OFFSET_X)) write(object, OBJECT_POSITION_X, x += dx);
		if (move_y) write(object, OBJECT_POSITION_Y, y += dy);
		write(object, OBJECT_POSITION_DIRTY, true);
		write(object, OBJECT_UNMODIFIED_POSITION_DIRTY, true);
		return !same_section(layer, object, x, y);
	}

	inline unsigned long long group_version = 1;

#if defined(__SSE2__) || defined(_M_X64)
#define ECHO_THREADED_MOVE 1

	// threads for huge groups
	// NEVER destroyed bc joining on unload deadlocks
	class MoveThreads {
	public:
		static const unsigned HELPERS = 5;
		static const unsigned PARTS = HELPERS + 1;

		static MoveThreads& get() {
			static MoveThreads* threads = new MoveThreads();
			return *threads;
		}

		template <class Job>
		void run(Job& job) {
			task = [](void* task_data, unsigned part) {
				(*static_cast<Job*>(task_data))(part);
			};
			task_data = &job;
			float_mode = _mm_getcsr();
			parts_done.store(0, std::memory_order_relaxed);
			next_part.store(0, std::memory_order_release);
			round.fetch_add(1, std::memory_order_seq_cst);
			if (helpers_sleeping.load(std::memory_order_seq_cst) > 0) round.notify_all();
			for (unsigned part; (part = next_part.fetch_add(1, std::memory_order_acq_rel)) < PARTS;) {
				job(part);
				parts_done.fetch_add(1, std::memory_order_release);
			}
			while (parts_done.load(std::memory_order_acquire) != PARTS)
				_mm_pause();
		}

	private:
		MoveThreads() {
			for (unsigned i = 0; i < HELPERS; ++i)
				std::thread([this] {
					helper_loop();
				}).detach();
		}

		void helper_loop() {
			unsigned last_round = 0;
			for (;;) {
				auto waiting_since = chrono::steady_clock::now();
				for (unsigned spins = 0; round.load(std::memory_order_acquire) == last_round; ++spins) {
					_mm_pause();
					if ((spins & 255) != 255) continue;
					if (chrono::steady_clock::now() - waiting_since < chrono::milliseconds(2))
						continue;
					helpers_sleeping.fetch_add(1, std::memory_order_seq_cst);
					if (round.load(std::memory_order_seq_cst) == last_round)
						round.wait(last_round, std::memory_order_seq_cst);
					helpers_sleeping.fetch_sub(1, std::memory_order_seq_cst);
					waiting_since = chrono::steady_clock::now();
				}
				last_round = round.load(std::memory_order_acquire);
				for (unsigned part; (part = next_part.fetch_add(1, std::memory_order_acq_rel)) < PARTS;) {
					if (_mm_getcsr() != float_mode) _mm_setcsr(float_mode);
					task(task_data, part);
					parts_done.fetch_add(1, std::memory_order_release);
				}
			}
		}

		void (*task)(void*, unsigned) = nullptr;
		void* task_data = nullptr;
		unsigned float_mode = 0;
		std::atomic<unsigned> round{ 0 };
		std::atomic<unsigned> next_part{ 0 };
		std::atomic<unsigned> parts_done{ 0 };
		std::atomic<unsigned> helpers_sleeping{ 0 };
	};

	struct CheckedGroup {
		void* array;
		unsigned long long version;
		unsigned size;
		bool unique;
	};

	inline CheckedGroup checked_groups[64]{};

	inline unsigned movable_count(void* array, void* const* objects, unsigned size) {
		auto& checked = checked_groups[(reinterpret_cast<uintptr_t>(array) >> 4) & 63];
		if (checked.array == array && checked.version == group_version && size <= checked.size)
			return checked.unique ? size : 0;
		unsigned count = 0;
		while (count < size && objects[count])
			++count;
		std::vector<void*> sorted(objects, objects + count);
		std::sort(sorted.begin(), sorted.end());
		bool unique = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
		checked = { array, group_version, size, unique && count == size };
		return unique ? count : 0;
	}

#endif

	inline unsigned threaded_minimum = 512;

	template <class Count, class Data, class UpdateSection, class Finish>
	void move(void* layer, void* array, double dx, double dy,
		Count count, Data data, UpdateSection update_section, Finish finish) {
		unsigned array_count = count(array);
		write(layer, LAYER_MOVED_COUNT, read<unsigned>(layer, LAYER_MOVED_COUNT) + array_count);
		auto [objects, size] = data(array);
		bool move_x = dx != 0., move_y = dy != 0.;
#ifdef ECHO_THREADED_MOVE
		if (unsigned object_count = size >= threaded_minimum ? movable_count(array, reinterpret_cast<void* const*>(objects), size) : 0;
			object_count >= threaded_minimum) {
			static std::vector<unsigned char> section_changed;
			section_changed.resize(object_count);
			auto& threads = MoveThreads::get();
			const unsigned CHUNK = 128;
			std::atomic<unsigned> next_index{ 0 };
			auto move_chunks = [&](unsigned) {
				for (;;) {
					unsigned begin = next_index.fetch_add(CHUNK, std::memory_order_relaxed);
					if (begin >= object_count) return;
					unsigned end = std::min(begin + CHUNK, object_count);
					for (unsigned i = begin; i < end && i < begin + PREFETCH_AHEAD; ++i)
						prefetch_object(objects[i]);
					for (unsigned i = begin; i < end; ++i) {
						if (i + PREFETCH_AHEAD < end) prefetch_object(objects[i + PREFETCH_AHEAD]);
						section_changed[i] = move_object(layer, objects[i], dx, dy, move_x, move_y);
					}
				}
			};
			threads.run(move_chunks);
			for (unsigned i = 0; i < object_count; ++i)
				if (section_changed[i]) update_section(objects[i]);
			finish(array);
			return;
		}
#endif
		unsigned object_count = 0;
		while (object_count < size && objects[object_count])
			++object_count;
		for (unsigned i = 0; i < object_count && i < PREFETCH_AHEAD; ++i)
			prefetch_object(objects[i]);
		for (unsigned i = 0; i < object_count; ++i) {
			if (i + PREFETCH_AHEAD < object_count) prefetch_object(objects[i + PREFETCH_AHEAD]);
			if (move_object(layer, objects[i], dx, dy, move_x, move_y)) update_section(objects[i]);
		}
		finish(array);
	}
}
