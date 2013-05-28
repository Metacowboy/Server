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

#include "frame_maker.h"

#include "../ffmpeg_producer_params.h"
#include "ffmpeg_error.h"

#include "input/input.h"
#include "audio/audio_decoder.h"
#include "video/video_decoder.h"
#include "muxer/frame_muxer.h"

#include <common/concurrency/executor.h>
#include <common/diagnostics/graph.h>
#include <common/exception/exceptions.h>
#include <common/exception/win32_exception.h>
#include <common/memory/memcpy.h>
#include <common/utility/string.h>

#include <core/video_format.h>
#include <core/producer/frame_producer.h>
#include <core/producer/frame/basic_frame.h>
#include <core/producer/frame/frame_transform.h>
#include <core/producer/frame/frame_factory.h>
#include <core/producer/frame/pixel_format.h>
#include <core/mixer/write_frame.h>

#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_queue.h>
#include <tbb/atomic.h>
#include <tbb/recursive_mutex.h>
#include <tbb/parallel_invoke.h>

#include <boost/range/algorithm.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <boost/format.hpp>
#include <boost/timer.hpp>

#if defined(_MSC_VER)
#pragma warning (push)
//#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

static const size_t MAX_BUFFER_COUNT = 100;
static const size_t MIN_BUFFER_COUNT = 50;
static const size_t MAX_BUFFER_SIZE  = 64 * 1000000;

namespace caspar { namespace ffmpeg2 {
		
class frame_maker::implementation : boost::noncopyable
{	
private:
	const safe_ptr<diagnostics::graph>							graph_;

	const std::shared_ptr<ffmpeg::ffmpeg_producer_params>				params_;
//	const safe_ptr<core::frame_factory>							frame_factory_;
//	const core::video_format_desc								format_desc_;

	std::unique_ptr<input>										input_;	
	std::unique_ptr<video_decoder>								video_decoder_;
	std::unique_ptr<audio_decoder>								audio_decoder_;	
	std::unique_ptr<frame_muxer>								muxer_;

	double														fps_;

	tbb::atomic<bool>											loop_;
	
	uint32_t													frame_number_;
	
	tbb::concurrent_bounded_queue<safe_ptr<core::basic_frame>>	frame_buffer_;
	tbb::atomic<size_t>											frame_buffer_size_;

	safe_ptr<core::basic_frame>									last_frame_;

	int															hints_; // TODO Get hints working again CP 2013-04

	boost::timer												frame_timer_;
	boost::timer												tick_timer_;
	double														tick_delay_;

	executor													executor_;
	
public:
	explicit implementation(
		const safe_ptr<diagnostics::graph> graph, 
		const std::shared_ptr<ffmpeg::ffmpeg_producer_params>& params,
		const safe_ptr<core::frame_factory>& frame_factory
	) : graph_(graph)
	  , params_(params)
//	  , frame_factory_(frame_factory)
//	  , format_desc_(frame_factory->get_video_format_desc())
	  , last_frame_(core::basic_frame::empty())
//	  , default_stream_index_(av_find_default_stream_index(format_context_.get()))
	  , fps_(0.0)
	  , hints_(0)
	  , frame_number_(0)
	  , tick_delay_(0.0)
	  , executor_(print())
	{
		input_.reset(new input(params));

		auto video_stream = input_->video_stream();
		auto audio_stream = input_->audio_stream();
		if (video_stream == nullptr && audio_stream == nullptr) 
		{
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("No video or audio stream found in resource"));		
		}
		if (video_stream)
		{
			video_decoder_.reset(new video_decoder(input_->video_stream()));
		}
		auto format_desc = frame_factory->get_video_format_desc();
		if (audio_stream)
		{
			audio_decoder_.reset(new audio_decoder(input_->audio_stream(), format_desc));
		}

		// TODO Log the audio / video not present warnings. CP 2013-04
		fps_ = input_->read_fps(format_desc.fps);
		muxer_.reset(new frame_muxer(fps_, frame_factory, params_->filter_str));

		loop_			= params->loop;
		frame_buffer_size_	= 0;

		//if(params_->start > 0)			
		//	queued_seek(params_->start);
				
		graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f, 0.8));	
		graph_->set_color("read-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("decode-time", diagnostics::color(1.0f, 1.0f, 0.1f));
		//graph_->set_color("frame-time", diagnostics::color(1.0f, 0.1f, 0.1f));
		graph_->set_color("frame-time", diagnostics::color(0.6f, 0.3f, 0.9f));	
		graph_->set_color("underflow", diagnostics::color(0.3f, 0.6f, 0.9f));	
		//graph_->set_color("seek", diagnostics::color(1.0f, 0.5f, 0.0f));	
		//graph_->set_color("buffer-count", diagnostics::color(0.7f, 0.4f, 0.4f));
		graph_->set_color("buffer-size", diagnostics::color(0.1f, 0.1f, 1.0f));	

		tick();

	}

	~implementation()
	{
	}

	safe_ptr<caspar::core::basic_frame> poll()
	{
		tick();
		graph_->set_value("tick-time", tick_timer_.elapsed()*fps_*0.5);
		tick_timer_.restart();
		graph_->set_value("buffer-size", (static_cast<double>(frame_buffer_.size()+0.001) * 0.1)); // 1 / 10 * 0.5
		safe_ptr<caspar::core::basic_frame> frame;
		auto result = frame_buffer_.try_pop(frame);
		
		if(result)
		{
			//tick();
//			if(packet)
//				buffer_size_ -= packet->size;
			return last_frame_ = frame;
		}

		//graph_->set_value("buffer-size", (static_cast<double>(buffer_size_)+0.001)/MAX_BUFFER_SIZE);
		//graph_->set_value("buffer-count", (static_cast<double>(buffer_.size()+0.001)/MAX_BUFFER_COUNT));
		//graph_->set_tag("underflow");
		return last_frame_;
	}

	/*
	void seek(uint32_t target)
	{
		executor_.begin_invoke([=]
		{
			std::shared_ptr<AVPacket> packet;
			while(buffer_.try_pop(packet) && packet)
				buffer_size_ -= packet->size;

			queued_seek(target);

			tick();
		}, high_priority);
	}
	*/

	bool full() const
	{
		return (frame_buffer_size_ > MAX_BUFFER_SIZE || frame_buffer_.size() > MAX_BUFFER_COUNT) && frame_buffer_.size() > MIN_BUFFER_COUNT;
	}

	void tick()
	{	
		if(!executor_.is_running())
			return;
		
		executor_.begin_invoke([this]
		{	
			if (frame_buffer_.size() >= 10) {
				return;
			}
			//if(full())
			//	return;

			try
			{
				auto frame = decode_packet();
				if (frame == bad_video_frame())
					graph_->set_tag("bad-frame");

				if (frame)
				{
					if (params_->resource_type == ffmpeg::FFMPEG_FILE || frame_buffer_.size() < 10) 
					{
						frame_buffer_.push(make_safe_ptr(frame));
					}
				}
				//av_usleep((unsigned)(tick_delay_ * 1000000.0));
				tick();
			}
			catch(...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				executor_.stop();
			}
		});
	}

	std::shared_ptr<core::basic_frame> read_frame()
	{
		std::shared_ptr<core::basic_frame> frame = nullptr;
		for (int i = 0; i < 32 && frame == nullptr; ++i)
		{
			frame = decode_packet();
		}
		if (frame == nullptr)
			graph_->set_tag("underflow");

		return frame;
	}

	std::shared_ptr<core::basic_frame> decode_packet()
	{
		// Read a packet from the input stream
		frame_timer_.restart();

		auto packet = input_->read_packet();
	
		double read_time = frame_timer_.elapsed();
		graph_->set_value("read-time", read_time * fps_ * 0.5);

		// If end of stream loop or shutdown
		if(input_->eof())														     
		{
			frame_number_ = 0;

			if(params_->loop)
			{
				queued_seek(params_->start);
				graph_->set_tag("seek");		
				CASPAR_LOG(trace) << print() << " Looping.";			
			}		
			else
				executor_.stop();
		}
		else
		{	
			frame_timer_.restart();
			
			std::shared_ptr<AVFrame>			video;
			std::shared_ptr<core::audio_buffer> audio;

			if (video_decoder_ && packet->stream_index == input_->video_stream_index())
			{
				video = video_decoder_->decode(packet);	
				muxer_->push(video, hints_);
				if(!audio_decoder_)
				{
					if(video == flush_video())
						muxer_->push(flush_audio());
					else if(!muxer_->audio_ready())
						muxer_->push(empty_audio());
				}
			} else if (audio_decoder_ && packet->stream_index == input_->audio_stream_index())
			{
				do
				{
					audio = audio_decoder_->decode(packet);		
					muxer_->push(audio);
					if(!video_decoder_)
					{
						if(audio == flush_audio())
							muxer_->push(flush_video(), 0);
						else if(!muxer_->video_ready())
							muxer_->push(empty_video(), 0);
					}
				} while (packet->size > 0 && audio != nullptr);
			}
		
			graph_->set_value("decode-time", frame_timer_.elapsed() * fps_ * 0.5);

			frame_number_ = std::max(frame_number_, video_decoder_ ? video_decoder_->file_frame_number() : 0);
			//file_frame_number = std::max(file_frame_number, audio_decoder_ ? audio_decoder_->file_frame_number() : 0);

			auto write_frame = muxer_->poll();

			graph_->set_value("frame-time", frame_timer_.elapsed() * fps_ * 0.5);

			return write_frame;

		}
		return bad_video_frame();
	}

	int synchronize_audio(int number_samples)
	{
		int wanted_samples = number_samples;
		return wanted_samples;
	}

	std::shared_ptr<core::basic_frame> bad_video_frame()
	{
		static std::shared_ptr<core::basic_frame> frame(new core::basic_frame());
		return frame;
	}

	void queued_seek(const uint32_t target)
	{  	
		CASPAR_LOG(debug) << print() << " Seeking: NYI" << target;
		/*
		int flags = AVSEEK_FLAG_FRAME;
		if(target == 0)
		{
			// Fix VP6 seeking
			int vid_stream_index = av_find_best_stream(format_context_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
			if(vid_stream_index >= 0)
			{
				auto codec_id = format_context_->streams[vid_stream_index]->codec->codec_id;
				if(codec_id == CODEC_ID_VP6A || codec_id == CODEC_ID_VP6F || codec_id == CODEC_ID_VP6)
					flags = AVSEEK_FLAG_BYTE;
			}
		}
		
		auto stream = format_context_->streams[video_stream_index_];
		auto codec  = stream->codec;
		auto fixed_target = (target*stream->time_base.den*codec->time_base.num)/(stream->time_base.num*codec->time_base.den)*codec->ticks_per_frame;
		
		THROW_ON_ERROR2(avformat_seek_file(format_context_.get(), default_stream_index_, std::numeric_limits<int64_t>::min(), fixed_target, std::numeric_limits<int64_t>::max(), 0), print());		
		
		auto flush_packet	= create_packet();
		flush_packet->data	= nullptr;
		flush_packet->size	= 0;
		flush_packet->pos	= target;

		buffer_.push(flush_packet);
		*/
	}	

	bool is_eof(int ret)
	{
		if(ret == AVERROR(EIO))
			CASPAR_LOG(trace) << print() << " Received EIO, assuming EOF. ";
		if(ret == AVERROR_EOF)
			CASPAR_LOG(trace) << print() << " Received EOF. ";

		return ret == AVERROR_EOF || ret == AVERROR(EIO) || frame_number_ >= params_->length; // av_read_frame doesn't always correctly return AVERROR_EOF;
	}

	boost::property_tree::wptree info() const
	{
		boost::property_tree::wptree info;
		info.add(L"type",				L"ffmpeg-producer");
		info.add(L"filename",			params_->resource_name);
		info.add(L"width",				video_decoder_ ? video_decoder_->width() : 0);
		info.add(L"height",				video_decoder_ ? video_decoder_->height() : 0);
		info.add(L"progressive",		video_decoder_ ? video_decoder_->is_progressive() : false);
		info.add(L"fps",				fps_);
		info.add(L"loop",				input_->loop());
		info.add(L"frame-number",		frame_number_);
//		auto nb_frames2 = nb_frames();
//		info.add(L"nb-frames",			nb_frames2 == std::numeric_limits<int64_t>::max() ? -1 : nb_frames2);
		info.add(L"file-frame-number",	frame_number_);
//		info.add(L"file-nb-frames",		nb_frames());
		return info;
	}

	std::wstring print() const
	{
		std::wstring mode = video_decoder_ ? print_mode(video_decoder_->width(), video_decoder_->height(), fps_, !video_decoder_->is_progressive()) : L"";
		mode += L"|";
		// TODO Add in the frame counts CP 2013-04
		return mode;
	}
					
	std::wstring print_mode(size_t width, size_t height, double fps, bool interlaced) const
	{
		std::wostringstream fps_ss;
		fps_ss << std::fixed << std::setprecision(2) << (!interlaced ? fps : 2.0 * fps);

		return boost::lexical_cast<std::wstring>(width) + L"x" + boost::lexical_cast<std::wstring>(height) + (!interlaced ? L"p" : L"i") + fps_ss.str();
	}



};

frame_maker::frame_maker(
	const safe_ptr<diagnostics::graph>& graph, 
	const std::shared_ptr<ffmpeg::ffmpeg_producer_params>& params,
	const safe_ptr<core::frame_factory>& frame_factory
) : impl_(new implementation(graph, params, frame_factory)){}
//bool input::eof() const {return !impl_->executor_.is_running();}
safe_ptr<caspar::core::basic_frame> frame_maker::poll() { return impl_->poll(); }
boost::property_tree::wptree frame_maker::info() const { return impl_->info(); }
std::wstring frame_maker::print() const { return impl_->print(); }

std::shared_ptr<core::audio_buffer> frame_maker::flush_audio()
{
	static std::shared_ptr<core::audio_buffer> audio(new core::audio_buffer());
	return audio;
}

std::shared_ptr<core::audio_buffer> frame_maker::empty_audio()
{
	static std::shared_ptr<core::audio_buffer> audio(new core::audio_buffer());
	return audio;
}

std::shared_ptr<AVFrame> frame_maker::flush_video()
{
	static std::shared_ptr<AVFrame> video(avcodec_alloc_frame(), av_free);
	return video;
}

std::shared_ptr<AVFrame> frame_maker::empty_video()
{
	static std::shared_ptr<AVFrame> video(avcodec_alloc_frame(), av_free);
	return video;
}


}}

