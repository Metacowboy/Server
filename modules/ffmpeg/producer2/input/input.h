/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#pragma once

#include <common/memory/safe_ptr.h>

#include <memory>
#include <string>
#include <cstdint>

#include <boost/noncopyable.hpp>

struct AVFormatContext;
struct AVPacket;

namespace caspar {

namespace ffmpeg {

struct ffmpeg_producer_params;

}

namespace ffmpeg2 {

enum FFMPEG_Resource;

class input : boost::noncopyable
{
public:
	explicit input(
		const std::shared_ptr<ffmpeg::ffmpeg_producer_params>& params
	);

	AVStream const* video_stream() const;
	AVStream const* audio_stream() const;

	int video_stream_index() const;
	int audio_stream_index() const;

	double read_fps(double fail_value);

	std::shared_ptr<AVPacket> read_packet();
	bool eof() const;

	void loop(bool value);
	bool loop() const;

	void seek(uint32_t target);

	std::shared_ptr<AVFormatContext> format_context();

private:
	class implementation;
	std::shared_ptr<implementation> impl_;
};

	
}}
