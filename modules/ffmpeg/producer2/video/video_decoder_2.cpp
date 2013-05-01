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

#include "video_decoder.h"

#include "../frame_maker.h"

#include "../ffmpeg_error.h"

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavcodec/avcodec.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg2 {
	
class video_decoder::implementation : boost::noncopyable
{
public:
	std::shared_ptr<AVCodecContext>		codec_context_;

	int64_t								stream_frame_number_;
	bool								is_progressive_;

private:
	std::shared_ptr<AVFrame>			decoded_frame_;

	const int64_t						stream_nb_frames_;

public:
	explicit implementation(AVStream const* stream) 
		: stream_nb_frames_(stream->nb_frames)
		, codec_context_(open_codec(stream->codec))
	{
		//codec_context_ = open_codec(stream->codec);
		decoded_frame_ = std::shared_ptr<AVFrame>(avcodec_alloc_frame(), av_free);

		stream_frame_number_ = 0;
	}

	std::shared_ptr<AVCodecContext> open_codec(AVCodecContext* weak_codec_context)
	{
		// TODO Throw if nullptr
		fix_codec_framerate(weak_codec_context);

		AVCodec* video_codec = avcodec_find_decoder(weak_codec_context->codec_id);
		//THROW_ON_ERROR(video_codec_, "avcodec_find_decoder", video_codec);

		THROW_ON_ERROR2(avcodec_open2(weak_codec_context, video_codec, nullptr), L"[video-decoder] " + widen(video_codec->long_name));

		return std::shared_ptr<AVCodecContext>(weak_codec_context, avcodec_close);
	}

	void fix_codec_framerate(AVCodecContext* codec_context) 
	{
		// Fix for some some codecs which report wrong frame rate
		if(codec_context->time_base.num > 999 && codec_context->time_base.den == 1)
		{
			codec_context->time_base.den = 1000;
		}
	}
	
	std::shared_ptr<AVFrame> decode(const std::shared_ptr<AVPacket>& packet)
	{		
		if(packet->data == nullptr)
		{			
			if(codec_context_->codec->capabilities & CODEC_CAP_DELAY)
			{
				auto video = decode_packet(packet);
				if(video)
				{
					return video;
				}
			}
					
			stream_frame_number_ = static_cast<size_t>(packet->pos);
			avcodec_flush_buffers(codec_context_.get());
			return frame_maker::flush_video();
		}
			
		return decode_packet(packet);
	}

	std::shared_ptr<AVFrame> decode_packet(const std::shared_ptr<AVPacket>& packet)
	{
		int frame_finished = 0;
		//THROW_ON_ERROR2(avcodec_decode_video2(codec_context_.get(), decoded_frame_.get(), &frame_finished, packet.get()), "[video_decoder]");
		
		// If a decoder consumes less then the whole packet then something is wrong
		// that might be just harmless padding at the end, or a problem with the
		// AVParser or demuxer which puted more then one frame in a AVPacket.

		// Some ffmpeg clients seem to prefer do / while (frame_finished == 0) { decode... }  CP 2013-04

		//if(frame_finished == 0)	
		//	return nullptr;

		do
		{
	//		avcodec_decode_video2(codec_context_.get(), decoded_frame_.get(), &frame_finished, packet.get());
			THROW_ON_ERROR2(avcodec_decode_video2(codec_context_.get(), decoded_frame_.get(), &frame_finished, packet.get()), "[video_decoder]");
		} while (frame_finished == 0);

		is_progressive_ = !decoded_frame_->interlaced_frame;

		if(decoded_frame_->repeat_pict > 0)
			CASPAR_LOG(warning) << "[video_decoder] Field repeat_pict not implemented.";
		
		++stream_frame_number_;

		return decoded_frame_;
	}
	
	uint32_t nb_frames() const
	{
		return static_cast<uint32_t>(std::max<int64_t>(stream_nb_frames_, stream_frame_number_)); // TODO REVIEW: Should this math be done in the frame_maker? See who uses the public nb_frames and file_frame_number CP 2013-04
	}

	std::wstring print() const
	{		
		return L"[video-decoder] " + widen(codec_context_->codec->long_name);
	}
};

video_decoder::video_decoder(AVStream const* stream) : impl_(new implementation(stream)){}
std::shared_ptr<AVFrame> video_decoder::decode(const std::shared_ptr<AVPacket>& packet){return impl_->decode(packet);}
size_t video_decoder::width() const{return impl_->codec_context_->width;}
size_t video_decoder::height() const{return impl_->codec_context_->height;}
uint32_t video_decoder::nb_frames() const{return impl_->nb_frames();}
uint32_t video_decoder::file_frame_number() const{return static_cast<uint32_t>(impl_->stream_frame_number_);}
bool	video_decoder::is_progressive() const{return impl_->is_progressive_;}
std::wstring video_decoder::print() const{return impl_->print();}

}}