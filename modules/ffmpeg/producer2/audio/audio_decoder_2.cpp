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

#include "audio_decoder.h"

#include "audio_resampler.h"

//#include "../util/util.h"
#include "../ffmpeg_error.h"

#include "../frame_maker.h"

#include <core/video_format.h>

#include <tbb/cache_aligned_allocator.h>

#include <queue>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg2 {
	
class audio_decoder::implementation : boost::noncopyable
{
public:
	uint64_t													stream_frame_number_;

private:
	int															index_;
	std::shared_ptr<AVCodecContext>								codec_context_;

	audio_resampler												resampler_;

	std::vector<int8_t,  tbb::cache_aligned_allocator<int8_t>>	buffer1_;

public:
	explicit implementation(AVStream const* audio_stream, const core::video_format_desc& format_desc) 
		: resampler_(
			format_desc.audio_channels,    audio_stream->codec->channels,
			format_desc.audio_sample_rate, audio_stream->codec->sample_rate,
			AV_SAMPLE_FMT_S32,			   audio_stream->codec->sample_fmt
		)
		, buffer1_(AVCODEC_MAX_AUDIO_FRAME_SIZE*2)
//		, nb_frames_(0) //audio_stream->nb_frames)
	{
		codec_context_ = open_codec(audio_stream->codec);

		stream_frame_number_ = 0;   
	}

	std::shared_ptr<AVCodecContext> open_codec(AVCodecContext* weak_codec_context) {
		// TODO Throw if nullptr

		AVCodec* audio_codec = avcodec_find_decoder(weak_codec_context->codec_id);
		//THROW_ON_ERROR(video_codec_, "avcodec_find_decoder", video_codec);

		THROW_ON_ERROR2(avcodec_open2(weak_codec_context, audio_codec, nullptr), L"[audio-decoder] " + widen(audio_codec->long_name));
		return std::shared_ptr<AVCodecContext>(weak_codec_context, avcodec_close);

	}

	std::shared_ptr<core::audio_buffer> decode(const std::shared_ptr<AVPacket>& packet)
	{
		if(packet->data == nullptr)
		{
			stream_frame_number_ = static_cast<size_t>(packet->pos);
			avcodec_flush_buffers(codec_context_.get());
			return frame_maker::flush_audio();
		}

		auto audio = decode_packet(packet);

		// TODO REVIEW Why this check? CP 2013-04
		//if(packet->size == 0)					
		//	packets_.pop();

		return audio;
	}

	std::shared_ptr<core::audio_buffer> decode_packet(const std::shared_ptr<AVPacket>& packet)
	{		
		buffer1_.resize(AVCODEC_MAX_AUDIO_FRAME_SIZE*2);
		int written_bytes = buffer1_.size() - FF_INPUT_BUFFER_PADDING_SIZE;
		
		int ret = THROW_ON_ERROR2(avcodec_decode_audio3(codec_context_.get(), reinterpret_cast<int16_t*>(buffer1_.data()), &written_bytes, packet.get()), "[audio_decoder]");

		// There might be several frames in one packet.
		packet->size -= ret;
		packet->data += ret;
			
		buffer1_.resize(written_bytes);

		buffer1_ = resampler_.resample(std::move(buffer1_));
		
		const auto n_samples = buffer1_.size() / av_get_bytes_per_sample(AV_SAMPLE_FMT_S32);
		const auto samples = reinterpret_cast<int32_t*>(buffer1_.data());

		++stream_frame_number_;

		return std::make_shared<core::audio_buffer>(samples, samples + n_samples);
	}

	uint32_t nb_frames() const
	{
		return 0;//std::max<int64_t>(nb_frames_, stream_frame_number_);
	}

	std::wstring print() const
	{		
		return L"[audio-decoder] " + widen(codec_context_->codec->long_name);
	}
};

audio_decoder::audio_decoder(AVStream const* audio_stream, const core::video_format_desc& format_desc) : impl_(new implementation(audio_stream, format_desc)){}
std::shared_ptr<core::audio_buffer> audio_decoder::decode(const std::shared_ptr<AVPacket>& packet){return impl_->decode(packet);}
uint32_t audio_decoder::nb_frames() const{return impl_->nb_frames();}
uint32_t audio_decoder::file_frame_number() const{return static_cast<uint32_t>(impl_->stream_frame_number_);}
std::wstring audio_decoder::print() const{return impl_->print();}

}}