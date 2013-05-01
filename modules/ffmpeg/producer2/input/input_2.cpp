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

#include "../../stdafx.h"

#include "input.h"

#include "../util/flv.h"
#include "../../ffmpeg_producer_params.h"
#include "../ffmpeg_error.h"

#include <common/exception/exceptions.h>
#include <common/exception/win32_exception.h>
#include <common/utility/string.h>

#include <core/video_format.h>

#include <tbb/atomic.h>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
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

namespace caspar { namespace ffmpeg2 {
		
class input::implementation : boost::noncopyable
{
public:
	std::shared_ptr<AVFormatContext>				format_context_; // Destroy this last
	tbb::atomic<bool>								loop_; // TODO REVIEW: This is in the same thread as the frame_maker so should not need to be tbb::atomic CP 2013-04

	int												video_stream_index_;
	int												audio_stream_index_;

private:
	const std::shared_ptr<ffmpeg::ffmpeg_producer_params>	params_;

	int												last_read_result_;
	uint32_t										frame_number_;

public:
	explicit implementation(
		const std::shared_ptr<ffmpeg::ffmpeg_producer_params>& params
	) : params_(params),
		frame_number_(0),
		last_read_result_(-1)
	{		
		format_context_ = open_input(params);
		loop_			= params->loop;

		if(params_->start > 0)			
			seek(params_->start);
								
	}
	
	std::wstring print() const
	{
		return L"ffmpeg_input[" + params_->resource_name + L")]";
	}
	
	std::shared_ptr<AVPacket> read_packet()
	{	
		std::shared_ptr<AVPacket> packet(new AVPacket, [](AVPacket* p)
		{
			av_free_packet(p);
			delete p;
		});

		av_init_packet(packet.get());
		last_read_result_ = av_read_frame(format_context_.get(), packet.get()); // packet is only valid until next call of av_read_frame. Use av_dup_packet to extend its life.	
		THROW_ON_ERROR(last_read_result_, "av_read_frame", print());
		return packet;
				// TODO Check that this is up one CP 2013-04
				/*
				if(is_eof(ret))														     
				{
					frame_number_	= 0;

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
					THROW_ON_ERROR(ret, "av_read_frame", print());

					if(packet->stream_index == default_stream_index_)
						++frame_number_;

					THROW_ON_ERROR2(av_dup_packet(packet.get()), print());
				
					// Make sure that the packet is correctly deallocated even if size and data is modified during decoding.
					auto size = packet->size;
					auto data = packet->data;
			
					packet = safe_ptr<AVPacket>(packet.get(), [packet, size, data](AVPacket*)
					{
						packet->size = size;
						packet->data = data;				
					});

					buffer_.try_push(packet);
					buffer_size_ += packet->size;
				
					graph_->set_value("buffer-size", (static_cast<double>(buffer_size_)+0.001)/MAX_BUFFER_SIZE);
					graph_->set_value("buffer-count", (static_cast<double>(buffer_.size()+0.001)/MAX_BUFFER_COUNT));
				}	
		
				tick();		
			}
			catch(...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				executor_.stop();
			}
		});
		*/

	}	
	
	std::shared_ptr<AVFormatContext> open_input(const std::shared_ptr<ffmpeg::ffmpeg_producer_params>& params)
	{
		AVFormatContext* weak_context = nullptr;

		switch (params->resource_type) {
		case ffmpeg::FFMPEG_FILE:
			THROW_ON_ERROR2(avformat_open_input(&weak_context, narrow(params->resource_name).c_str(), nullptr, nullptr), params->resource_name);
			break;
		case ffmpeg::FFMPEG_DEVICE: {
			AVDictionary* format_options = NULL;
			av_dict_set(&format_options, "video_size", narrow(params->size_str).c_str(), 0); // 640x360 for 16:9
			av_dict_set(&format_options, "pixel_format", narrow(params->pixel_format).c_str(), 0); // yuyv422 for sony // TODO auto set this from the ffmpeg log after -list_options on init
			//av_dict_set(&format_options, "avioflags", "direct", 0);
			av_dict_set(&format_options, "framerate", narrow(params->frame_rate).c_str(), 0); // TODO auto set this from channel fps and -list_options CP 2013-04
			AVInputFormat* input_format = av_find_input_format("dshow");
			THROW_ON_ERROR2(avformat_open_input(&weak_context, narrow(params->resource_name).c_str(), input_format, &format_options), params->resource_name);
			av_dict_free(&format_options);
			} break;
		case ffmpeg::FFMPEG_STREAM:
			THROW_ON_ERROR2(avformat_open_input(&weak_context, narrow(params->resource_name).c_str(), nullptr, nullptr), params->resource_name);
			break;
		};

		std::shared_ptr<AVFormatContext> context(weak_context, av_close_input_file); // TODO REVIEW Does this also need to av_free(ptr)? CP 2013-03
		THROW_ON_ERROR2(avformat_find_stream_info(context.get(), nullptr), params->resource_name);

		// Note that we can run without either video or audio so there is no need to throw on failure here.
		video_stream_index_ = find_best_video_stream(context);
		audio_stream_index_ = find_best_audio_stream(context);

		//!!!fix_meta_data(context, video_stream_index_);

		return context;
	}

	int find_best_video_stream(std::shared_ptr<AVFormatContext> const& format_context)
	{
		return av_find_best_stream(format_context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
		/*
		for(int i = 0; i < format_context->nb_streams; ++i) {
			if( format_context->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO ) {
				return i;
			}
		}
		return -1;
		*/
	}

	int find_best_audio_stream(std::shared_ptr<AVFormatContext> const& format_context)
	{
		return av_find_best_stream(format_context.get(), AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
	}

	AVStream const* video_stream() const
	{
		return video_stream_index_ < 0 ? nullptr : format_context_->streams[video_stream_index_];
	}

	AVStream const* audio_stream() const
	{
		return audio_stream_index_ < 0 ? nullptr : format_context_->streams[audio_stream_index_];
	}

	void fix_meta_data(std::shared_ptr<AVFormatContext> const& format_context, int video_stream_index)
	{
		if(video_stream_index > -1)
		{
			auto video_stream   = format_context->streams[video_stream_index];
			auto video_context  = format_context->streams[video_stream_index]->codec;
						
			if(boost::filesystem2::path(format_context->filename).extension() == ".flv")
			{
				try
				{
					auto meta = read_flv_meta_info(format_context->filename);
					double fps = boost::lexical_cast<double>(meta["framerate"]);
					video_stream->nb_frames = static_cast<int64_t>(boost::lexical_cast<double>(meta["duration"])*fps);
				}
				catch(...){}
			}
			else
			{
				auto stream_time = video_stream->time_base;
				auto duration	 = video_stream->duration;
				auto codec_time  = video_context->time_base;
				auto ticks		 = video_context->ticks_per_frame;

				if(video_stream->nb_frames == 0)
					video_stream->nb_frames = (duration*stream_time.num*codec_time.den)/(stream_time.den*codec_time.num*ticks);	
			}
		}
	}

	double read_fps(double fail_value)
	{						
		if(video_stream_index_ > -1)
		{
			const auto video_context = format_context_->streams[video_stream_index_]->codec;
			const auto video_stream  = format_context_->streams[video_stream_index_];
						
			AVRational time_base = video_context->time_base;

			if(boost::filesystem2::path(format_context_->filename).extension() == ".flv")
			{
				try
				{
					auto meta = read_flv_meta_info(format_context_->filename);
					return boost::lexical_cast<double>(meta["framerate"]);
				}
				catch(...)
				{
					return 0.0;
				}
			}
			else
			{
				time_base.num *= video_context->ticks_per_frame;

				if(!is_sane_fps(time_base))
				{			
					time_base = fix_time_base(time_base);

					if(!is_sane_fps(time_base) && audio_stream_index_ > -1)
					{
						const auto audio_context = format_context_->streams[audio_stream_index_]->codec;
						const auto audio_stream  = format_context_->streams[audio_stream_index_];

						double duration_sec = audio_stream->duration / static_cast<double>(audio_context->sample_rate);
								
						time_base.num = static_cast<int>(duration_sec*100000.0);
						time_base.den = static_cast<int>(video_stream->nb_frames*100000);
					}
				}
			}
		
			double fps = static_cast<double>(time_base.den) / static_cast<double>(time_base.num);

			double closest_fps = 0.0;
			for(int n = 0; n < core::video_format::count; ++n)
			{
				auto format = core::video_format_desc::get(static_cast<core::video_format::type>(n));

				double diff1 = std::abs(format.fps - fps);
				double diff2 = std::abs(closest_fps - fps);

				if(diff1 < diff2)
					closest_fps = format.fps;
			}
	
			return closest_fps;
		}

		return fail_value;	
	}

	bool is_sane_fps(AVRational time_base)
	{
		double fps = static_cast<double>(time_base.den) / static_cast<double>(time_base.num);
		return fps > 20.0 && fps < 65.0;
	}

	AVRational fix_time_base(AVRational time_base)
	{
		if(time_base.num == 1)
			time_base.num = static_cast<int>(std::pow(10.0, static_cast<int>(std::log10(static_cast<float>(time_base.den)))-1));	
			
		if(!is_sane_fps(time_base))
		{
			auto tmp = time_base;
			tmp.den /= 2;
			if(is_sane_fps(tmp))
				time_base = tmp;
		}

		return time_base;
	}

	void seek(const uint32_t target)
	{  	
		CASPAR_LOG(debug) << print() << " Seeking: " << target;

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
		
		THROW_ON_ERROR2(avformat_seek_file(format_context_.get(), video_stream_index_, std::numeric_limits<int64_t>::min(), fixed_target, std::numeric_limits<int64_t>::max(), 0), print());		
		
		/* TODO This shouldn't be needed CP 2013-04
		auto flush_packet	= create_packet();
		flush_packet->data	= nullptr;
		flush_packet->size	= 0;
		flush_packet->pos	= target;

		buffer_.push(flush_packet);
		*/
	}	

	bool eof() const
	{
		if(last_read_result_ == AVERROR(EIO))
			CASPAR_LOG(trace) << print() << " Received EIO, assuming EOF. ";
		if(last_read_result_ == AVERROR_EOF)
			CASPAR_LOG(trace) << print() << " Received EOF. ";

		return last_read_result_ == AVERROR_EOF || last_read_result_ == AVERROR(EIO) || frame_number_ >= params_->length; // av_read_frame doesn't always correctly return AVERROR_EOF;
	}
};

input::input(
	const std::shared_ptr<ffmpeg::ffmpeg_producer_params>& params
) : impl_(new implementation(params)){}
AVStream const* input::video_stream() const { return impl_->video_stream(); }
AVStream const* input::audio_stream() const { return impl_->audio_stream(); }
int input::video_stream_index() const { return impl_->video_stream_index_; }
int input::audio_stream_index() const { return impl_->audio_stream_index_; }
double input::read_fps(double fail_value) { return impl_->read_fps(fail_value); }
std::shared_ptr<AVPacket> input::read_packet() { return impl_->read_packet(); }
bool input::eof() const {return impl_->eof();}
std::shared_ptr<AVFormatContext> input::format_context(){return impl_->format_context_;}
void input::loop(bool value){impl_->loop_ = value;}
bool input::loop() const{return impl_->loop_;}
void input::seek(uint32_t target){impl_->seek(target);}

}}
