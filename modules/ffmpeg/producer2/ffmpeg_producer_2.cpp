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

#include "../stdafx.h"

#include "ffmpeg_producer.h"

#include "frame_maker.h"
#include "input/file_util.h"

#include "../ffmpeg_error.h"
#include "../ffmpeg_params.h"

#include <common/env.h>
#include <common/utility/assert.h>
#include <common/diagnostics/graph.h>


//#include <core/video_format.h>
#include <core/parameters/parameters.h>
#include <core/producer/frame_producer.h>
//#include <core/producer/frame/frame_factory.h>
#include <core/producer/frame/basic_frame.h>
//#include <core/producer/frame/frame_transform.h>
//
//#include <boost/algorithm/string.hpp>
//#include <boost/assign.hpp>
#include <boost/timer.hpp>
//#include <boost/foreach.hpp>
//#include <boost/filesystem.hpp>
//#include <boost/range/algorithm/find_if.hpp>
//#include <boost/range/algorithm/find.hpp>
//#include <boost/regex.hpp>
//
//#include <tbb/parallel_invoke.h>

#include <limits>
#include <memory>
#include <queue>

namespace caspar { namespace ffmpeg2 {
				
class ffmpeg_producer : public core::frame_producer
{
private:
	const safe_ptr<diagnostics::graph>							graph_;
					
	frame_maker													frame_maker_;	

	const std::shared_ptr<ffmpeg::ffmpeg_producer_params>				params_;

	//const double												fps_;

	
	int64_t														frame_number_;
	int64_t														file_frame_number_;
	
public:
	explicit ffmpeg_producer(
		const safe_ptr<core::frame_factory>& frame_factory,
		const std::shared_ptr<ffmpeg::ffmpeg_producer_params>& ffmpeg_producer_params
	) : frame_maker_(graph_, ffmpeg_producer_params, frame_factory),
		params_(ffmpeg_producer_params),
		//fps_(read_fps(*input_.context(), format_desc_.fps)),
		frame_number_(0),
		file_frame_number_(0)
	{
		//graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		//graph_->set_color("underflow", diagnostics::color(0.6f, 0.3f, 0.9f));	
		diagnostics::register_graph(graph_);	
	}

	// frame_producer
	
	virtual safe_ptr<core::basic_frame> receive(int hints) override
	{		
		auto frame = frame_maker_.poll();
		
		++frame_number_;
//		file_frame_number_ = frame.second;

		//graph_->set_text(print());

		return frame;
	}

	virtual safe_ptr<core::basic_frame> last_frame() const override
	{
		return core::basic_frame::empty();
	//	return disable_audio(last_frame_);
	}

	virtual uint32_t nb_frames() const override
	{
		if(params_->resource_type == ffmpeg::FFMPEG_DEVICE || params_->resource_type == ffmpeg::FFMPEG_STREAM/* || input_.loop()*/) 
		{
			return std::numeric_limits<uint32_t>::max();
		}

		uint32_t nb_frames = file_nb_frames();

		nb_frames = std::min(params_->length, nb_frames);
		//nb_frames = muxer_->calc_nb_frames(nb_frames);
		
		return nb_frames > params_->start ? nb_frames - params_->start: 0;
	}

	uint32_t file_nb_frames() const
	{
		uint32_t file_nb_frames = 0;
		return file_nb_frames;
	}
	
	virtual boost::unique_future<std::wstring> call(const std::wstring& param) override
	{
		boost::promise<std::wstring> promise;
		promise.set_value(do_call(param));
		return promise.get_future();
	}
				
	virtual std::wstring print() const override
	{
		return L"ffmpeg[" + params_->resource_name + L"|" + frame_maker_.print();
	}

	boost::property_tree::wptree info() const override
	{
		return frame_maker_.info();
	}

	std::wstring do_call(const std::wstring& param)
	{
		/* // TODO Put this back CP 2013-04
		static const boost::wregex loop_exp(L"LOOP\\s*(?<VALUE>\\d?)?", boost::regex::icase);
		static const boost::wregex seek_exp(L"SEEK\\s+(?<VALUE>\\d+)", boost::regex::icase);
		
		boost::wsmatch what;
		if(boost::regex_match(param, what, loop_exp))
		{
			if(!what["VALUE"].str().empty())
				input_.loop(boost::lexical_cast<bool>(what["VALUE"].str()));
			return boost::lexical_cast<std::wstring>(input_.loop());
		}
		if(boost::regex_match(param, what, seek_exp))
		{
			input_.seek(boost::lexical_cast<uint32_t>(what["VALUE"].str()));
			return L"";
		}
		BOOST_THROW_EXCEPTION(invalid_argument());
		*/
		return L"";
	}

};

safe_ptr<core::frame_producer> create_producer(const safe_ptr<core::frame_factory>& frame_factory, core::parameters const& params)
{
	auto ffmpeg_params = std::shared_ptr<ffmpeg::ffmpeg_producer_params>(new ffmpeg::ffmpeg_producer_params());

	// Determine the resource type from the parameters, or infer from the resource_name
	auto resource_type_str = params.at(0);
	if (resource_type_str == L"FILE")
	{
		ffmpeg_params->resource_type = ffmpeg::FFMPEG_FILE;
		ffmpeg_params->resource_name = params.at_original(1);
	} else if (resource_type_str == L"DEVICE")
	{
		ffmpeg_params->resource_type = ffmpeg::FFMPEG_DEVICE;
		ffmpeg_params->resource_name = params.at_original(1);
	} else if (resource_type_str == L"STREAM")
	{
		ffmpeg_params->resource_type = ffmpeg::FFMPEG_STREAM;
		ffmpeg_params->resource_name = params.at_original(1);
	} else
	{
		ffmpeg_params->resource_type = ffmpeg::FFMPEG_FILE;
		ffmpeg_params->resource_name = params.at_original(0);

		// Infer the resource_type from the resource_name if the resource_name looks like a URI
		auto tokens = core::parameters::protocol_split(ffmpeg_params->resource_name);
		if (tokens[0] == L"device")
		{
			ffmpeg_params->resource_type = ffmpeg::FFMPEG_DEVICE;
			ffmpeg_params->resource_name = tokens[1];
		} else if (tokens[0] == L"http" || tokens[0] == L"rtp" || tokens[0] == L"rtps")
		{
			ffmpeg_params->resource_type = ffmpeg::FFMPEG_STREAM;
		}
	}

	switch (ffmpeg_params->resource_type)
	{
	case ffmpeg::FFMPEG_FILE:
		ffmpeg_params->resource_name = probe_stem(env::media_folder() + L"\\" + ffmpeg_params->resource_name); // TODO Make probe_stem a static on input
		ffmpeg_params->loop			= params.has(L"LOOP");
		ffmpeg_params->start		= params.get(L"SEEK", static_cast<uint32_t>(0));
		break;
	case ffmpeg::FFMPEG_DEVICE:
		// Do nothing
		//resource_name = L"video=Sony Visual Communication Camera";
		//resource_name = L"video=Logitech QuickCam Easy/Cool";
		break;
	case ffmpeg::FFMPEG_STREAM:
		// Do nothing? TODO CP 2013-01
		break;
	};

	if(ffmpeg_params->resource_name.empty())
		return core::frame_producer::empty();
	
	ffmpeg_params->length		= params.get(L"LENGTH", std::numeric_limits<uint32_t>::max());
	auto filter_str	= params.get(L"FILTER", L""); 	
	boost::replace_all(filter_str, L"DEINTERLACE_BOB", L"YADIF=1:-1");
	boost::replace_all(filter_str, L"DEINTERLACE", L"YADIF=0:-1");
	//filter_str = append_filter(filter_str, L"fps=fps=50");
	ffmpeg_params->filter_str	= filter_str;
	
	return create_producer_destroy_proxy(make_safe<ffmpeg_producer>(frame_factory, ffmpeg_params));
}

}}