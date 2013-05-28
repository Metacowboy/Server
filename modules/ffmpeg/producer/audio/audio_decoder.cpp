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

#include "../util/util.h"
#include "../../ffmpeg_error.h"

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
	#include "libswresample/swresample.h"

}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg {
	
struct audio_decoder::implementation : boost::noncopyable
{	
	int															index_;
	const safe_ptr<AVCodecContext>								codec_context_;		
	const core::video_format_desc								format_desc_;
	int64_t														format_desc_channel_layout_;

	audio_resampler												resampler_;

	std::vector<uint8_t,  tbb::cache_aligned_allocator<uint8_t>>	buffer1_;

	std::queue<safe_ptr<AVPacket>>								packets_;

	const int64_t												nb_frames_;
	tbb::atomic<size_t>											file_frame_number_;

	std::shared_ptr<SwrContext>									swr_context_;

public:
	explicit implementation(const safe_ptr<AVFormatContext>& context, const core::video_format_desc& format_desc) 
		: format_desc_(format_desc)	
		, codec_context_(open_codec(*context, AVMEDIA_TYPE_AUDIO, index_))
		, resampler_(format_desc.audio_channels,	codec_context_->channels,
					 format_desc.audio_sample_rate, codec_context_->sample_rate,
					 AV_SAMPLE_FMT_S32,				codec_context_->sample_fmt)
		, buffer1_(AVCODEC_MAX_AUDIO_FRAME_SIZE * 4)
		, nb_frames_(0)//context->streams[index_]->nb_frames)
	{
		format_desc_channel_layout_ = av_get_default_channel_layout(format_desc_.audio_channels);
		file_frame_number_ = 0;   
	}

	void push(const std::shared_ptr<AVPacket>& packet)
	{			
		if(!packet)
			return;

		if(packet->stream_index == index_ || packet->data == nullptr)
			packets_.push(make_safe_ptr(packet));
	}	
	
	std::shared_ptr<core::audio_buffer> poll()
	{
		if(packets_.empty())
			return nullptr;
				
		auto packet = packets_.front();

		if(packet->data == nullptr)
		{
			packets_.pop();
			file_frame_number_ = static_cast<size_t>(packet->pos);
			avcodec_flush_buffers(codec_context_.get());
			return flush_audio();
		}

		auto audio = decode(packet);

		if(packet->size == 0)					
			packets_.pop();

		return audio;
	}

	std::shared_ptr<core::audio_buffer> decode(const std::shared_ptr<AVPacket>& packet)
	{		
		int frame_finished = 0;
		auto decoded_frame = std::shared_ptr<AVFrame>(avcodec_alloc_frame(), av_free);
		do
		{
			auto len1 = THROW_ON_ERROR2(avcodec_decode_audio4(codec_context_.get(), decoded_frame.get(), &frame_finished, packet.get()), "[audio_decoder]");
			packet->data += len1;
			packet->size -= len1;
		} while (!frame_finished && packet->size > 0);

		// TODO What if !frame_finished && packet->size == 0 Really shouldn't happen.

		// TODO Stop here, allow the consumer to figure out pts / drift / sync, then call another function to resample

		int64_t codec_channel_layout;
		if (codec_context_->channel_layout && codec_context_->channels == av_get_channel_layout_nb_channels(codec_context_->channel_layout))
		{
			codec_channel_layout = codec_context_->channel_layout;
		} else 
		{
			codec_channel_layout = av_get_default_channel_layout(codec_context_->channels);
		}

		int wanted_samples = decoded_frame->nb_samples;

		// TODO Might as well init the swr context in the ctor.
		if (codec_context_->sample_fmt != AV_SAMPLE_FMT_S32 ||
			codec_channel_layout != format_desc_channel_layout_ ||
			codec_context_->sample_rate != (int)format_desc_.audio_sample_rate ||
            (wanted_samples != decoded_frame->nb_samples && !swr_context_))
		{
			swr_context_.reset(swr_alloc_set_opts(
				NULL,
				format_desc_channel_layout_,	AV_SAMPLE_FMT_S32,			format_desc_.audio_sample_rate,
                codec_channel_layout,			codec_context_->sample_fmt, codec_context_->sample_rate,
                0, NULL),
				[](SwrContext* p) { swr_free(&p); }
			);
			// TODO Can THROW2 this
			if (!swr_context_ || swr_init(swr_context_.get()) < 0) {
/*                    fprintf(stderr, "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                        codec_context_->sample_rate,
                        av_get_sample_fmt_name(codec_context_->sample_fmt),
                        codec_context_->channels,
                        is->audio_tgt.freq,
                        av_get_sample_fmt_name(is->audio_tgt.fmt),
                        is->audio_tgt.channels);
                    break;*/
			}
		}

        if (swr_context_)
		{
			// TODO Add the compensation back in
			//if (wanted_samples != decoded_frame->nb_samples)
			//{
			//	// TODO This is mostly done, just THROW2 on it for the error handling.
			//	if (swr_set_compensation(swr_context_.get(), (wanted_samples - is->frame->nb_samples) * format_desc_.audio_sample_rate / codec_context_->sample_rate,
			//		wanted_samples * format_desc_.audio_sample_rate / codec_context_->sample_rate) < 0)
			//	{
			//		fprintf(stderr, "swr_set_compensation() failed\n");
			//		break;
			//	}
			//}
			const uint8_t *in[] = { decoded_frame->data[0] };
			buffer1_.resize(AVCODEC_MAX_AUDIO_FRAME_SIZE * 4); // TODO REVIEW Maybe we shouldn't fiddle with the size.
			uint8_t* out[] = { buffer1_.data() };
			int out_count = buffer1_.size() / format_desc_.audio_channels / av_get_bytes_per_sample(AV_SAMPLE_FMT_S32);
			int len2 = THROW_ON_ERROR2(swr_convert(swr_context_.get(), out, out_count, in, decoded_frame->nb_samples), "[audio_decoder]");
			// TODO put this check back
            //if (len2 == out_count) {
            //    fprintf(stderr, "warning: audio buffer is probably too small\n");
            //    swr_init(is->swr_ctx);
            //}
            const auto n_samples = len2 * format_desc_.audio_channels;
			const auto samples = reinterpret_cast<int32_t*>(buffer1_.data());
			++file_frame_number_;
			return std::make_shared<core::audio_buffer>(samples, samples + n_samples);
        } else {
			const auto n_samples = decoded_frame->nb_samples * format_desc_.audio_channels;
			const auto samples = reinterpret_cast<int32_t*>(decoded_frame->data[0]);
			++file_frame_number_;
			return std::make_shared<core::audio_buffer>(samples, samples + n_samples);
        }
	}

	bool ready() const
	{
		return packets_.size() > 10;
	}

	uint32_t nb_frames() const
	{
		return 0;//std::max<int64_t>(nb_frames_, file_frame_number_);
	}

	std::wstring print() const
	{		
		return L"[audio-decoder] " + widen(codec_context_->codec->long_name);
	}
};

audio_decoder::audio_decoder(const safe_ptr<AVFormatContext>& context, const core::video_format_desc& format_desc) : impl_(new implementation(context, format_desc)){}
void audio_decoder::push(const std::shared_ptr<AVPacket>& packet){impl_->push(packet);}
bool audio_decoder::ready() const{return impl_->ready();}
std::shared_ptr<core::audio_buffer> audio_decoder::poll(){return impl_->poll();}
uint32_t audio_decoder::nb_frames() const{return impl_->nb_frames();}
uint32_t audio_decoder::file_frame_number() const{return impl_->file_frame_number_;}
std::wstring audio_decoder::print() const{return impl_->print();}

}}