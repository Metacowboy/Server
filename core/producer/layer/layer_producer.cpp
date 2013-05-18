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

class layer_consumer : public write_frame_consumer
{	
	tbb::concurrent_bounded_queue<std::shared_ptr<basic_frame>>	frame_buffer_;
	core::video_format_desc										format_desc_;
	int															channel_index_;
	tbb::atomic<bool>											is_running_;

public:
	layer_consumer() 
	{
		is_running_ = true;
		frame_buffer_.set_capacity(3);
	}

	~layer_consumer()
	{
		stop();
	}

	// frame_consumer

	virtual boost::unique_future<bool> send(const safe_ptr<basic_frame>& frame) override
	{
		frame_buffer_.try_push(frame);
		return caspar::wrap_as_future(is_running_.load());
	}

	virtual void initialize(const core::video_format_desc& format_desc, int channel_index) override
	{
		format_desc_    = format_desc;
		channel_index_  = channel_index;
	}

	virtual std::wstring print() const override
	{
		return L"[layer_consumer|" + boost::lexical_cast<std::wstring>(channel_index_) + L"]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"layer_consumer");
		info.add(L"channel-index", channel_index_);
		return info;
	}
	
	virtual bool has_synchronization_clock() const override
	{
		return false;
	}

	virtual size_t buffer_depth() const override
	{
		return 1;
	}

	virtual int index() const override
	{
		return 78500 + channel_index_;
	}

	// channel_consumer

	void stop()
	{
		is_running_ = false;
		frame_buffer_.try_push(basic_frame::empty());
	}
	
	const core::video_format_desc& get_video_format_desc()
	{
		return format_desc_;
	}

	std::shared_ptr<basic_frame> receive()
	{
		if(!is_running_)
			return basic_frame::empty();
		std::shared_ptr<basic_frame> frame;
		frame_buffer_.try_pop(frame);
		return frame;
	}
};

class frame_copy_visitor : public frame_visitor
{
public:

	std::shared_ptr<write_frame> destination_frame_;

	frame_copy_visitor(const std::shared_ptr<write_frame>& destination_frame, const std::shared_ptr<basic_frame>& src_frame)
		: destination_frame_(destination_frame)
	{
		src_frame->accept(*this);
	}

	virtual ~frame_copy_visitor()
	{
	}

	virtual void begin(basic_frame& frame)
	{
	}

	virtual void end()
	{
	}

	virtual void visit(write_frame& frame)
	{
		fast_memcpy(destination_frame_->image_data().begin(), frame.image_data().begin(), frame.image_data().size());
		destination_frame_->commit();
	}

};

class layer_producer : public frame_producer
{
	const safe_ptr<frame_factory>			frame_factory_;
	const std::shared_ptr<layer_consumer>	consumer_;

	std::queue<safe_ptr<basic_frame>>		frame_buffer_;
	safe_ptr<basic_frame>					last_frame_;
	uint64_t								frame_number_;

public:
	explicit layer_producer(const safe_ptr<frame_factory>& frame_factory, const safe_ptr<stage>& stage, int layer) 
		: frame_factory_(frame_factory)
		, consumer_(new layer_consumer())
		, last_frame_(basic_frame::empty())
		, frame_number_(0)
	{
		stage->add_layer_consumer(layer, consumer_);
		CASPAR_LOG(info) << print() << L" Initialized";
	}

	~layer_producer()
	{
		consumer_->stop();
		CASPAR_LOG(info) << print() << L" Uninitialized";
	}

	// frame_producer
			
	virtual safe_ptr<basic_frame> receive(int) override
	{
		auto format_desc = consumer_->get_video_format_desc();

		if(frame_buffer_.size() > 1)
		{
			auto frame = frame_buffer_.front();
			frame_buffer_.pop();
			return last_frame_ = frame;
		}
		
		auto consumer_frame = consumer_->receive();
		if(!consumer_frame)
			return basic_frame::late();		

		frame_number_++;
		
		core::pixel_format_desc desc;
		desc.pix_fmt = core::pixel_format::bgra;
		desc.planes.push_back(core::pixel_format_desc::plane(format_desc.width, format_desc.height, 4));
		auto frame = frame_factory_->create_frame(this, desc);

		frame_copy_visitor copier(frame, consumer_frame);

		frame_buffer_.push(frame);	
		
		return receive(0);
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