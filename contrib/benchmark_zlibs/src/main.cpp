/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Copyright (C) 2023-2023  The DOSBox Staging Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <array>
#include <chrono>
#include <cstdio>

#include <zlib.h>

constexpr auto one_megabyte = 1024 * 1024;

using data_array_t = std::array<Bytef, one_megabyte>;

static data_array_t generate_data_in()
{
	data_array_t data = {};
	for (size_t i = 0; i < data.size(); ++i) {
		data[i] = static_cast<Bytef>(i % 256);
	}
	return data;
}

int main()
{
	static data_array_t data_in  = generate_data_in();
	static data_array_t data_out = {};

	// Initialize the stream
	z_stream stream = {};
	if (const auto rcode = deflateInit(&stream, Z_DEFAULT_COMPRESSION);
	    rcode != Z_OK) {
		printf("deflateInit failed\n");
		return 1;
	}

	// Configure the stream
	stream.avail_in  = data_in.size();
	stream.next_in   = data_in.data();
	stream.avail_out = data_out.size();
	stream.next_out  = data_out.data();

	// Compress the data
	const auto start = std::chrono::high_resolution_clock::now();
	if (const auto rcode = deflate(&stream, Z_FINISH); rcode != Z_STREAM_END) {
		printf("deflate failed\n");
		return 1;
	}
	if (const auto rcode = deflateEnd(&stream); rcode != Z_OK) {
		printf("deflateEnd failed\n");
		return 1;
	}
	const auto end = std::chrono::high_resolution_clock::now();

	// Calculate and print compression speed in MB/s
	using namespace std::chrono;
	const auto duration_us = duration_cast<microseconds>(end - start);
	const auto duration_s  = duration_us.count() / 1000000.0;
	const auto speed_mb_s  = static_cast<double>(data_in.size()) /
	                        one_megabyte / duration_s;

	printf("%.2f MB/s\n", speed_mb_s);

	// OK!
	return 0;
}
