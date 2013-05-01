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

#include "../../StdAfx.h"

#include "frame_muxer.h"

#include "../filter/filter.h"
#include "../frame_maker.h"

#include <core/producer/frame_producer.h>
#include <core/producer/frame/basic_frame.h>
#include <core/producer/frame/frame_transform.h>
#include <core/producer/frame/pixel_format.h>
#include <core/producer/frame/frame_factory.h>
#include <core/mixer/write_frame.h>

#include <common/env.h>
#include <common/exception/exceptions.h>
#include <common/log/log.h>
#include <common/memory/memcpy.h>

#include <tbb/concurrent_unordered_map.h>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

#include <boost/foreach.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <deque>
#include <queue>
#include <vector>

using namespace caspar::core;

namespace caspar { namespace ffmpeg2 {
	
struct frame_muxer::implementation : boost::noncopyable
{	
	std::queue<std::queue<safe_ptr<write_frame>>>	video_streams_;
	std::queue<core::audio_buffer>					audio_streams_;
	std::queue<safe_ptr<basic_frame>>				frame_buffer_;
	display_mode::type								display_mode_;
	const double									in_fps_;
	const video_format_desc							format_desc_;
	bool											auto_transcode_;
	bool											auto_deinterlace_;
	
	std::vector<size_t>								audio_cadence_;
			
	safe_ptr<core::frame_factory>					frame_factory_;
	
	filter											filter_;
	const std::wstring								filter_str_;
	bool											force_deinterlacing_;
		
	implementation(double in_fps, const safe_ptr<core::frame_factory>& frame_factory, const std::wstring& filter_str)
		: display_mode_(display_mode::invalid)
		, in_fps_(in_fps)
		, format_desc_(frame_factory->get_video_format_desc())
		, auto_transcode_(env::properties().get(L"configuration.auto-transcode", true))
		, auto_deinterlace_(env::properties().get(L"configuration.auto-deinterlace", true))
		, audio_cadence_(format_desc_.audio_cadence)
		, frame_factory_(frame_factory)
		, filter_str_(filter_str)
		, force_deinterlacing_(false)
	{
		video_streams_.push(std::queue<safe_ptr<write_frame>>());
		audio_streams_.push(core::audio_buffer());
		
		// Note: Uses 1 step rotated cadence for 1001 modes (1602, 1602, 1601, 1602, 1601)
		// This cadence fills the audio mixer most optimally.
		boost::range::rotate(audio_cadence_, std::end(audio_cadence_)-1);
	}

	std::wstring print_mode(size_t width, size_t height, double fps, bool interlaced)
	{
		std::wostringstream fps_ss;
		fps_ss << std::fixed << std::setprecision(2) << (!interlaced ? fps : 2.0 * fps);

		return boost::lexical_cast<std::wstring>(width) + L"x" + boost::lexical_cast<std::wstring>(height) + (!interlaced ? L"p" : L"i") + fps_ss.str();
	}

	void push(const std::shared_ptr<AVFrame>& video_frame, int hints)
	{		
		if(!video_frame)
			return;
		
		if(video_frame == frame_maker::flush_video())
		{	
			video_streams_.push(std::queue<safe_ptr<write_frame>>());
		}
		else if(video_frame == frame_maker::empty_video())
		{
			video_streams_.back().push(make_safe<core::write_frame>(this));
			display_mode_ = display_mode::simple;
		}
		else
		{
			bool deinterlace_hint = (hints & core::frame_producer::DEINTERLACE_HINT) != 0;
		
			if(auto_deinterlace_ && force_deinterlacing_ != deinterlace_hint)
			{
				force_deinterlacing_ = deinterlace_hint;
				display_mode_ = display_mode::invalid;
			}

			if(display_mode_ == display_mode::invalid)
				update_display_mode(video_frame, force_deinterlacing_);
				
			if(hints & core::frame_producer::ALPHA_HINT)
				video_frame->format = make_alpha_format(video_frame->format);
		
			auto format = video_frame->format;
			if(video_frame->format == CASPAR_PIX_FMT_LUMA) // CASPAR_PIX_FMT_LUMA is not valid for filter, change it to GRAY8
				video_frame->format = PIX_FMT_GRAY8;

			filter_.push(video_frame);
			BOOST_FOREACH(auto& av_frame, filter_.poll_all())
			{
				if(video_frame->format == PIX_FMT_GRAY8 && format == CASPAR_PIX_FMT_LUMA)
					av_frame->format = format;

				video_streams_.back().push(make_write_frame(this, av_frame, frame_factory_, hints));
			}
		}

		if(video_streams_.back().size() > 32)
			BOOST_THROW_EXCEPTION(invalid_operation() << source_info("frame_muxer") << msg_info("video-stream overflow. This can be caused by incorrect frame-rate. Check clip meta-data."));
	}

	void push(const std::shared_ptr<core::audio_buffer>& audio)
	{
		if(!audio)	
			return;

		if(audio == frame_maker::flush_audio())
		{
			audio_streams_.push(core::audio_buffer());
		}
		else if(audio == frame_maker::empty_audio())
		{
			boost::range::push_back(audio_streams_.back(), core::audio_buffer(audio_cadence_.front(), 0));
		}
		else
		{
			boost::range::push_back(audio_streams_.back(), *audio);
		}

		if(audio_streams_.back().size() > 32*audio_cadence_.front())
			BOOST_THROW_EXCEPTION(invalid_operation() << source_info("frame_muxer") << msg_info("audio-stream overflow. This can be caused by incorrect frame-rate. Check clip meta-data."));
	}
	
	bool video_ready() const
	{		
		return video_streams_.size() > 1 || (video_streams_.size() >= audio_streams_.size() && video_ready2());
	}
	
	bool audio_ready() const
	{
		return audio_streams_.size() > 1 || (audio_streams_.size() >= video_streams_.size() && audio_ready2());
	}

	bool video_ready2() const
	{		
		switch(display_mode_)
		{
		case display_mode::deinterlace_bob_reinterlace:					
		case display_mode::interlace:	
		case display_mode::half:
			return video_streams_.front().size() >= 2;
		default:										
			return video_streams_.front().size() >= 1;
		}
	}
	
	bool audio_ready2() const
	{
		switch(display_mode_)
		{
		case display_mode::duplicate:					
			return audio_streams_.front().size()/2 >= audio_cadence_.front();
		default:										
			return audio_streams_.front().size() >= audio_cadence_.front();
		}
	}
		
	std::shared_ptr<basic_frame> poll()
	{
		if(!frame_buffer_.empty())
		{
			auto frame = frame_buffer_.front();
			frame_buffer_.pop();	
			return frame;
		}

		if(video_streams_.size() > 1 && audio_streams_.size() > 1 && (!video_ready2() || !audio_ready2()))
		{
			if(!video_streams_.front().empty() || !audio_streams_.front().empty())
				CASPAR_LOG(trace) << "Truncating: " << video_streams_.front().size() << L" video-frames, " << audio_streams_.front().size() << L" audio-samples.";

			video_streams_.pop();
			audio_streams_.pop();
		}

		if(!video_ready2() || !audio_ready2() || display_mode_ == display_mode::invalid)
			return nullptr;
				
		auto frame1				= pop_video();
		frame1->audio_data()	= pop_audio();

		switch(display_mode_)
		{
		case display_mode::simple:						
		case display_mode::deinterlace_bob:				
		case display_mode::deinterlace:	
			{
				frame_buffer_.push(frame1);
				break;
			}
		case display_mode::interlace:					
		case display_mode::deinterlace_bob_reinterlace:	
			{				
				auto frame2 = pop_video();

				frame_buffer_.push(core::basic_frame::interlace(frame1, frame2, format_desc_.field_mode));	
				break;
			}
		case display_mode::duplicate:	
			{
				auto frame2				= make_safe<core::write_frame>(*frame1);
				frame2->audio_data()	= pop_audio();

				frame_buffer_.push(frame1);
				frame_buffer_.push(frame2);
				break;
			}
		case display_mode::half:	
			{				
				pop_video(); // Throw away

				frame_buffer_.push(frame1);
				break;
			}
		}
		
		return frame_buffer_.empty() ? nullptr : poll();
	}
	
	safe_ptr<core::write_frame> pop_video()
	{
		auto frame = video_streams_.front().front();
		video_streams_.front().pop();		
		return frame;
	}

	core::audio_buffer pop_audio()
	{
		CASPAR_VERIFY(audio_streams_.front().size() >= audio_cadence_.front());

		auto begin = audio_streams_.front().begin();
		auto end   = begin + audio_cadence_.front();

		core::audio_buffer samples(begin, end);
		audio_streams_.front().erase(begin, end);
		
		boost::range::rotate(audio_cadence_, std::begin(audio_cadence_)+1);

		return samples;
	}
				
	void update_display_mode(const std::shared_ptr<AVFrame>& frame, bool force_deinterlace)
	{
		std::wstring filter_str = filter_str_;

		display_mode_ = display_mode::simple;
		if(auto_transcode_)
		{
			auto mode = get_mode(*frame);
			auto fps  = in_fps_;

			if(filter::is_deinterlacing(filter_str_))
				mode = core::field_mode::progressive;

			if(filter::is_double_rate(filter_str_))
				fps *= 2;
			
			display_mode_ = get_display_mode(mode, fps, format_desc_.field_mode, format_desc_.fps);
			
			if((frame->height != 480 || format_desc_.height != 486) && // don't deinterlace for NTSC DV
					display_mode_ == display_mode::simple && mode != core::field_mode::progressive && format_desc_.field_mode != core::field_mode::progressive && 
					frame->height != static_cast<int>(format_desc_.height))
			{
				display_mode_ = display_mode::deinterlace_bob_reinterlace; // The frame will most likely be scaled, we need to deinterlace->reinterlace	
			}

			if(force_deinterlace && mode != core::field_mode::progressive && 
			   display_mode_ != display_mode::deinterlace && 
			   display_mode_ != display_mode::deinterlace_bob && 
			   display_mode_ != display_mode::deinterlace_bob_reinterlace)			
			{	
				CASPAR_LOG(info) << L"[frame_muxer] Automatically started non bob-deinterlacing. Consider starting producer with bob-deinterlacing (FILTER DEINTERLACE_BOB) for smoothest playback.";
				display_mode_ = display_mode::deinterlace;
			}

			if(display_mode_ == display_mode::deinterlace)
				filter_str = append_filter(filter_str, L"YADIF=0:-1");
			else if(display_mode_ == display_mode::deinterlace_bob || display_mode_ == display_mode::deinterlace_bob_reinterlace)
				filter_str = append_filter(filter_str, L"YADIF=1:-1");
		}

		if(display_mode_ == display_mode::invalid)
		{
			CASPAR_LOG(warning) << L"[frame_muxer] Auto-transcode: Failed to detect display-mode.";
			display_mode_ = display_mode::simple;
		}
			
		if(!boost::iequals(filter_.filter_str(), filter_str))
		{
			for(int n = 0; n < filter_.delay(); ++n)
			{
				filter_.push(frame);
				auto av_frame = filter_.poll();
				if(av_frame)							
					video_streams_.back().push(make_write_frame(this, make_safe_ptr(av_frame), frame_factory_, 0));
			}
			filter_ = filter(filter_str);
			CASPAR_LOG(info) << L"[frame_muxer] " << display_mode::print(display_mode_) << L" " << print_mode(frame->width, frame->height, in_fps_, frame->interlaced_frame > 0);
		}
	}
	
	uint32_t calc_nb_frames(uint32_t nb_frames) const
	{
		uint64_t nb_frames2 = nb_frames;
		
		if(filter_.is_double_rate()) // Take into account transformations in filter.
			nb_frames2 *= 2;

		switch(display_mode_) // Take into account transformation in run.
		{
		case display_mode::deinterlace_bob_reinterlace:
		case display_mode::interlace:	
		case display_mode::half:
			nb_frames2 /= 2;
			break;
		case display_mode::duplicate:
			nb_frames2 *= 2;
			break;
		}

		return static_cast<uint32_t>(nb_frames2);
	}

	core::field_mode::type get_mode(const AVFrame& frame)
	{
		if(!frame.interlaced_frame)
			return core::field_mode::progressive;

		return frame.top_field_first ? core::field_mode::upper : core::field_mode::lower;
	}

	core::pixel_format::type get_pixel_format(PixelFormat pix_fmt)
	{
		switch(pix_fmt)
		{
		case CASPAR_PIX_FMT_LUMA:	return core::pixel_format::luma;
		case PIX_FMT_GRAY8:			return core::pixel_format::gray;
		case PIX_FMT_BGRA:			return core::pixel_format::bgra;
		case PIX_FMT_ARGB:			return core::pixel_format::argb;
		case PIX_FMT_RGBA:			return core::pixel_format::rgba;
		case PIX_FMT_ABGR:			return core::pixel_format::abgr;
		case PIX_FMT_YUV444P:		return core::pixel_format::ycbcr;
		case PIX_FMT_YUV422P:		return core::pixel_format::ycbcr;
		case PIX_FMT_YUV420P:		return core::pixel_format::ycbcr;
		case PIX_FMT_YUV411P:		return core::pixel_format::ycbcr;
		case PIX_FMT_YUV410P:		return core::pixel_format::ycbcr;
		case PIX_FMT_YUVA420P:		return core::pixel_format::ycbcra;
		default:					return core::pixel_format::invalid;
		}
	}

	core::pixel_format_desc get_pixel_format_desc(PixelFormat pix_fmt, size_t width, size_t height)
	{
		// Get linesizes
		AVPicture dummy_pict;	
		avpicture_fill(&dummy_pict, nullptr, pix_fmt == CASPAR_PIX_FMT_LUMA ? PIX_FMT_GRAY8 : pix_fmt, width, height);

		core::pixel_format_desc desc;
		desc.pix_fmt = get_pixel_format(pix_fmt);
		
		switch(desc.pix_fmt)
		{
		case core::pixel_format::gray:
		case core::pixel_format::luma:
			{
				desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[0], height, 1));						
				return desc;
			}
		case core::pixel_format::bgra:
		case core::pixel_format::argb:
		case core::pixel_format::rgba:
		case core::pixel_format::abgr:
			{
				desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[0]/4, height, 4));						
				return desc;
			}
		case core::pixel_format::ycbcr:
		case core::pixel_format::ycbcra:
			{		
				// Find chroma height
				size_t size2 = dummy_pict.data[2] - dummy_pict.data[1];
				size_t h2 = size2/dummy_pict.linesize[1];			

				desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[0], height, 1));
				desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[1], h2, 1));
				desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[2], h2, 1));

				if(desc.pix_fmt == core::pixel_format::ycbcra)						
					desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[3], height, 1));	
				return desc;
			}		
		default:		
			desc.pix_fmt = core::pixel_format::invalid;
			return desc;
		}
	}

	int make_alpha_format(int format)
	{
		switch(get_pixel_format(static_cast<PixelFormat>(format)))
		{
		case core::pixel_format::ycbcr:
		case core::pixel_format::ycbcra:
			return CASPAR_PIX_FMT_LUMA;
		default:
			return format;
		}
	}

	safe_ptr<core::write_frame> make_write_frame(const void* tag, const safe_ptr<AVFrame>& decoded_frame, const safe_ptr<core::frame_factory>& frame_factory, int hints)
	{			
		static tbb::concurrent_unordered_map<int64_t, tbb::concurrent_queue<std::shared_ptr<SwsContext>>> sws_contexts_;
	
		if(decoded_frame->width < 1 || decoded_frame->height < 1)
			return make_safe<core::write_frame>(tag);

		const auto width  = decoded_frame->width;
		const auto height = decoded_frame->height;
		auto desc		  = get_pixel_format_desc(static_cast<PixelFormat>(decoded_frame->format), width, height);
	
		if(hints & core::frame_producer::ALPHA_HINT)
			desc = get_pixel_format_desc(static_cast<PixelFormat>(make_alpha_format(decoded_frame->format)), width, height);

		std::shared_ptr<core::write_frame> write;

		if(desc.pix_fmt == core::pixel_format::invalid)
		{
			auto pix_fmt = static_cast<PixelFormat>(decoded_frame->format);
			auto target_pix_fmt = PIX_FMT_BGRA;

			if(pix_fmt == PIX_FMT_UYVY422)
				target_pix_fmt = PIX_FMT_YUV422P;
			else if(pix_fmt == PIX_FMT_YUYV422)
				target_pix_fmt = PIX_FMT_YUV422P;
			else if(pix_fmt == PIX_FMT_UYYVYY411)
				target_pix_fmt = PIX_FMT_YUV411P;
			else if(pix_fmt == PIX_FMT_YUV420P10)
				target_pix_fmt = PIX_FMT_YUV420P;
			else if(pix_fmt == PIX_FMT_YUV422P10)
				target_pix_fmt = PIX_FMT_YUV422P;
			else if(pix_fmt == PIX_FMT_YUV444P10)
				target_pix_fmt = PIX_FMT_YUV444P;
		
			auto target_desc = get_pixel_format_desc(target_pix_fmt, width, height);

			write = frame_factory->create_frame(tag, target_desc);
			write->set_type(get_mode(*decoded_frame));

			std::shared_ptr<SwsContext> sws_context;

			//CASPAR_LOG(warning) << "Hardware accelerated color transform not supported.";
		
			int64_t key = ((static_cast<int64_t>(width)			 << 32) & 0xFFFF00000000) | 
						  ((static_cast<int64_t>(height)		 << 16) & 0xFFFF0000) | 
						  ((static_cast<int64_t>(pix_fmt)		 <<  8) & 0xFF00) | 
						  ((static_cast<int64_t>(target_pix_fmt) <<  0) & 0xFF);
			
			auto& pool = sws_contexts_[key];
						
			if(!pool.try_pop(sws_context))
			{
				double param;
				sws_context.reset(sws_getContext(width, height, pix_fmt, width, height, target_pix_fmt, SWS_POINT, nullptr, nullptr, &param), sws_freeContext);
			}
			
			if(!sws_context)
			{
				BOOST_THROW_EXCEPTION(operation_failed() << msg_info("Could not create software scaling context.") << 
										boost::errinfo_api_function("sws_getContext"));
			}	
		
			safe_ptr<AVFrame> av_frame(avcodec_alloc_frame(), av_free);	
			avcodec_get_frame_defaults(av_frame.get());			
			if(target_pix_fmt == PIX_FMT_BGRA)
			{
				auto size = avpicture_fill(reinterpret_cast<AVPicture*>(av_frame.get()), write->image_data().begin(), PIX_FMT_BGRA, width, height);
				CASPAR_VERIFY(size == write->image_data().size()); 
			}
			else
			{
				av_frame->width	 = width;
				av_frame->height = height;
				for(size_t n = 0; n < target_desc.planes.size(); ++n)
				{
					av_frame->data[n]		= write->image_data(n).begin();
					av_frame->linesize[n]	= target_desc.planes[n].linesize;
				}
			}

			sws_scale(sws_context.get(), decoded_frame->data, decoded_frame->linesize, 0, height, av_frame->data, av_frame->linesize);	
			pool.push(sws_context);

			write->commit();		
		}
		else
		{
			write = frame_factory->create_frame(tag, desc);
			write->set_type(get_mode(*decoded_frame));

			for(int n = 0; n < static_cast<int>(desc.planes.size()); ++n)
			{
				auto plane            = desc.planes[n];
				auto result           = write->image_data(n).begin();
				auto decoded          = decoded_frame->data[n];
				auto decoded_linesize = decoded_frame->linesize[n];
			
				CASPAR_ASSERT(decoded);
				CASPAR_ASSERT(write->image_data(n).begin());

				if(decoded_linesize != static_cast<int>(plane.width))
				{
					// Copy line by line since ffmpeg sometimes pads each line.
					tbb::parallel_for<size_t>(0, desc.planes[n].height, [&](size_t y)
					{
						fast_memcpy(result + y*plane.linesize, decoded + y*decoded_linesize, plane.linesize);
					});
				}
				else
				{
					fast_memcpy(result, decoded, plane.size);
				}

				write->commit(n);
			}
		}

		//if(decoded_frame->height == 480) // NTSC DV
		//{
		//	write->get_frame_transform().fill_translation[1] += 2.0/static_cast<double>(frame_factory->get_video_format_desc().height);
		//	write->get_frame_transform().fill_scale[1] = 1.0 - 6.0*1.0/static_cast<double>(frame_factory->get_video_format_desc().height);
		//}
	
		// Fix field-order if needed
		if(write->get_type() == core::field_mode::lower && frame_factory->get_video_format_desc().field_mode == core::field_mode::upper)
			write->get_frame_transform().fill_translation[1] += 1.0/static_cast<double>(frame_factory->get_video_format_desc().height);
		else if(write->get_type() == core::field_mode::upper && frame_factory->get_video_format_desc().field_mode == core::field_mode::lower)
			write->get_frame_transform().fill_translation[1] -= 1.0/static_cast<double>(frame_factory->get_video_format_desc().height);

		return make_safe_ptr(write);
	}






};

frame_muxer::frame_muxer(double in_fps, const safe_ptr<core::frame_factory>& frame_factory, const std::wstring& filter)
	: impl_(new implementation(in_fps, frame_factory, filter)){}
void frame_muxer::push(const std::shared_ptr<AVFrame>& video_frame, int hints){impl_->push(video_frame, hints);}
void frame_muxer::push(const std::shared_ptr<core::audio_buffer>& audio_samples){return impl_->push(audio_samples);}
std::shared_ptr<basic_frame> frame_muxer::poll(){return impl_->poll();}
uint32_t frame_muxer::calc_nb_frames(uint32_t nb_frames) const {return impl_->calc_nb_frames(nb_frames);}
bool frame_muxer::video_ready() const{return impl_->video_ready();}
bool frame_muxer::audio_ready() const{return impl_->audio_ready();}

}}