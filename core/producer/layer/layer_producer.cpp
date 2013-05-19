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

#include "../../stdafx.h"

#include "layer_producer.h"

#include "../../consumer/write_frame_consumer.h"
#include "../../consumer/output.h"
#include "../../video_channel.h"

#include "../stage.h"
#include "../frame/basic_frame.h"
#include "../frame/frame_factory.h"
#include "../../mixer/write_frame.h"
#include "../../mixer/read_frame.h"

#include <common/exception/exceptions.h>
#include <common/memory/memcpy.h>
#include <common/concurrency/future_util.h>

#include <tbb/concurrent_queue.h>

namespace caspar { namespace core {

class layer_consumer : public write_frame_consumer, frame_visitor
{	
	const safe_ptr<frame_factory>							frame_factory_;
	void*													tag_;
	tbb::concurrent_bounded_queue<safe_ptr<basic_frame>>	frame_buffer_;
	int														layer_index_;

public:
	layer_consumer(const safe_ptr<frame_factory>& frame_factory, void* tag) 
		: frame_factory_(frame_factory)
		, tag_(tag)
	{
		frame_buffer_.set_capacity(100);
	}

	~layer_consumer()
	{
	}

	// frame_visitor

	virtual void begin(basic_frame& frame)
	{
	}

	virtual void end()
	{
	}

	virtual void visit(write_frame& src_frame)
	{
		core::pixel_format_desc desc = src_frame.get_pixel_format_desc();
		auto frame = frame_factory_->create_frame(tag_, desc);
		auto src_image_data = src_frame.image_data();
		if (src_image_data.begin() != nullptr && src_image_data.size() > 0)
		{
			fast_memcpy(frame->image_data().begin(), src_frame.image_data().begin(), src_frame.image_data().size());
			frame->commit();
			frame_buffer_.try_push(frame);
		}
	}

	// frame_consumer

	virtual void send(const safe_ptr<basic_frame>& src_frame) override
	{
		// Do the copy ASAP then add to the frame buffer
		frame_buffer_.try_push(src_frame);
//		src_frame->accept(*this);
	}

	virtual std::wstring print() const override
	{
		return L"[layer_consumer|" + boost::lexical_cast<std::wstring>(layer_index_) + L"]";
	}

	safe_ptr<basic_frame> receive()
	{
		safe_ptr<basic_frame> frame;
		if (!frame_buffer_.try_pop(frame))
		{
			return basic_frame::late();
		}
		return frame;
	}
};

class layer_producer : public frame_producer
{
	const safe_ptr<frame_factory>			frame_factory_;
	const std::shared_ptr<layer_consumer>	consumer_;

	safe_ptr<basic_frame>					last_frame_;
	uint64_t								frame_number_;

public:
	explicit layer_producer(const safe_ptr<frame_factory>& frame_factory, const safe_ptr<stage>& stage, int layer) 
		: frame_factory_(frame_factory)
		, consumer_(new layer_consumer(frame_factory, this))
		, last_frame_(basic_frame::empty())
		, frame_number_(0)
	{
		stage->add_layer_consumer(layer, consumer_);
		CASPAR_LOG(info) << print() << L" Initialized";
	}

	~layer_producer()
	{
		CASPAR_LOG(info) << print() << L" Uninitialized";
	}

	// frame_producer
			
	virtual safe_ptr<basic_frame> receive(int) override
	{
		auto consumer_frame = consumer_->receive();
		/*if (consumer_frame == basic_frame::late())
		{
			return basic_frame::late();		
		}*/

		frame_number_++;

		return last_frame_ = consumer_frame;
	}	

	virtual safe_ptr<basic_frame> last_frame() const override
	{
		return last_frame_; 
	}	

	virtual std::wstring print() const override
	{
		return L"layer[]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"layer-producer");
		return info;
	}
};

safe_ptr<frame_producer> create_layer_producer(const safe_ptr<core::frame_factory>& frame_factory, const safe_ptr<stage>& stage, int layer)
{
	return create_producer_print_proxy(
		make_safe<layer_producer>(frame_factory, stage, layer)
	);
}

}}