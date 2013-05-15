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
* Author: Cambell Prince, cambell.prince@gmail.com
*/

#pragma once

#include <common/memory/safe_ptr.h>
#include <core/mixer/audio/audio_mixer.h>

#include <memory>
#include <string>
#include <cstdint>

#include <boost/noncopyable.hpp>

namespace caspar {

namespace core {

class basic_frame;
class write_frame;
struct frame_factory;

}

namespace diagnostics {

class graph;

}
	 
namespace ffmpeg {

struct ffmpeg_producer_params;

}

namespace ffmpeg2 {

static const int CASPAR_PIX_FMT_LUMA = 10; // Just hijack some unual pixel format.

class frame_maker : boost::noncopyable
{
public:
	explicit frame_maker(
		const safe_ptr<diagnostics::graph>& graph,
		const std::shared_ptr<ffmpeg::ffmpeg_producer_params>& params,
		const safe_ptr<core::frame_factory>& frame_factory
	);

	safe_ptr<caspar::core::basic_frame> poll();
	
	boost::property_tree::wptree info() const;
	std::wstring print() const;
	
	static std::shared_ptr<core::audio_buffer> flush_audio();
	static std::shared_ptr<core::audio_buffer> empty_audio();
	static std::shared_ptr<AVFrame> flush_video();
	static std::shared_ptr<AVFrame> empty_video();

private:
	class implementation;
	std::shared_ptr<implementation> impl_;
};

	
}}
