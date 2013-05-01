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

#include "file_util.h"

//#include "../tbb_avcodec.h"
//#include "../../ffmpeg_error.h"
//
//#include <tbb/concurrent_unordered_map.h>
//#include <tbb/concurrent_queue.h>
//
//#include <core/producer/frame/frame_transform.h>
//#include <core/producer/frame/frame_factory.h>
//#include <core/producer/frame_producer.h>
//#include <core/mixer/write_frame.h>
//
//#include <common/exception/exceptions.h>
//#include <common/utility/assert.h>
//#include <common/memory/memcpy.h>
//
//#include <tbb/parallel_for.h>
//
//#include <boost/filesystem.hpp>
//#include <boost/lexical_cast.hpp>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg2 {
		
bool is_valid_file(const std::wstring filename)
{			
	static const std::vector<std::wstring> invalid_exts = boost::assign::list_of(L".png")(L".tga")(L".bmp")(L".jpg")(L".jpeg")(L".gif")(L".tiff")(L".tif")(L".jp2")(L".jpx")(L".j2k")(L".j2c")(L".swf")(L".ct");
	static std::vector<std::wstring>	   valid_exts   = boost::assign::list_of(L".m2t")(L".mov")(L".mp4")(L".dv")(L".flv")(L".mpg")(L".wav")(L".mp3")(L".dnxhd")(L".h264")(L".prores");

	auto ext = boost::to_lower_copy(boost::filesystem::wpath(filename).extension());
		
	if(std::find(valid_exts.begin(), valid_exts.end(), ext) != valid_exts.end())
		return true;	
	
	if(std::find(invalid_exts.begin(), invalid_exts.end(), ext) != invalid_exts.end())
		return false;	

	auto filename2 = narrow(filename);

	if(boost::filesystem::path(filename2).extension() == ".m2t")
		return true;

	std::ifstream file(filename);

	std::vector<unsigned char> buf;
	for(auto file_it = std::istreambuf_iterator<char>(file); file_it != std::istreambuf_iterator<char>() && buf.size() < 2048; ++file_it)
		buf.push_back(*file_it);

	if(buf.empty())
		return nullptr;

	AVProbeData pb;
	pb.filename = filename2.c_str();
	pb.buf		= buf.data();
	pb.buf_size = buf.size();

	int score = 0;
	return av_probe_input_format2(&pb, true, &score) != nullptr;
}

std::wstring probe_stem(const std::wstring stem)
{
	auto stem2 = boost::filesystem2::wpath(stem);
	auto dir = stem2.parent_path();
	for(auto it = boost::filesystem2::wdirectory_iterator(dir); it != boost::filesystem2::wdirectory_iterator(); ++it)
	{
		if(boost::iequals(it->path().stem(), stem2.filename()) && is_valid_file(it->path().file_string()))
			return it->path().file_string();
	}
	return L"";
}

}}